#include "Retell.h"
#include "Ini.h"
#include "Mills.h"
#include "Rumours.h"
#include "Session.h"
#include "SkyrimNet.h"
#include "Text.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <fstream>
#include <regex>
#include <sstream>

namespace RumourEngine::Retell {

    namespace {

        constexpr const char* kPrompt = "rumour_retell";
        constexpr const char* kVariant = "rumour_retell";
        constexpr std::size_t kMaxSteps = 8;     // per call; a longer path takes another
        constexpr std::size_t kMaxInFlight = 4;  // calls at once, one per rumour
        constexpr int         kMaxAttempts = 3;  // then the step before is passed on unchanged

        using Clock = std::chrono::steady_clock;

        std::mutex                                     g_lock;
        std::set<std::pair<std::int32_t, std::string>> g_pending;
        std::set<std::int32_t>                         g_inFlight;
        std::map<std::int32_t, int>                    g_failures;
        std::map<std::int32_t, Clock::time_point>      g_retryAt;
        std::atomic<unsigned>                          g_generation = 0;

        std::mutex               g_placeLock;
        std::vector<std::string> g_playerMills;
        std::string              g_playerPlace;
        std::atomic<bool>        g_prefetch = false;

        // ---- Mill lines ----

        using Ini::Trim;

        /** A line from the mill's kind, for mills mills.ini doesn't describe. */
        std::string DefaultLine(const Mills::Mill& m, const Mills::MillSet& set) {
            const std::string& p = m.placeName.empty() ? m.id : m.placeName;
            std::string        hold;
            if (auto h = set.holdNames.find(m.hold); h != set.holdNames.end()) hold = h->second;
            switch (m.kind) {
            case Mills::Kind::Commons:
                return p + "'s townsfolk: the people who live and trade in " + p +
                       ". They care about their neighbours' business and whatever comes in from the road.";
            case Mills::Kind::Court:
                return p + "'s court: the Jarl's household, steward and staff. They care about the hold, the war, and what "
                           "anything means for the Jarl.";
            case Mills::Kind::Garrison:
                return p + "'s guards: they care about crime, fights and who's armed, and tell everything with a sneer.";
            case Mills::Kind::Underworld:
                return p + "'s underworld: thieves, fences and smugglers. They care about who's watching and who can be "
                           "squeezed.";
            case Mills::Kind::Enclave:
                return p + ": a closed household of its own. They hear little from outside and judge it by their own "
                           "concerns.";
            case Mills::Kind::Institution:
                return p + ": members spread across several towns, who carry talk between them.";
            default:
                return p + ": the few families of a small place" + (hold.empty() ? "" : " in " + hold) +
                       ". They care about their own neighbours and whoever passes through.";
            }
        }

        /** The group's name: the line up to its first colon or full stop. */
        std::string GroupName(const std::string& line) {
            const auto cut = line.find_first_of(":.");
            return Trim(cut == std::string::npos ? line : line.substr(0, cut));
        }

        // ---- The answer ----

        /** Strip quotes, asterisks and spaces from both ends, ASCII or curly. */
        std::string Clean(std::string s) {
            static const std::string_view kEnds[] = { "\"", "*", "\xE2\x80\x9C", "\xE2\x80\x9D", " ", "\t", "\r" };
            bool                          changed = true;
            while (changed && !s.empty()) {
                changed = false;
                for (auto e : kEnds) {
                    if (s.starts_with(e)) {
                        s.erase(0, e.size());
                        changed = true;
                    }
                    if (s.ends_with(e)) {
                        s.erase(s.size() - e.size());
                        changed = true;
                    }
                }
            }
            return s;
        }

        /** One telling per step: numbered lines, or failing that exactly one paragraph per step. */
        std::optional<std::vector<std::string>> ParseTellings(std::string text, std::size_t want) {
            // A reasoning model's thinking, if the variant lets it through.
            if (auto end = text.rfind("</think>"); end != std::string::npos) text.erase(0, end + 8);

            static const std::regex  kNumbered(R"(^\s*\**(\d+)[.):]\**\s*(.+?)\s*$)");
            std::map<std::size_t, std::string> got;
            std::istringstream       in(text);
            std::string              line;
            while (std::getline(in, line)) {
                std::smatch m;
                if (!std::regex_match(line, m, kNumbered)) continue;
                const auto n = static_cast<std::size_t>(std::stoul(m[1].str()));
                if (n >= 1 && n <= want && !got.contains(n)) got[n] = Clean(m[2].str());
            }
            bool complete = got.size() == want;
            for (auto& [n, t] : got) complete = complete && !t.empty();
            if (complete) {
                std::vector<std::string> out;
                for (std::size_t i = 1; i <= want; ++i) out.push_back(got[i]);
                return out;
            }
            // Some models drop the numbers but keep one paragraph per step.
            std::vector<std::string> paras;
            std::string              para;
            std::istringstream       in2(text);
            auto                     flush = [&] {
                auto t = Clean(Trim(para));
                if (!t.empty()) paras.push_back(t);
                para.clear();
            };
            while (std::getline(in2, line)) {
                if (Trim(line).empty()) flush();
                else para += (para.empty() ? "" : " ") + Trim(line);
            }
            flush();
            if (paras.size() == want) return paras;
            return std::nullopt;
        }

