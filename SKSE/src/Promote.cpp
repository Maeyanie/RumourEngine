#include "Promote.h"
#include "Api.h"
#include "Database.h"
#include "Ini.h"
#include "Rumours.h"
#include "Session.h"
#include "Settings.h"
#include "SkyrimNet.h"
#include "Text.h"

#include <nlohmann/json.hpp>

#include <chrono>

namespace RumourEngine::Promote {

    namespace {

        using json = nlohmann::json;
        using Clock = std::chrono::steady_clock;

        constexpr const char* kDecision = "rumourengine/lasting";
        constexpr const char* kPrompt = "rumour_settle";
        constexpr const char* kVariant = "rumour_settle";
        constexpr std::size_t kMaxInFlight = 2;  // decisions and settle calls together
        constexpr int         kMaxAttempts = 3;  // then the fallback: notability, or the furthest-travelled telling
        constexpr int         kMaxMakes = 3;     // failed entries, then not again this session

        std::mutex                                g_lock;
        std::set<std::int32_t>                    g_inFlight;
        std::map<std::int32_t, int>               g_failures;
        std::map<std::int32_t, Clock::time_point> g_retryAt;
        std::map<std::int32_t, int>               g_makeFailures;
        bool                                      g_noRoute = false;  // SkyrimNet has no decision provider
        unsigned                                  g_generation = 0;

        constexpr const char* kMonths[12] = { "Morning Star", "Sun's Dawn", "First Seed", "Rain's Hand", "Second Seed", "Midyear",
                                              "Sun's Height", "Last Seed", "Hearthfire", "Frostfall", "Sun's Dusk", "Evening Star" };
        constexpr int kMonthDays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

        /** "Evening Star of 4E 201": the month a game day fell in, counted back from today's date. */
        std::string MonthOf(double day) {
            auto* cal = RE::Calendar::GetSingleton();
            if (!cal) return {};
            int year = static_cast<int>(cal->GetYear());
            int month = std::clamp(static_cast<int>(cal->GetMonth()), 0, 11);
            int doy = std::max(0, static_cast<int>(cal->GetDay()) - 1);
            for (int m = 0; m < month; ++m) doy += kMonthDays[m];
            doy -= static_cast<int>(std::floor(cal->GetCurrentGameTime()) - std::floor(day));
            while (doy < 0) {
                --year;
                doy += 365;
            }
            while (doy >= 365) {
                ++year;
                doy -= 365;
            }
            int m = 0;
            while (m < 11 && doy >= kMonthDays[m]) doy -= kMonthDays[m++];
            return fmt::format("{} of 4E {}", kMonths[m], year);
        }

        /** "across Skyrim", or "in Whiterun Hold and the Reach". */
        std::string Where(const Rumours::Promotion& p) {
            if (p.holds.empty()) return {};
            if (p.allHolds && p.holds.size() + 1 >= p.allHolds) return "across Skyrim";
            if (p.holds.size() >= 4) return "across much of Skyrim";
            std::string out = "in ";
            for (std::size_t i = 0; i < p.holds.size(); ++i) {
                std::string h = p.holds[i];
                if (h.starts_with("The ")) h[0] = 't';
                out += (i == 0 ? "" : i + 1 == p.holds.size() ? " and " : ", ") + h;
            }
            return out;
        }

        /** "Old news from Evening Star of 4E 201, talked about across Skyrim: " */
        std::string Header(const Rumours::Promotion& p) {
            const auto when = MonthOf(p.created);
            const auto where = Where(p);
            return "Old news" + (when.empty() ? std::string{} : " from " + when) +
                   (where.empty() ? std::string{} : ", talked about " + where) + ": ";
        }

        /** The answer as one paragraph: no thinking, no quotes around it. */
        std::string CleanAnswer(std::string text) {
            if (auto end = text.rfind("</think>"); end != std::string::npos) text.erase(0, end + 8);
            std::string out;
            for (char c : text) {
                if (c == '\r') continue;
                if (c == '\n') c = ' ';
                if (c == ' ' && (out.empty() || out.back() == ' ')) continue;
                out += c;
            }
            out = Ini::Trim(out);
            for (std::string_view q : { "\"", "\xE2\x80\x9C", "\xE2\x80\x9D" }) {
                if (out.starts_with(q)) out.erase(0, q.size());
                if (out.ends_with(q)) out.erase(out.size() - q.size());
            }
            return Ini::Trim(out);
        }

