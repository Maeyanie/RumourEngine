#include "Harvest.h"
#include "Database.h"
#include "Delivery.h"
#include "Mills.h"
#include "Placements.h"
#include "Retell.h"
#include "Rumours.h"
#include "Session.h"
#include "Settings.h"
#include "SkyrimNet.h"
#include "Text.h"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <chrono>
#include <regex>
#include <sstream>
#include <unordered_set>

namespace RumourEngine::Harvest {

    namespace {

        using json = nlohmann::json;
        using Clock = std::chrono::steady_clock;

        constexpr const char* kScreenTemplate = "rumourengine/screen_memories";
        constexpr const char* kScreenPrompt = "rumour_screen";  // the chat fallback, and its variant
        constexpr const char* kSeedPrompt = "rumour_seed";      // the prompt, and its variant
        constexpr int         kEngagementMax = 100;
        constexpr std::size_t kMaxKnown = 6;     // live rumours offered as "the same talk"
        constexpr std::size_t kMaxSubjects = 8;  // people a seed may say it is about
        constexpr int         kMaxAttempts = 3;  // failed screenings or seeds of a memory, then it is dropped
        constexpr RE::FormID  kPlayer = 0x14;
        // A memory RumourEngine wrote (someone remembering they told it), never harvested back.
        constexpr const char* kOwnTag = "rumourengine";
        constexpr float       kToldImportance = 0.5f;  // level with what it competes with, below the event itself
        // One person's memories this close together are one happening: a long fight leaves several,
        // with no event of SkyrimNet's in common.
        constexpr double kSameStretchDays = 2.0 / 24.0;

        struct Memory {
            std::int64_t               id = 0;
            std::string                text;
            double                     importance = 0.0;
            std::vector<std::int64_t>  events;   // SkyrimNet's related_event_ids: shared by every witness
            std::vector<std::uint64_t> related;  // UUIDs of the others it involves
            double                     day = -1.0;  // when it was formed; below 0 if SkyrimNet didn't say
            // A catch-up's other memories from the same stretch of time, which go where this one does.
            std::vector<std::pair<std::int64_t, double>> stretchIds;
            std::vector<std::string>                     stretchTexts;

            bool Near(double d) const {
                if (d < 0.0 || day < 0.0) return false;
                if (std::abs(d - day) <= kSameStretchDays) return true;
                for (const auto& [id, t] : stretchIds) {
                    if (std::abs(d - t) <= kSameStretchDays) return true;
                }
                return false;
            }
        };

        /** Someone with new memories, and what screening needs to know of them. */
        struct Person {
            RE::FormID                                        form = 0;
            std::uint64_t                                     uuid = 0;
            std::string                                       name;
            std::vector<std::string>                          mills;  // home first
            double                                            gossip = 0.0;
            std::vector<Memory>                               memories;  // m1, m2, ...
            std::vector<std::pair<std::int32_t, std::string>> known;     // r1, r2, ...: talk they could mean
            bool                                              back = false;  // a dismissed follower's time away
        };

        struct Verdict {
            double      spread = -1.0;  // p(would pass it on); below 0 when unanswered
            bool        everything = false;  // would tell the private details too; else only the gist
            double      interest = 0.0;  // 1 to 5, the expected level
            std::string same = "new";   // new, r<i> or m<j>
        };

        /** A memory screening accepted, waiting to be written as a rumour. */
        struct Accepted {
            Person                    who;  // its memories and known talk left empty
            Memory                    m;
            double                    interest = 0.0;
            double                    spread = 0.0;
            bool                      everything = false;
            std::vector<std::int64_t> alsoIds;  // other witnesses' memories of the same happening, folded in
            std::vector<std::pair<std::int64_t, double>> ownIds;     // their own other memories of it, and when
            std::vector<std::string>                     alsoTexts;  // those memories, for the writer
            std::vector<std::pair<RE::FormID, std::vector<std::string>>> witnesses;  // others who saw it, and their mills
            int                       attempts = 0;
        };

        struct ScreenResult {
            unsigned                                      gen = 0;
            Person                                        who;
            std::optional<std::map<std::string, Verdict>> verdicts;  // by memory key; empty when it failed
            std::string                                   error;
            bool                                          noRoute = false;
        };

        struct SeedResult {
            unsigned                             gen = 0;
            std::vector<Accepted>                batch;
            std::vector<std::vector<RE::FormID>> subjects;  // per memory: s1, s2, ...
            std::optional<std::string>           text;
            std::string                          error;
        };

        std::mutex                                     g_lock;
        std::atomic<unsigned>                          g_generation = 0;
        std::unordered_set<std::int64_t>               g_screened;     // memory ids, and those waiting to be seeded
        std::unordered_map<std::int64_t, std::int32_t> g_eventRumour;  // SkyrimNet event id -> the rumour it became
        // Whose memories became which rumour, and when each was formed: for kSameStretchDays.
        std::unordered_map<std::uint64_t, std::vector<std::pair<double, std::int32_t>>> g_ownerRumours;
        std::map<std::int64_t, int>                    g_failures;
        double                                         g_lastSweep = -1.0;
        int                                            g_screening = 0;  // screening calls out
        bool                                           g_seeding = false;
        bool                                           g_noRoute = false;  // no decision provider: screen by chat
        Clock::time_point                              g_seedRetryAt{};
        std::vector<Accepted>                          g_accepted;
        std::vector<ScreenResult>                      g_screenResults;
        std::vector<SeedResult>                        g_seedResults;
        bool                                           g_rowLogged = false;
        bool                                           g_engagementLogged = false;
        bool                                           g_decisionLogged = false;

        // ---- Small things ----

        std::string OneLine(std::string_view s) {
            std::string out;
            bool        space = false;
            for (char c : s) {
                if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
                    space = !out.empty();
                    continue;
                }
                if (space) out += ' ';
                space = false;
                out += c;
            }
            return out;
        }

        std::string Trim(std::string_view s) {
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
            return std::string(s);
        }