        // ---- Calls ----

        void Done(unsigned gen, std::int32_t id) {
            std::lock_guard lk(g_lock);
            if (gen == g_generation) g_inFlight.erase(id);
        }

        void Failed(unsigned gen, const Rumours::Chain& c, const std::string& why) {
            bool giveUp = false;
            {
                std::lock_guard lk(g_lock);
                if (gen != g_generation) return;
                const int n = ++g_failures[c.id];
                giveUp = n >= kMaxAttempts;
                if (giveUp) {
                    g_failures.erase(c.id);
                    g_retryAt.erase(c.id);
                } else {
                    g_retryAt[c.id] = Clock::now() + std::chrono::seconds(30 * n);
                    g_pending.insert({ c.id, c.steps.back() });
                }
                g_inFlight.erase(c.id);
            }
            if (!giveUp) {
                logger::warn("Retell: rumour #{} for {} failed ({}); trying again shortly", c.id, c.steps.back(), why);
                return;
            }
            // Nobody should go without the talk for want of a model: pass the last version on as it was.
            logger::warn("Retell: rumour #{} for {} failed {} times ({}); passing on the version before unchanged", c.id,
                         c.steps.back(), kMaxAttempts, why);
            std::vector<std::pair<std::string, std::string>> texts;
            for (const auto& s : c.steps) texts.emplace_back(s, c.fromText);
            Rumours::SetVersions(c.id, texts, "copied");
        }

        void Send(unsigned gen, Rumours::Chain c) {
            // Laid out for prompt caching: the template's instructions never change, the
            // groups are shared within a burst to one settlement, and the talk comes last.
            std::vector<std::string> groups;
            std::vector<std::string> lines;
            auto                     addGroup = [&](const std::string& mill) {
                if (mill.empty() || std::find(groups.begin(), groups.end(), mill) != groups.end()) return;
                groups.push_back(mill);
                lines.push_back(LineOf(mill));
            };
            addGroup(c.from);
            for (const auto& s : c.steps) addGroup(s);

            std::string groupText, stepText;
            for (const auto& l : lines) groupText += "- " + l + "\n";
            for (std::size_t i = 0; i < c.steps.size(); ++i) {
                stepText += fmt::format("{}. {}\n", i + 1, GroupName(LineOf(c.steps[i])));
            }
            const nlohmann::json ctx = {
                { "retellGroups", Trim(groupText) },
                { "retellFrom", c.from.empty() ? std::string("the people where it started") : GroupName(LineOf(c.from)) },
                { "retellTalk", c.fromText },
                { "retellFirst", c.from.empty() },
                { "retellSteps", Trim(stepText) },
                { "retellCount", c.steps.size() },
            };

            const bool queued = SkyrimNet::SendCustomPrompt(kPrompt, kVariant, Text::Dump(ctx), [gen, c](std::string text, bool ok) {
                // A SkyrimNet worker thread: no RE:: here.
                if (gen != g_generation) return;
                if (!ok) {
                    Failed(gen, c, text.empty() ? "no answer" : text.substr(0, 200));
                    return;
                }
                auto tellings = ParseTellings(text, c.steps.size());
                if (!tellings) {
                    Failed(gen, c, "couldn't read " + std::to_string(c.steps.size()) + " tellings in: " + text.substr(0, 300));
                    return;
                }
                std::vector<std::pair<std::string, std::string>> texts;
                for (std::size_t i = 0; i < c.steps.size(); ++i) texts.emplace_back(c.steps[i], (*tellings)[i]);
                {
                    std::lock_guard lk(g_lock);
                    g_failures.erase(c.id);
                    g_retryAt.erase(c.id);
                }
                Rumours::SetVersions(c.id, texts, kVariant);
                Done(gen, c.id);
            });
            if (!queued) {
                Failed(gen, c, "SkyrimNet couldn't queue the call (needs API v8+)");
                return;
            }
            std::string path = c.from.empty() ? "(first telling)" : c.from;
            for (const auto& s : c.steps) path += " -> " + s;
            logger::info("Retell: rumour #{}: {}", c.id, path);
        }