        /** The entry's label on SkyrimNet's dashboard: "Rumour #57: The Legion stormed the..." */
        std::string Label(const Rumours::Promotion& p) {
            std::string gist = p.promotedText;
            if (auto colon = gist.find(": "); colon != std::string::npos) gist.erase(0, colon + 2);
            if (gist.size() > 60) {
                auto cut = gist.find_last_of(' ', 60);
                gist = gist.substr(0, cut == std::string::npos ? 60 : cut) + "...";
            }
            return fmt::format("Rumour #{}: {}", p.id, gist);
        }

        void Done(unsigned gen, std::int32_t id) {
            std::lock_guard lk(g_lock);
            if (gen != g_generation) return;
            g_inFlight.erase(id);
            g_failures.erase(id);
            g_retryAt.erase(id);
        }

        /** Another try shortly, or false once it has had its tries. */
        bool Retry(unsigned gen, std::int32_t id) {
            std::lock_guard lk(g_lock);
            if (gen != g_generation) return true;
            g_inFlight.erase(id);
            const int n = ++g_failures[id];
            if (n >= kMaxAttempts) {
                g_failures.erase(id);
                g_retryAt.erase(id);
                return false;
            }
            g_retryAt[id] = Clock::now() + std::chrono::seconds(30 * n);
            return true;
        }

        /** Without a decision model: the notability its caller or the harvest gave it decides. */
        void DecideByNotability(const Rumours::Promotion& p, const char* why) {
            const bool keep = p.notability >= Settings::PromoteNotability() - 1e-6;
            Rumours::Decide(p.id, keep ? 1.0 : 0.0, keep);
            logger::info("Promote: rumour #{} {} as world knowledge ({}, notability {:.2f})", p.id, keep ? "will be kept" : "is not kept",
                         why, p.notability);
        }

        void Ask(unsigned gen, const Rumours::Promotion& p) {
            const json ctx = { { "lasting_text", p.text } };
            const auto id = p.id;
            const bool queued = SkyrimNet::SendCustomDecision(kDecision, Text::Dump(ctx), [gen, p](std::string, std::string text, bool ok) {
                // A SkyrimNet worker thread: no RE:: here.
                const auto j = json::parse(text, nullptr, false);
                double     yes = -1.0;
                if (ok && j.is_object()) {
                    const auto answers = j.value("answers", json::object());
                    if (auto a = answers.find("lasting"); a != answers.end() && a->is_object() && a->contains("noul") &&
                        (*a)["noul"].is_number()) {
                        yes = (*a)["noul"].get<double>();
                    }
                }
                if (yes < 0.0) {
                    const bool noRoute = j.is_object() && j.value("error", "") == "no_decisions_route";
                    {
                        std::lock_guard lk(g_lock);
                        if (gen != g_generation) return;
                        if (noRoute) g_noRoute = true;
                    }
                    if (noRoute || !Retry(gen, p.id)) {
                        Done(gen, p.id);
                        DecideByNotability(p, noRoute ? "no decision provider" : "the decision model didn't answer");
                    } else {
                        logger::warn("Promote: no answer on rumour #{} ({}); asking again shortly", p.id, text.substr(0, 200));
                    }
                    return;
                }
                {
                    std::lock_guard lk(g_lock);
                    if (gen != g_generation) return;
                }
                const bool keep = yes >= Settings::PromoteLasting() - 1e-6;
                Rumours::Decide(p.id, yes, keep);
                Done(gen, p.id);
                logger::info("Promote: rumour #{} {} as world knowledge (will last {:.2f}): {}", p.id,
                             keep ? "will be kept" : "is not kept", yes, p.text.substr(0, 100));
            });
            if (!queued) {
                Done(gen, id);
                DecideByNotability(p, "SkyrimNet can't ask the decision model (needs API v12+)");
            }
        }

        void Send(unsigned gen, const Rumours::Promotion& p) {
            std::string versions;
            for (const auto& [place, text] : p.versions) versions += "- " + place + ": " + text + "\n";
            const json ctx = { { "settleText", p.text }, { "settleVersions", Ini::Trim(versions) } };
            const auto header = Header(p);
            // If the writer never answers: the telling that had travelled furthest, as people had it.
            const auto fallback = p.versions.empty() ? p.text : p.versions.front().second;
            const auto id = p.id;
            const bool queued = SkyrimNet::SendCustomPrompt(kPrompt, kVariant, Text::Dump(ctx), [gen, id, header, fallback](std::string text, bool ok) {
                // A SkyrimNet worker thread: no RE:: here.
                {
                    std::lock_guard lk(g_lock);
                    if (gen != g_generation) return;
                }
                auto account = ok ? CleanAnswer(text) : std::string{};
                if (account.empty()) {
                    if (Retry(gen, id)) {
                        logger::warn("Promote: rumour #{}'s settled account failed ({}); trying again shortly", id,
                                     ok ? "an empty answer" : text.substr(0, 200));
                        return;
                    }
                    logger::warn("Promote: rumour #{}'s settled account failed {} times; keeping its furthest-travelled telling", id,
                                 kMaxAttempts);
                    account = fallback;
                }
                Rumours::SetPromotionText(id, header + account);
                Done(gen, id);
            });
            if (!queued) {
                Rumours::SetPromotionText(id, header + fallback);
                Done(gen, id);
                logger::warn("Promote: SkyrimNet couldn't queue rumour #{}'s settled account (needs API v8+); keeping a telling", id);
                return;
            }
            logger::info("Promote: rumour #{}: writing what people came to know of it, from {} tellings", id, p.versions.size());
        }