        std::string Lower(std::string s) {
            for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        std::string Key(char c, std::size_t i) { return std::string(1, c) + std::to_string(i + 1); }

        /** Index from a key like "m3" or "r2", or nullopt. */
        std::optional<std::size_t> IndexOf(const std::string& key, char c) {
            if (key.size() < 2 || std::tolower(static_cast<unsigned char>(key[0])) != c) return std::nullopt;
            std::size_t n = 0;
            for (std::size_t i = 1; i < key.size(); ++i) {
                if (!std::isdigit(static_cast<unsigned char>(key[i]))) return std::nullopt;
                n = n * 10 + (key[i] - '0');
            }
            if (n == 0) return std::nullopt;
            return n - 1;
        }

        std::string NameOf(RE::FormID form) {
            auto* f = form ? RE::TESForm::LookupByID(form) : nullptr;
            if (!f) return "";
            if (auto* a = f->As<RE::Actor>()) {
                return Text::FromGame(a->GetDisplayFullName());
            }
            if (auto* npc = f->As<RE::TESNPC>()) {
                return Text::FromGame(npc->GetName());
            }
            return "";
        }

        /** How much of a gossip, in words, for the screening prompt. */
        std::string GossipWords(double g) {
            if (g < 0.3) return "Keeps to themselves, and rarely passes anything on.";
            if (g < 0.5) return "Will pass on news when it comes up.";
            if (g < 0.7) return "Enjoys a bit of gossip.";
            return "An incorrigible gossip, who passes on everything.";
        }

        double GossipOf(const Mills::MillSet& set, RE::FormID form) {
            auto* f = RE::TESForm::LookupByID(form);
            if (auto* a = f ? f->As<RE::Actor>() : nullptr) return Delivery::GossipScore(a);
            if (auto p = set.persons.find(form); p != set.persons.end()) return Delivery::GossipScoreOf(p->second);
            return 0.5;
        }

        /** Notability from how much a room would care: 3 is town talk, 5 gets round its hold and
         *  sometimes further. Big news, a march on a town, comes from callers, not from memories. */
        double NotabilityOf(double interest) { return std::clamp(0.05 + 0.1 * interest, 0.0, 1.0); }

        template <class T>
        std::vector<T> Numbers(const json& row, const char* field) {
            std::vector<T> out;
            auto           it = row.find(field);
            if (it == row.end()) return out;
            const json* arr = &*it;
            json        parsed;
            if (it->is_string()) {  // some builds hand JSON columns back as text
                parsed = json::parse(it->get<std::string>(), nullptr, false);
                arr = &parsed;
            }
            if (!arr->is_array()) return out;
            for (const auto& v : *arr) {
                if (v.is_number_unsigned()) out.push_back(static_cast<T>(v.get<std::uint64_t>()));
                else if (v.is_number_integer()) out.push_back(static_cast<T>(v.get<std::int64_t>()));
                else if (v.is_string()) {
                    try {
                        out.push_back(static_cast<T>(std::stoull(v.get<std::string>())));
                    } catch (...) {
                    }
                }
            }
            return out;
        }

        // ---- The database ----

        void MarkScreened(std::int64_t memory, const char* verdict, std::int32_t rumour, std::uint64_t actor,
                          const std::vector<std::int64_t>& events, const std::string& detail, double now, double memoryDay = -1.0) {
            Database::With([&](sqlite3* db) {
                sqlite3_stmt* st = nullptr;
                if (sqlite3_prepare_v2(db,
                                       "INSERT OR REPLACE INTO screened(memory_id, verdict, rumour_id, actor, events, detail, game_time, "
                                       "memory_day) VALUES(?,?,?,?,?,?,?,?);",
                                       -1, &st, nullptr) != SQLITE_OK) {
                    logger::error("Harvest: {}", sqlite3_errmsg(db));
                    return;
                }
                const auto eventsJson = Text::Dump(json(events));
                const auto actorText = actor ? std::to_string(actor) : std::string{};
                sqlite3_bind_int64(st, 1, memory);
                sqlite3_bind_text(st, 2, verdict, -1, SQLITE_TRANSIENT);
                if (rumour) sqlite3_bind_int(st, 3, rumour);
                else sqlite3_bind_null(st, 3);
                sqlite3_bind_text(st, 4, actorText.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(st, 5, eventsJson.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(st, 6, detail.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_double(st, 7, now);
                if (memoryDay >= 0.0) sqlite3_bind_double(st, 8, memoryDay);
                else sqlite3_bind_null(st, 8);
                if (sqlite3_step(st) != SQLITE_DONE) logger::error("Harvest: {}", sqlite3_errmsg(db));
                sqlite3_finalize(st);
            });
            std::lock_guard lk(g_lock);
            g_screened.insert(memory);
            g_failures.erase(memory);
            if (rumour) {
                for (auto e : events) g_eventRumour[e] = rumour;
                if (actor && memoryDay >= 0.0) g_ownerRumours[actor].push_back({ memoryDay, rumour });
            }
        }

        /** The memories folded into an accepted one: other witnesses' and the person's own. */
        void MarkFolded(const Accepted& a, const char* verdict, std::int32_t rumour, const std::string& detail, double now) {
            for (auto id : a.alsoIds) MarkScreened(id, verdict, rumour, 0, {}, detail, now);
            for (const auto& [id, day] : a.ownIds) MarkScreened(id, verdict, rumour, a.who.uuid, {}, detail, now, day);
        }

        /** A screened memory, and the others from its stretch of time that came with it. */
        void MarkWithStretch(const Memory& m, const char* verdict, std::int32_t rumour, std::uint64_t actor, const std::string& detail,
                             double now) {
            MarkScreened(m.id, verdict, rumour, actor, m.events, detail, now, m.day);
            for (const auto& [id, day] : m.stretchIds) MarkScreened(id, verdict, rumour, actor, {}, detail, now, day);
        }

        /** A failed screening or seeding: try again next sweep, or give up after a few. */
        bool Failed(std::int64_t memory, std::uint64_t actor, const std::string& why, double now) {
            bool giveUp;
            {
                std::lock_guard lk(g_lock);
                giveUp = ++g_failures[memory] >= kMaxAttempts;
                if (!giveUp) g_screened.erase(memory);
            }
            if (giveUp) MarkScreened(memory, "failed", 0, actor, {}, why, now);
            return giveUp;
        }

        // ---- Answers ----

        std::map<std::string, Verdict> ParseDecision(const json& j, std::size_t count) {
            std::map<std::string, Verdict> out;
            const auto                     answers = j.value("answers", json::object());
            for (std::size_t i = 0; i < count; ++i) {
                const auto key = Key('m', i);
                Verdict    v;
                if (auto a = answers.find("spread_" + key); a != answers.end() && a->is_object()) {
                    if (a->contains("noul") && (*a)["noul"].is_number()) v.spread = (*a)["noul"].get<double>();
                    else if (a->contains("choice") && (*a)["choice"].is_string()) v.spread = (*a)["choice"] == "true" ? 1.0 : 0.0;
                }
                if (auto a = answers.find("interest_" + key); a != answers.end() && a->is_object()) {
                    // A score is the expected level counted from 0 (probabilities over "0".."4"), so the
                    // template's "1".."5" lines come back one lower. Kept unrounded: "mostly a 2, some
                    // 3s" (2.75) should fall short of a bar of 3, not round up to it.
                    if (a->contains("score") && (*a)["score"].is_number()) v.interest = (*a)["score"].get<double>() + 1.0;
                    else if (a->contains("choice") && (*a)["choice"].is_string()) {
                        try {
                            v.interest = std::stod((*a)["choice"].get<std::string>());
                        } catch (...) {
                        }
                    }
                }
                if (auto a = answers.find("same_" + key); a != answers.end() && a->is_object() && a->contains("choice") &&
                                                          (*a)["choice"].is_string()) {
                    v.same = (*a)["choice"].get<std::string>();
                }
                // Unanswered, the discreet reading: the gist.
                if (auto a = answers.find("tell_" + key); a != answers.end() && a->is_object() && a->contains("choice") &&
                                                          (*a)["choice"].is_string()) {
                    v.everything = (*a)["choice"] == "everything";
                }
                v.interest = std::clamp(v.interest, 0.0, 5.0);
                out[key] = v;
            }
            return out;
        }

        /** The chat fallback's answer: one line per memory, "m1: spread yes; tell gist; interest 4; same new". */
        std::map<std::string, Verdict> ParseScreenChat(std::string text) {
            if (auto t = text.rfind("</think>"); t != std::string::npos) text = text.substr(t + 8);
            static const std::regex line(R"(^\W*m\s*(\d+)\b(.*)$)", std::regex::icase);
            static const std::regex spread(R"(spread\W+(yes|no))", std::regex::icase);
            static const std::regex tell(R"(tell\W+(everything|gist))", std::regex::icase);
            static const std::regex interest(R"(interest\W+([1-5]))", std::regex::icase);
            static const std::regex same(R"(same\W+(new|[rm]\d+))", std::regex::icase);
            std::map<std::string, Verdict> out;
            std::istringstream             in(text);
            std::string                    l;
            std::smatch                    m, x;
            while (std::getline(in, l)) {
                if (!std::regex_match(l, m, line)) continue;
                Verdict    v;
                const auto rest = m[2].str();
                if (std::regex_search(rest, x, spread)) v.spread = (x[1].str()[0] == 'y' || x[1].str()[0] == 'Y') ? 1.0 : 0.0;
                if (std::regex_search(rest, x, tell)) v.everything = x[1].str()[0] == 'e' || x[1].str()[0] == 'E';
                if (std::regex_search(rest, x, interest)) v.interest = std::stoi(x[1].str());
                if (std::regex_search(rest, x, same)) {
                    v.same = x[1].str();
                    for (auto& c : v.same) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                out["m" + m[1].str()] = v;
            }
            return out;
        }

        struct Seed {
            bool                     skip = false;
            std::string              telling;
            std::vector<std::size_t> about;  // indexes into that memory's people
            std::vector<std::string> topics;
        };

        /** The seed writer's answer: a block per memory, "m1" then telling / about / topics lines, or skip. */
        std::map<std::size_t, Seed> ParseSeeds(std::string text) {
            if (auto t = text.rfind("</think>"); t != std::string::npos) text = text.substr(t + 8);
            static const std::regex head(R"(^\W*m\s*(\d+)\b\W*(.*)$)", std::regex::icase);
            static const std::regex field(R"(^\W*(telling|about|topics)\W*:\s*(.*)$)", std::regex::icase);
            static const std::regex skipWord(R"(^\W*skip\b)", std::regex::icase);
            static const std::regex sKey(R"(\bs\s*(\d+)\b)", std::regex::icase);
            static const std::regex fieldLine(R"(^\W*(telling|about|topics)\b.*)", std::regex::icase);
            std::map<std::size_t, Seed> out;
            std::optional<std::size_t>  cur;
            std::string                 last;
            std::istringstream          in(text);
            std::string                 l;
            std::smatch                 m;
            auto                        apply = [&](const std::string& name, const std::string& value) {
                auto& s = out[*cur];
                if (name == "telling") {
                    s.telling = Trim(value);
                } else if (name == "about") {
                    for (std::sregex_iterator it(value.begin(), value.end(), sKey), end; it != end; ++it) {
                        const auto n = std::stoul((*it)[1].str());
                        if (n > 0) s.about.push_back(n - 1);
                    }
                } else if (name == "topics") {
                    std::istringstream parts(value);
                    std::string        t;
                    while (std::getline(parts, t, ',')) {
                        t = Trim(t);
                        for (auto& c : t) c = static_cast<char>(c == ' ' ? '_' : std::tolower(static_cast<unsigned char>(c)));
                        if (!t.empty() && t.size() < 24) s.topics.push_back(t);
                    }
                }
                last = name;
            };
            while (std::getline(in, l)) {
                if (Trim(l).empty()) continue;
                if (!std::regex_match(l, fieldLine) && std::regex_match(l, m, head)) {
                    cur = std::stoul(m[1].str());
                    last.clear();
                    out[*cur];
                    const auto rest = m[2].str();
                    std::smatch f;
                    if (std::regex_search(rest, skipWord)) out[*cur].skip = true;
                    else if (std::regex_match(rest, f, field)) apply(Lower(f[1].str()), f[2].str());
                    continue;
                }
                if (!cur) continue;
                if (std::regex_match(l, m, field)) {
                    apply(Lower(m[1].str()), m[2].str());
                } else if (std::regex_search(l, skipWord) && out[*cur].telling.empty()) {
                    out[*cur].skip = true;
                } else if (last == "telling") {
                    out[*cur].telling += " " + Trim(l);  // a telling that ran onto another line
                }
            }
            for (auto& [k, s] : out) {
                auto& t = s.telling;
                while (!t.empty() && (t.front() == '"' || t.front() == '\'' || t.front() == '*')) t.erase(t.begin());
                while (!t.empty() && (t.back() == '"' || t.back() == '\'' || t.back() == '*')) t.pop_back();
                t = Trim(t);
            }
            return out;
        }

        // ---- Screening ----

        json ScreenContext(const Person& p) {
            json mems = json::array();
            for (std::size_t i = 0; i < p.memories.size(); ++i) {
                json earlier = json::array();
                for (std::size_t k = 0; k < i; ++k) {
                    earlier.push_back({ { "key", Key('m', k) }, { "content", OneLine(p.memories[k].text).substr(0, 160) } });
                }
                mems.push_back({ { "key", Key('m', i) }, { "content", OneLine(p.memories[i].text) }, { "earlier", earlier } });
            }
            json rumours = json::array();
            for (std::size_t i = 0; i < p.known.size(); ++i) {
                rumours.push_back({ { "key", Key('r', i) }, { "content", OneLine(p.known[i].second) } });
            }
            return json{ { "screen_npc", p.uuid },
                         { "screen_name", p.name },
                         { "screen_gossip", GossipWords(p.gossip) },
                         { "screen_group", Retell::LineOf(p.mills.front()) },
                         { "screen_memories", mems },
                         { "screen_rumours", rumours },
                         { "screen_back", p.back },
                         { "screen_count", p.memories.size() } };
        }

        void Post(ScreenResult r) {
            std::lock_guard lk(g_lock);
            g_screenResults.push_back(std::move(r));
        }

        void ScreenByChat(unsigned gen, const Person& p) {
            {
                std::lock_guard lk(g_lock);
                ++g_screening;
            }
            const bool queued = SkyrimNet::SendCustomPrompt(kScreenPrompt, kScreenPrompt, Text::Dump(ScreenContext(p)),
                                                            [gen, p](std::string text, bool ok) {
                                                                ScreenResult r{ gen, p };
                                                                if (ok) r.verdicts = ParseScreenChat(text);
                                                                else r.error = text.substr(0, 200);
                                                                Post(std::move(r));
                                                            });
            if (!queued) Post(ScreenResult{ gen, p, std::nullopt, "SkyrimNet couldn't queue the chat screening" });
        }

        void Screen(unsigned gen, const Person& p) {
            bool chat;
            {
                std::lock_guard lk(g_lock);
                chat = g_noRoute;
            }
            if (chat) {
                ScreenByChat(gen, p);
                return;
            }
            {
                std::lock_guard lk(g_lock);
                ++g_screening;
            }
            const bool queued = SkyrimNet::SendCustomDecision(kScreenTemplate, Text::Dump(ScreenContext(p)),
                                                              [gen, p](std::string, std::string text, bool ok) {
                                                                  ScreenResult r{ gen, p };
                                                                  const auto   j = json::parse(text, nullptr, false);
                                                                  if (ok && j.is_object()) {
                                                                      r.verdicts = ParseDecision(j, p.memories.size());
                                                                      bool first;
                                                                      {
                                                                          std::lock_guard lk(g_lock);
                                                                          first = !std::exchange(g_decisionLogged, true);
                                                                      }
                                                                      if (first) logger::info("Harvest: first screening answer, as given: {}", text.substr(0, 2000));
                                                                  } else {
                                                                      r.error = j.is_object() ? j.value("error", "") + ": " + j.value("message", "") : text.substr(0, 200);
                                                                      r.noRoute = j.is_object() && j.value("error", "") == "no_decisions_route";
                                                                  }
                                                                  Post(std::move(r));
                                                              });
            if (!queued) Post(ScreenResult{ gen, p, std::nullopt, "SkyrimNet couldn't queue the decision (needs API v12)" });
        }

        /** Act on one person's answers: reinforce, reject, or accept for seeding. */
        void Judge(const Person& p, const std::map<std::string, Verdict>& verdicts, double now) {
            const double threshold = std::max(0.05, Settings::ScreenThreshold() - Settings::GossipThresholdBonus() * p.gossip);
            const int    minInterest = Settings::MinInterest();
            std::map<std::string, std::size_t> acceptedAt;  // memory key -> index in g_accepted

            for (std::size_t i = 0; i < p.memories.size(); ++i) {
                const auto& m = p.memories[i];
                const auto  key = Key('m', i);
                auto        it = verdicts.find(key);
                if (it == verdicts.end() || it->second.spread < 0.0) {
                    Failed(m.id, p.uuid, "no answer for " + key, now);
                    continue;
                }
                const auto&       v = it->second;
                const std::string detail = fmt::format("spread {:.2f} (needs {:.2f}), tell {}, interest {:.2f} (needs {}), same {}", v.spread,
                                                       threshold, v.everything ? "everything" : "gist", v.interest, minInterest, v.same);
                const std::string head = fmt::format("Harvest: {} {} (memory {}, importance {:.2f})", p.name, key, m.id, m.importance);

                // Talk already going round: this is it reaching them, or reaching them again.
                if (auto r = IndexOf(v.same, 'r'); r && *r < p.known.size() && Rumours::IsLive(p.known[*r].first)) {
                    const auto rid = p.known[*r].first;
                    logger::info("{}: the same as rumour #{}; {}", head, rid, detail);
                    Rumours::Reinforce(rid, p.mills, p.form, now);
                    MarkWithStretch(m, "same", rid, p.uuid, detail, now);
                    continue;
                }
                if (v.spread < threshold || v.interest < minInterest) {
                    logger::info("{}: kept to themselves; {}: {}", head, detail, OneLine(m.text).substr(0, 140));
                    MarkWithStretch(m, "rejected", 0, p.uuid, detail, now);
                    continue;
                }
                // Another of their memories of the same happening, accepted just now.
                if (auto j = IndexOf(v.same, 'm'); j && *j < i) {
                    std::lock_guard lk(g_lock);
                    if (auto at = acceptedAt.find(Key('m', *j)); at != acceptedAt.end()) {
                        auto& a = g_accepted[at->second];
                        a.ownIds.push_back({ m.id, m.day });
                        a.ownIds.insert(a.ownIds.end(), m.stretchIds.begin(), m.stretchIds.end());
                        a.alsoTexts.push_back(m.text);
                        a.alsoTexts.insert(a.alsoTexts.end(), m.stretchTexts.begin(), m.stretchTexts.end());
                        a.m.events.insert(a.m.events.end(), m.events.begin(), m.events.end());
                        a.interest = std::max(a.interest, v.interest);
                        logger::info("{}: the same happening as their {}; {}", head, v.same, detail);
                        continue;
                    }
                }
                logger::info("{}: accepted; {}: {}", head, detail, OneLine(m.text).substr(0, 140));
                Accepted a;
                a.who = p;
                a.who.memories.clear();
                a.who.known.clear();
                a.m = m;
                a.ownIds = m.stretchIds;
                a.alsoTexts = m.stretchTexts;
                a.interest = v.interest;
                a.spread = v.spread;
                a.everything = v.everything;
                std::lock_guard lk(g_lock);
                acceptedAt[key] = g_accepted.size();
                g_accepted.push_back(std::move(a));
            }
        }

        void HandleScreenResults(double now) {
            std::vector<ScreenResult> results;
            {
                std::lock_guard lk(g_lock);
                results.swap(g_screenResults);
                g_screening -= static_cast<int>(results.size());
                if (g_screening < 0) g_screening = 0;
            }
            for (auto& r : results) {
                if (r.gen != g_generation) continue;
                if (!r.verdicts && r.noRoute) {
                    bool first;
                    {
                        std::lock_guard lk(g_lock);
                        first = !std::exchange(g_noRoute, true);
                    }
                    if (first) logger::info("Harvest: SkyrimNet has no decision provider, so screening asks the rumour_screen chat prompt instead");
                    ScreenByChat(r.gen, r.who);
                    continue;
                }
                if (!r.verdicts) {
                    logger::warn("Harvest: screening {}'s memories failed ({}); trying again next harvest", r.who.name, r.error);
                    for (const auto& m : r.who.memories) Failed(m.id, r.who.uuid, r.error, now);
                    continue;
                }
                Judge(r.who, *r.verdicts, now);
            }
        }

        // ---- Seeding ----

        bool ShareEvents(const std::vector<std::int64_t>& a, const std::vector<std::int64_t>& b) {
            for (auto x : a) {
                if (std::find(b.begin(), b.end(), x) != b.end()) return true;
            }
            return false;
        }

        void StartSeeding(unsigned gen) {
            std::vector<Accepted> batch;
            {
                std::lock_guard lk(g_lock);
                if (g_screening > 0 || g_seeding || g_accepted.empty() || Clock::now() < g_seedRetryAt) return;
                // One person's memories close together in time are one happening. The most important is
                // written, with the others beside it for the writer.
                {
                    std::vector<std::size_t> order;
                    for (std::size_t i = 0; i < g_accepted.size(); ++i) order.push_back(i);
                    std::stable_sort(order.begin(), order.end(), [](std::size_t x, std::size_t y) {
                        const auto &a = g_accepted[x], &b = g_accepted[y];
                        return a.who.uuid != b.who.uuid ? a.who.uuid < b.who.uuid : a.m.day < b.m.day;
                    });
                    std::vector<bool> folded(g_accepted.size(), false);
                    for (std::size_t s = 0; s < order.size();) {
                        const auto& first = g_accepted[order[s]];
                        std::size_t e = s + 1;
                        while (e < order.size() && first.who.uuid && first.m.day >= 0.0) {
                            const auto &prev = g_accepted[order[e - 1]], &next = g_accepted[order[e]];
                            if (next.who.uuid != first.who.uuid || next.m.day - prev.m.day > kSameStretchDays) break;
                            ++e;
                        }
                        if (e - s > 1) {
                            std::size_t top = order[s];
                            for (std::size_t k = s; k < e; ++k) {
                                const auto& c = g_accepted[order[k]];
                                if (c.m.importance > g_accepted[top].m.importance) top = order[k];
                            }
                            auto&       main = g_accepted[top];
                            std::string ids;
                            for (std::size_t k = s; k < e; ++k) {
                                if (order[k] == top) continue;
                                auto& o = g_accepted[order[k]];
                                ids += fmt::format("{}{}", ids.empty() ? "" : ", ", o.m.id);
                                main.ownIds.push_back({ o.m.id, o.m.day });
                                main.ownIds.insert(main.ownIds.end(), o.ownIds.begin(), o.ownIds.end());
                                main.alsoTexts.push_back(o.m.text);
                                main.alsoTexts.insert(main.alsoTexts.end(), o.alsoTexts.begin(), o.alsoTexts.end());
                                main.alsoIds.insert(main.alsoIds.end(), o.alsoIds.begin(), o.alsoIds.end());
                                main.m.events.insert(main.m.events.end(), o.m.events.begin(), o.m.events.end());
                                main.witnesses.insert(main.witnesses.end(), o.witnesses.begin(), o.witnesses.end());
                                main.interest = std::max(main.interest, o.interest);
                                folded[order[k]] = true;
                            }
                            logger::info("Harvest: {}'s memories {} and {} are one stretch of time; writing {}", main.who.name, ids,
                                         main.m.id, main.m.id);
                        }
                        s = e;
                    }
                    std::vector<Accepted> kept;
                    for (std::size_t i = 0; i < g_accepted.size(); ++i) {
                        if (!folded[i]) kept.push_back(std::move(g_accepted[i]));
                    }
                    g_accepted = std::move(kept);
                }
                // The likeliest talk first; then several witnesses' memories of one happening become one rumour.
                std::stable_sort(g_accepted.begin(), g_accepted.end(), [](const Accepted& a, const Accepted& b) {
                    if (a.interest != b.interest) return a.interest > b.interest;
                    return a.m.importance > b.m.importance;
                });
                std::vector<Accepted> rest;
                const auto            max = static_cast<std::size_t>(Settings::MaxSeedsPerSweep());
                for (auto& a : g_accepted) {
                    auto same = std::find_if(batch.begin(), batch.end(), [&](const Accepted& b) {
                        return !a.m.events.empty() && ShareEvents(a.m.events, b.m.events);
                    });
                    if (same != batch.end()) {
                        if (same->who.form != a.who.form) same->witnesses.emplace_back(a.who.form, a.who.mills);
                        if (same->who.form == a.who.form) {
                            same->ownIds.push_back({ a.m.id, a.m.day });
                            same->ownIds.insert(same->ownIds.end(), a.ownIds.begin(), a.ownIds.end());
                            same->alsoTexts.push_back(a.m.text);
                        } else {
                            same->alsoIds.push_back(a.m.id);
                            for (const auto& [id, day] : a.ownIds) same->alsoIds.push_back(id);
                        }
                        same->alsoIds.insert(same->alsoIds.end(), a.alsoIds.begin(), a.alsoIds.end());
                        same->m.events.insert(same->m.events.end(), a.m.events.begin(), a.m.events.end());
                        continue;
                    }
                    if (batch.size() < max) batch.push_back(std::move(a));
                    else rest.push_back(std::move(a));
                }
                g_accepted = std::move(rest);
                g_seeding = true;
            }

            // Who each memory may be about: its owner, the player, and the others SkyrimNet linked to it.
            const auto                           playerUuid = SkyrimNet::UUIDOf(kPlayer);
            std::vector<std::vector<RE::FormID>> subjects;
            std::string                          block;
            for (std::size_t i = 0; i < batch.size(); ++i) {
                const auto&              a = batch[i];
                std::vector<RE::FormID>  forms{ a.who.form };
                std::vector<std::string> names{ a.who.name + " (who remembers it)" };
                auto add = [&](RE::FormID f, const std::string& note) {
                    if (!f || forms.size() >= kMaxSubjects) return;
                    for (auto g : forms) {
                        if (g == f) return;
                    }
                    const auto n = NameOf(f);
                    if (n.empty()) return;
                    forms.push_back(f);
                    names.push_back(n + note);
                };
                add(kPlayer, " (the player)");
                for (auto u : a.m.related) {
                    if (u == playerUuid || u == a.who.uuid) continue;
                    add(SkyrimNet::FormOf(u), "");
                }
                std::string people;
                for (std::size_t k = 0; k < names.size(); ++k) people += fmt::format("{}s{} = {}", k ? "; " : "", k + 1, names[k]);
                std::string also;
                for (const auto& t : a.alsoTexts) also += fmt::format("\nTheir other memory of it: {}", OneLine(t).substr(0, 240));
                block += fmt::format("{}\nWho remembers it: {}, one of {}\nTheir memory: {}{}\nHow much they'd tell: {}\nPeople: {}\n\n",
                                     Key('m', i), a.who.name, Retell::LineOf(a.who.mills.front()), OneLine(a.m.text), also,
                                     a.everything ? "everything, juicy details and all"
                                                  : "only the gist, without the intimate details, their feelings or confidences",
                                     people);
                subjects.push_back(std::move(forms));
            }
            const json ctx = { { "seedMemories", Trim(block) }, { "seedCount", batch.size() } };
            logger::info("Harvest: writing {} rumour(s) from memories", batch.size());

            SeedResult pending{ gen, batch, subjects };
            const bool queued = SkyrimNet::SendCustomPrompt(kSeedPrompt, kSeedPrompt, Text::Dump(ctx), [pending](std::string text, bool ok) mutable {
                if (ok) pending.text = std::move(text);
                else pending.error = text.substr(0, 200);
                std::lock_guard lk(g_lock);
                g_seedResults.push_back(std::move(pending));
            });
            if (!queued) {
                std::lock_guard lk(g_lock);
                g_seedResults.push_back(SeedResult{ gen, std::move(batch), {}, std::nullopt, "SkyrimNet couldn't queue the call (needs API v8+)" });
            }
        }

        // ---- Remembering the telling ----

        /** SkyrimNet gives tags as a JSON array, or as the array's text. */
        bool HasTag(const json& row, std::string_view tag) {
            auto t = row.contains("tags") ? row["tags"] : json();
            if (t.is_string()) t = json::parse(t.get<std::string>(), nullptr, false);
            if (!t.is_array()) return false;
            for (const auto& x : t) {
                if (x.is_string() && x.get<std::string>() == tag) return true;
            }
            return false;
        }

        /** The one a rumour starts with remembers starting it, so they know it was them when asked
         *  who's been talking. Only they do: those it hops to later hear it offstage, and a telling
         *  on screen is SkyrimNet's to remember. */
        void RememberTelling(const Accepted& a, std::int32_t rid, const std::string& telling, double now) {
            std::string group = a.who.mills.empty() ? std::string("people") : Retell::LineOf(a.who.mills.front());
            if (const auto cut = group.find_first_of(":."); cut != std::string::npos) group.resize(cut);
            group = Trim(group);
            std::string place;
            if (auto set = Mills::Current(); set && !a.who.mills.empty()) {
                if (const auto* m = set->Find(a.who.mills.front())) place = m->placeName;
            }
            const auto text = fmt::format("I'm the one who spread it. I couldn't keep my mouth shut, and gossiped to {} about what I knew: {}",
                                          group, Trim(telling));
            const auto id = SkyrimNet::AddMemory(a.who.form, text, kToldImportance, "EXPERIENCE", place,
                                                 json::array({ kOwnTag, "gossip", "rumor" }).dump());
            if (!id) {
                logger::warn("Harvest: couldn't store {}'s memory of telling rumour #{}", a.who.name, rid);
                return;
            }
            MarkScreened(id, "own", rid, a.who.uuid, {}, "", now);
            logger::info("Harvest: {} remembers telling rumour #{} (memory {})", a.who.name, rid, id);
        }

        void SeedFailed(Accepted a, const std::string& why, double now) {
            if (++a.attempts < kMaxAttempts) {
                std::lock_guard lk(g_lock);
                g_accepted.push_back(std::move(a));
                return;
            }
            logger::warn("Harvest: gave up writing a rumour from {}'s memory {} ({})", a.who.name, a.m.id, why);
            MarkScreened(a.m.id, "seed_failed", 0, a.who.uuid, a.m.events, why, now);
            MarkFolded(a, "seed_failed", 0, why, now);
        }

        void HandleSeedResults(double now) {
            std::vector<SeedResult> results;
            {
                std::lock_guard lk(g_lock);
                results.swap(g_seedResults);
                if (!results.empty()) g_seeding = false;
            }
            for (auto& r : results) {
                if (r.gen != g_generation) continue;
                if (!r.text) {
                    logger::warn("Harvest: the rumour_seed call failed ({}); trying again in a minute", r.error);
                    {
                        std::lock_guard lk(g_lock);
                        g_seedRetryAt = Clock::now() + std::chrono::seconds(60);
                    }
                    for (auto& a : r.batch) SeedFailed(std::move(a), r.error, now);
                    continue;
                }
                const auto seeds = ParseSeeds(*r.text);
                for (std::size_t i = 0; i < r.batch.size(); ++i) {
                    auto& a = r.batch[i];
                    auto  s = seeds.find(i + 1);
                    if (s == seeds.end() || (!s->second.skip && s->second.telling.empty())) {
                        SeedFailed(std::move(a), "no telling for " + Key('m', i) + " in: " + OneLine(*r.text).substr(0, 300), now);
                        continue;
                    }
                    if (s->second.skip) {
                        logger::info("Harvest: the writer found nothing to tell in {}'s memory {}", a.who.name, a.m.id);
                        MarkScreened(a.m.id, "skipped", 0, a.who.uuid, a.m.events, "", now);
                        MarkFolded(a, "skipped", 0, "", now);
                        continue;
                    }
                    json about = json::array();
                    for (auto k : s->second.about) {
                        if (k < r.subjects[i].size()) about.push_back(r.subjects[i][k]);
                    }
                    // The one whose memory it is, and the witnesses whose memories were folded in, were
                    // there: they go by their own memories of it, so the talk stays out of their bios
                    // and their gossip, and nobody tells their own story back as hearsay.
                    json wereThere = json::array({ a.who.form });
                    for (const auto& [f, mills] : a.witnesses) wereThere.push_back(f);
                    // The telling is theirs, in the first person: they tell it themselves, and their own
                    // mills, like every other, have it retold as talk about what they told.
                    const json request = { { "source", "RumourEngine" },
                                           { "key", fmt::format("mem:{}", a.m.id) },
                                           { "text", s->second.telling },
                                           { "origin", { { "mills", a.who.mills }, { "teller", a.who.form } } },
                                           { "notability", NotabilityOf(a.interest) },
                                           { "subjects", about },
                                           { "topics", s->second.topics },
                                           { "known_better_by", wereThere } };
                    std::string error;
                    const auto  rid = Rumours::Add(Text::Dump(request), error);
                    if (!rid) {
                        SeedFailed(std::move(a), "AddRumour: " + error, now);
                        continue;
                    }
                    logger::info("Harvest: rumour #{} from {}'s memory {} (interest {:.2f}, {} other witness(es))", rid, a.who.name, a.m.id,
                                 a.interest, a.witnesses.size());
                    MarkScreened(a.m.id, "seeded", rid, a.who.uuid, a.m.events, fmt::format("interest {:.2f}, spread {:.2f}, tell {}", a.interest, a.spread, a.everything ? "everything" : "gist"), now, a.m.day);
                    MarkFolded(a, "same_event", rid, "", now);
                    for (const auto& [f, mills] : a.witnesses) Rumours::Reinforce(rid, mills, f, now, true);
                    if (Settings::RememberTelling()) RememberTelling(a, rid, s->second.telling, now);
                }
            }
        }

        // ---- The sweep ----

        /** Take up to `limit` unscreened memories from a query's rows into p.memories, newest (or most
         *  important) first as the query ordered them. A memory of a happening that's already a rumour
         *  reinforces it instead, with no call. Returns how many did. With `distinct`, each of the
         *  `limit` is from its own stretch of time; a memory near one taken goes along with it. */
        int Collect(Person& p, const std::string& raw, std::size_t limit, double now, bool distinct = false) {
            const auto rows = json::parse(raw, nullptr, false);
            if (!rows.is_array()) return 0;
            {
                std::lock_guard lk(g_lock);
                if (!rows.empty() && !std::exchange(g_rowLogged, true)) {
                    logger::info("Harvest: first memory row, as SkyrimNet gives it: {}", rows.front().dump().substr(0, 1500));
                }
            }
            int matched = 0;
            for (const auto& row : rows) {
                if (p.memories.size() >= limit && !distinct) break;
                if (!row.is_object()) continue;
                Memory m;
                m.id = row.value("id", std::int64_t{ 0 });
                m.text = row.value("content", std::string{});
                m.importance = row.value("importance_score", 0.0);
                if (row.contains("game_time") && row["game_time"].is_number()) m.day = row["game_time"].get<double>() / 86400.0;
                if (!m.id || Trim(m.text).empty()) continue;
                // A diary entry is someone's own summary, not something that happened.
                if (m.text.rfind("Diary Entry:", 0) == 0) continue;
                if (HasTag(row, kOwnTag)) continue;
                {
                    std::lock_guard lk(g_lock);
                    if (g_screened.contains(m.id)) continue;
                }
                m.events = Numbers<std::int64_t>(row, "related_event_ids");
                m.related = Numbers<std::uint64_t>(row, "related_actors");

                // Another witness's memory of a happening that's already a rumour: theirs too, no call needed.
                std::int32_t known = 0;
                bool         byTime = false;
                {
                    std::lock_guard lk(g_lock);
                    for (auto e : m.events) {
                        if (auto it = g_eventRumour.find(e); it != g_eventRumour.end()) {
                            known = it->second;
                            break;
                        }
                    }
                    // Or another of their own memories from the same stretch of time became one.
                    if (auto it = g_ownerRumours.find(p.uuid); !known && m.day >= 0.0 && it != g_ownerRumours.end()) {
                        for (const auto& [day, rid] : it->second) {
                            if (std::abs(day - m.day) <= kSameStretchDays) {
                                known = rid;
                                byTime = true;
                                break;
                            }
                        }
                    }
                }
                if (known && Rumours::IsLive(known)) {
                    logger::info("Harvest: {}'s memory {} is of the happening behind rumour #{}{}", p.name, m.id, known,
                                 byTime ? " (from the same stretch of time)" : "");
                    Rumours::Reinforce(known, p.mills, p.form, now, true);  // their memory of the happening itself
                    MarkScreened(m.id, "same_event", known, p.uuid, m.events, "", now, m.day);
                    ++matched;
                    continue;
                }
                if (distinct) {
                    auto near = std::find_if(p.memories.begin(), p.memories.end(), [&](const Memory& t) { return t.Near(m.day); });
                    if (near != p.memories.end()) {
                        near->stretchIds.push_back({ m.id, m.day });
                        near->stretchTexts.push_back(m.text);
                        continue;
                    }
                    if (p.memories.size() >= limit) continue;
                }
                p.memories.push_back(std::move(m));
            }
            std::lock_guard lk(g_lock);
            for (const auto& m : p.memories) {  // in hand; not fetched again meanwhile
                g_screened.insert(m.id);
                for (const auto& [id, day] : m.stretchIds) g_screened.insert(id);
            }
            return matched;
        }

        void Sweep(unsigned gen, double now) {
            auto set = Mills::Current();
            if (!set) return;
            const double window = Settings::HarvestWindowDays();
            const auto   raw = SkyrimNet::ActorEngagement(kEngagementMax, window * 86400.0, 7 * 86400.0);
            const auto   rows = json::parse(raw, nullptr, false);
            if (!rows.is_array()) {
                logger::warn("Harvest: SkyrimNet's actor engagement wasn't a list: {}", raw.substr(0, 300));
                return;
            }
            {
                std::lock_guard lk(g_lock);
                if (!rows.empty() && !std::exchange(g_engagementLogged, true)) {
                    logger::info("Harvest: first engagement row, as SkyrimNet gives it: {}", rows.front().dump().substr(0, 800));
                }
            }

            // Everyone with memories at all, most recently important first. The engagement
            // figures only order the list: what counts is what the memory query finds.
            struct Lead {
                RE::FormID form;
                double     shortTerm, mediumTerm, total;
            };
            std::vector<Lead> leads;
            for (const auto& row : rows) {
                if (!row.is_object()) continue;
                const auto form = static_cast<RE::FormID>(row.value("formId", std::uint64_t{ 0 }));
                if (!form || form == kPlayer || row.value("memoryCount", 0) <= 0) continue;
                leads.push_back({ form, row.value("recentMemoryImportanceShort", 0.0), row.value("recentMemoryImportanceMedium", 0.0),
                                  row.value("totalMemoryImportance", 0.0) });
            }
            std::sort(leads.begin(), leads.end(), [](const Lead& a, const Lead& b) {
                if (a.shortTerm != b.shortTerm) return a.shortTerm > b.shortTerm;
                if (a.mediumTerm != b.mediumTerm) return a.mediumTerm > b.mediumTerm;
                return a.total > b.total;
            });

            const auto          maxPeople = static_cast<std::size_t>(Settings::HarvestMaxActors());
            const auto          perActor = static_cast<std::size_t>(Settings::HarvestMemoriesPerActor());
            std::vector<Person> people;
            int                 sameEvent = 0, following = 0, homeless = 0, unknown = 0, nothingNew = 0;
            for (const auto& lead : leads) {
                if (people.size() >= maxPeople) break;
                // SkyrimNet may name the base form; its memories and our mills want the reference it last saw.
                const auto uuid = SkyrimNet::UUIDOf(lead.form);
                const auto seen = uuid ? SkyrimNet::FormOf(uuid) : 0;
                const auto formId = seen ? seen : lead.form;
                auto*      form = RE::TESForm::LookupByID(formId);
                auto*      actor = form ? form->As<RE::Actor>() : nullptr;
                // Following the player: their stories wait until they're dismissed and settled (stage 4b).
                // Dismissed but not settled yet: the same wait, and then their time away all at once.
                if (actor && (Delivery::InPlayersCompany(actor) ||
                              (actor->GetActorBase() && Placements::Unsettled(actor->GetActorBase()->GetFormID())))) {
                    ++following;
                    continue;
                }
                Person p;
                p.form = formId;
                p.uuid = uuid;
                p.name = NameOf(formId);
                if (!p.uuid || p.name.empty()) {
                    ++unknown;
                    continue;
                }
                p.mills = actor ? set->MillsFor(actor) : set->MillsFor(formId);
                if (p.mills.empty() && formId != lead.form) p.mills = set->MillsFor(lead.form);
                if (p.mills.empty()) {  // no home to start talk in
                    ++homeless;
                    continue;
                }
                p.gossip = GossipOf(*set, formId);

                SkyrimNet::MemoryFilter f;
                f.maxCount = static_cast<int>(perActor * 3);
                f.minImportance = static_cast<float>(std::max(0.0, Settings::ImportanceFloor() - Settings::GossipFloorBonus() * p.gossip));
                f.afterSeconds = (now - window) * 86400.0;
                f.beforeSeconds = now * 86400.0;
                sameEvent += Collect(p, SkyrimNet::QueryMemories(formId, f), perActor, now);
                if (p.memories.empty()) {
                    ++nothingNew;
                    continue;
                }
                p.known = Rumours::KnownBy(formId, p.mills, now, kMaxKnown);
                people.push_back(std::move(p));
            }

            std::size_t count = 0;
            for (const auto& p : people) count += p.memories.size();
            logger::info("Harvest: day {:.2f}: {} new memories from {} people to screen; of {} SkyrimNet listed, {} following, {} in no "
                         "mill, {} unknown, {} with nothing new above the floor{}",
                         now, count, people.size(), leads.size(), following, homeless, unknown, nothingNew,
                         sameEvent ? fmt::format("; {} matched to a rumour by event", sameEvent) : "");
            for (const auto& p : people) Screen(gen, p);
        }

        /** A settled follower's time away (ADR-001, "Dismissed followers"): their most important memories
         *  from joining to leaving, screened as stories they've brought home, in the mills where they live now. */
        void Catchups(unsigned gen, double now) {
            auto list = Placements::TakeCatchups();
            if (list.empty()) return;
            auto set = Mills::Current();
            if (!set) return;
            const auto max = static_cast<std::size_t>(Settings::FollowerHarvestMax());
            const auto perCall = static_cast<std::size_t>(Settings::HarvestMemoriesPerActor());
            for (const auto& c : list) {
                Person p;
                p.form = c.ref;
                p.uuid = SkyrimNet::UUIDOf(c.ref);
                p.name = NameOf(c.ref);
                auto* actor = RE::TESForm::LookupByID<RE::Actor>(c.ref);
                p.mills = actor ? set->MillsFor(actor) : set->MillsFor(c.ref);
                if (!p.uuid || p.name.empty() || p.mills.empty() || max == 0) {
                    logger::info("Harvest: {}'s time away isn't harvested ({})", p.name.empty() ? fmt::format("{:08X}", c.ref) : p.name,
                                 max == 0 ? "turned off" : p.mills.empty() ? "they live in no mill" : "SkyrimNet doesn't know them");
                    continue;
                }
                p.gossip = GossipOf(*set, c.ref);
                p.back = true;

                SkyrimNet::MemoryFilter f;
                f.maxCount = static_cast<int>(max * 3);
                f.minImportance = static_cast<float>(std::max(0.0, Settings::ImportanceFloor() - Settings::GossipFloorBonus() * p.gossip));
                f.afterSeconds = c.joined ? *c.joined * 86400.0 : 0.0;
                f.beforeSeconds = c.dismissed * 86400.0;
                f.byImportance = true;
                const int   matched = Collect(p, SkyrimNet::QueryMemories(c.ref, f), max, now, true);
                std::size_t along = 0;
                for (const auto& m : p.memories) along += m.stretchIds.size();
                logger::info("Harvest: {} is back from travelling with the player: {} memories from {} to day {:.2f} to screen{}{}", p.name,
                             p.memories.size(), c.joined ? fmt::format("day {:.2f}", *c.joined) : std::string("their first"), c.dismissed,
                             along ? fmt::format(", with {} more from the same stretches of time", along) : "",
                             matched ? fmt::format(", {} matched to a rumour by event", matched) : "");
                if (p.memories.empty()) continue;

                // One request per few memories, as for anyone.
                p.known = Rumours::KnownBy(c.ref, p.mills, now, kMaxKnown);
                std::vector<Memory> all = std::move(p.memories);
                for (std::size_t i = 0; i < all.size(); i += perCall) {
                    Person chunk = p;
                    chunk.memories.assign(all.begin() + i, all.begin() + std::min(all.size(), i + perCall));
                    Screen(gen, chunk);
                }
            }
        }
    }

    void Load() {
        std::unordered_set<std::int64_t>               screened;
        std::unordered_map<std::int64_t, std::int32_t> events;
        std::unordered_map<std::uint64_t, std::vector<std::pair<double, std::int32_t>>> owners;
        std::set<std::pair<std::int32_t, std::uint64_t>>                                 wereThere;  // rumour, SkyrimNet UUID
        Database::With([&](sqlite3* db) {
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db, "SELECT memory_id, rumour_id, events, actor, memory_day, verdict FROM screened;", -1, &st, nullptr) !=
                SQLITE_OK) {
                return;
            }
            while (sqlite3_step(st) == SQLITE_ROW) {
                screened.insert(sqlite3_column_int64(st, 0));
                const auto rid = sqlite3_column_int(st, 1);
                auto*      actor = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
                auto*      verdict = reinterpret_cast<const char*>(sqlite3_column_text(st, 5));
                std::uint64_t uuid = 0;
                if (rid && actor && *actor) {
                    try {
                        uuid = std::stoull(actor);
                    } catch (...) {
                    }
                }
                if (uuid && sqlite3_column_type(st, 4) != SQLITE_NULL) owners[uuid].push_back({ sqlite3_column_double(st, 4), rid });
                // Their memory became the rumour, was of the happening behind it, or is of telling it:
                // they were there. ("same" is the talk reaching them, which is hearsay to them too.)
                if (uuid && verdict) {
                    const std::string_view v = verdict;
                    if (v == "seeded" || v == "same_event" || v == "own") wereThere.insert({ rid, uuid });
                }
                auto* text = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
                if (!rid || !text) continue;
                const auto j = json::parse(text, nullptr, false);
                if (!j.is_array()) continue;
                for (const auto& e : j) {
                    if (e.is_number_integer()) events[e.get<std::int64_t>()] = rid;
                }
            }
            sqlite3_finalize(st);
        });
        {
            std::lock_guard lk(g_lock);
            g_screened = std::move(screened);
            g_eventRumour = std::move(events);
            g_ownerRumours = std::move(owners);
            logger::info("Harvest: {} memories screened before in this playthrough", g_screened.size());
        }
        // Rumours from before this was done listed the people who were there as having heard it only,
        // so it came back to them as talk. Rumours::Load has run, so the live ones are known.
        std::vector<std::pair<std::int32_t, RE::FormID>> who;
        for (const auto& [rid, uuid] : wereThere) {
            if (const auto form = SkyrimNet::FormOf(uuid)) who.emplace_back(rid, form);
        }
        if (const int added = Rumours::AddKnownBetter(who); added > 0) {
            logger::info("Harvest: {} time(s), someone who was there now goes by their own memory of a live rumour, not the talk", added);
        }
    }

    void Reset() {
        std::lock_guard lk(g_lock);
        ++g_generation;
        g_screened.clear();
        g_eventRumour.clear();
        g_ownerRumours.clear();
        g_failures.clear();
        g_lastSweep = -1.0;
        g_screening = 0;
        g_seeding = false;
        g_seedRetryAt = {};
        g_accepted.clear();
        g_screenResults.clear();
        g_seedResults.clear();
    }

    void Pump() {
        if (!Session::IsReady() || !Settings::HarvestEnabled() || SkyrimNet::Version() < 12) return;
        const unsigned gen = g_generation;
        const double   now = Rumours::Now();
        HandleScreenResults(now);
        HandleSeedResults(now);
        StartSeeding(gen);
        {
            std::lock_guard lk(g_lock);
            if (g_screening > 0 || g_seeding) return;
        }
        if (!SkyrimNet::MemoryReady()) {
            static std::atomic<bool> noted = false;
            if (!noted.exchange(true)) logger::info("Harvest: waiting for SkyrimNet's memory system to be ready");
            return;
        }
        // A follower who has just settled doesn't wait for the next sweep.
        Catchups(gen, now);
        {
            std::lock_guard lk(g_lock);
            const double interval = Settings::HarvestIntervalHours() / 24.0;
            if (g_lastSweep >= 0.0 && now >= g_lastSweep && now - g_lastSweep < interval) return;
            g_lastSweep = now;
        }
        Sweep(gen, now);
    }
}