        // ---- The player's whereabouts ----

        class LocationSink final : public RE::BSTEventSink<RE::TESActorLocationChangeEvent> {
        public:
            RE::BSEventNotifyControl ProcessEvent(const RE::TESActorLocationChangeEvent* e,
                                                  RE::BSTEventSource<RE::TESActorLocationChangeEvent>*) override {
                if (e && e->actor && e->actor->IsPlayerRef()) PlayerMoved(e->newLoc);
                return RE::BSEventNotifyControl::kContinue;
            }
        };
    }

    void Install() {
        static LocationSink sink;
        if (auto* events = RE::ScriptEventSourceHolder::GetSingleton()) {
            events->AddEventSink<RE::TESActorLocationChangeEvent>(&sink);
        }
    }

    void Reset() {
        std::lock_guard lk(g_lock);
        ++g_generation;
        g_pending.clear();
        g_inFlight.clear();
        g_failures.clear();
        g_retryAt.clear();
    }

    void PlayerMoved(RE::BGSLocation* location) {
        auto set = Mills::Current();
        if (!set) return;
        auto        mills = location ? set->PlaceMillsAt(location->GetFormID()) : std::vector<std::string>{};
        std::string place;
        if (!mills.empty()) {
            if (const auto* m = set->Find(mills.front())) place = m->placeName;
        }
        {
            std::lock_guard lk(g_placeLock);
            if (place == g_playerPlace && mills == g_playerMills) return;
            g_playerMills = std::move(mills);
            g_playerPlace = place;
        }
        if (!place.empty()) {
            logger::info("Retell: the player is in {}", place);
            g_prefetch = true;
        }
    }

    bool InPlayersPlace(const std::string& mill) {
        std::lock_guard lk(g_placeLock);
        return std::find(g_playerMills.begin(), g_playerMills.end(), mill) != g_playerMills.end();
    }

    void Request(std::int32_t id, const std::string& mill) {
        std::lock_guard lk(g_lock);
        g_pending.insert({ id, mill });
    }

    void Pump() {
        if (!Session::IsReady()) return;
        if (g_prefetch.exchange(false)) {
            std::vector<std::string> mills;
            {
                std::lock_guard lk(g_placeLock);
                mills = g_playerMills;
            }
            for (const auto& [id, mill] : Rumours::Unwritten(mills, Rumours::Now())) Request(id, mill);
        }
        // Mills another mod reads out in its own prompts are written as soon as a rumour gets there,
        // however it got there: a hop, a reach, a catch-up after a load.
        if (auto set = Mills::Current(); set && !set->eager.empty()) {
            for (const auto& [id, mill] : Rumours::Unwritten(set->eager, Rumours::Now())) Request(id, mill);
        }

        unsigned                                          gen;
        std::vector<std::pair<std::int32_t, std::string>> work;
        {
            std::lock_guard lk(g_lock);
            if (g_pending.empty() || g_inFlight.size() >= kMaxInFlight) return;
            gen = g_generation;
            const auto now = Clock::now();
            for (const auto& p : g_pending) {
                if (g_inFlight.contains(p.first)) continue;
                if (auto r = g_retryAt.find(p.first); r != g_retryAt.end() && now < r->second) continue;
                work.push_back(p);
            }
        }

        // One call per rumour at a time, along the longest path asked for: it usually
        // covers the others (a town's garrison hears it from its commons).
        std::map<std::int32_t, Rumours::Chain> best;
        std::vector<std::pair<std::int32_t, std::string>> done;
        for (const auto& [id, mill] : work) {
            auto chain = Rumours::ChainTo(id, mill, kMaxSteps);
            if (!chain) {
                done.emplace_back(id, mill);  // written meanwhile, or gone
                continue;
            }
            auto& b = best[id];
            if (chain->steps.size() > b.steps.size()) b = std::move(*chain);
        }

        std::vector<Rumours::Chain> start;
        {
            std::lock_guard lk(g_lock);
            if (gen != g_generation) return;
            for (const auto& d : done) g_pending.erase(d);
            for (auto& [id, chain] : best) {
                if (g_inFlight.size() >= kMaxInFlight) break;
                g_inFlight.insert(id);
                for (const auto& s : chain.steps) g_pending.erase({ id, s });
                start.push_back(std::move(chain));
            }
        }
        for (auto& c : start) Send(gen, std::move(c));
    }

    std::string LineOf(const std::string& mill) {
        auto set = Mills::Current();
        if (!set) return mill;
        if (auto l = set->lines.find(mill); l != set->lines.end()) return l->second;
        const auto* m = set->Find(mill);
        return m ? DefaultLine(*m, *set) : mill;
    }
}