        /** The world-knowledge entry, for those it had reached. Blocks while SkyrimNet embeds it. */
        void Make(const Rumours::Promotion& p) {
            {
                std::lock_guard lk(g_lock);
                if (g_makeFailures[p.id] >= kMaxMakes) return;
            }
            const auto  condition = fmt::format("contains(get_rumourengine_heard(actorUUID), \"|{}|\")", p.id);
            const float importance = static_cast<float>(std::clamp(0.8 * std::max(p.notability, p.lasting), 0.3, 0.8));
            const int   kid = SkyrimNet::AddWorldKnowledge(p.promotedText, condition, false, importance, Label(p));
            if (kid <= 0) {
                int n;
                {
                    std::lock_guard lk(g_lock);
                    n = ++g_makeFailures[p.id];
                }
                logger::warn("Promote: SkyrimNet refused rumour #{}'s world-knowledge entry{}", p.id,
                             n >= kMaxMakes ? "; not trying again until the next load" : "");
                return;
            }
            Rumours::SetKnowledgeId(p.id, kid);
            logger::info("Promote: rumour #{} is world knowledge now (entry {}): {}", p.id, kid, p.promotedText);
            Api::DispatchEvent("promoted", json{ { "type", "promoted" }, { "id", p.id }, { "source", p.source }, { "key", p.key },
                                                 { "knowledge_id", kid }, { "text", p.promotedText } }.dump());
        }
    }

    void Reset() {
        std::lock_guard lk(g_lock);
        ++g_generation;
        g_inFlight.clear();
        g_failures.clear();
        g_retryAt.clear();
        g_makeFailures.clear();
        g_noRoute = false;
    }

    void Pump() {
        if (!Session::IsReady()) return;
        for (auto kid : Rumours::TakeWithdrawn()) SkyrimNet::RemoveWorldKnowledge(kid);
        if (!Settings::PromoteEnabled()) {
            // Off: out of SkyrimNet's world knowledge, before an uninstall say. The texts stay.
            const auto ids = Rumours::TakeKnowledgeIds();
            std::size_t gone = 0;
            for (auto kid : ids) gone += SkyrimNet::RemoveWorldKnowledge(kid) ? 1 : 0;
            if (!ids.empty()) {
                logger::info("Promote: promotion is off, so {} world-knowledge entries were taken out ({} were gone already); "
                             "turning it on puts them back", ids.size(), ids.size() - gone);
            }
            return;
        }

        unsigned gen;
        bool     noRoute;
        {
            std::lock_guard lk(g_lock);
            gen = g_generation;
            noRoute = g_noRoute;
        }
        for (const auto& p : Rumours::PromotionsToMake()) {
            if (!p.promotedText.empty()) {
                Make(p);
                continue;
            }
            if (p.lasting < 0.0 && p.promote == "always") {
                Rumours::Decide(p.id, 1.0, true);  // its caller said so
                continue;
            }
            if (p.lasting < 0.0 && noRoute) {
                DecideByNotability(p, "no decision provider");
                continue;
            }
            {
                std::lock_guard lk(g_lock);
                if (gen != g_generation || g_inFlight.contains(p.id) || g_inFlight.size() >= kMaxInFlight) continue;
                if (auto r = g_retryAt.find(p.id); r != g_retryAt.end() && Clock::now() < r->second) continue;
                g_inFlight.insert(p.id);
            }
            if (p.lasting < 0.0) Ask(gen, p);
            else Send(gen, p);
        }
    }

    void TakeBack(double gameDays) {
        const auto ids = Database::TakeBackPromotions(gameDays);
        for (auto kid : ids) SkyrimNet::RemoveWorldKnowledge(kid);
        if (!ids.empty()) logger::info("Promote: took back {} world-knowledge entries made after day {:.2f}", ids.size(), gameDays);
    }
}
