#include "Gossip.h"
#include "Database.h"
#include "Ini.h"
#include "Rumours.h"
#include "Settings.h"
#include "SkyrimNet.h"
#include "Text.h"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <charconv>
#include <chrono>
#include <deque>
#include <fstream>

namespace RumourEngine::Gossip {

    namespace {
        using json = nlohmann::json;
        using Ini::Lower;

        // The job table when there is no gossips.ini: tools/hopsim.py's, as stage 2 used it.
        constexpr std::pair<std::string_view, double> kBuiltInJobs[] = {
            { "jobinnkeeperfaction", 0.9 },     { "jobinnserver", 0.85 },          { "jobbardfaction", 0.85 },
            { "favorjobsbeggarsfaction", 0.8 }, { "favorjobsdrunksfaction", 0.8 }, { "jobstreetvendorfaction", 0.75 },
            { "jobmerchantfaction", 0.7 },      { "carriagesystemfaction", 0.7 },  { "jobfencefaction", 0.6 },
            { "jobhostlerfaction", 0.6 },       { "jobstewardfaction", 0.5 },      { "jobjarlfaction", 0.4 },
            { "isguardfaction", 0.3 },          { "jobhousecarlfaction", 0.25 },   { "jobcourtwizardfaction", 0.2 },
        };
        constexpr double kDefaultScore = 0.45;

        // What a bio reading means. The decision model answers with a level from 0 (tight-lipped) to
        // 4 (incorrigible), and a bio that says nothing either way reads as about 1: tools/
        // gossip_panel.py had ordinary townsfolk at 0.8 to 1.3, the plainly close-mouthed at 0.35 to
        // 0.6 and the plainly talkative from 2.2 to 3.3. So a reading inside the middle band leaves
        // the job's score alone, and one outside it moves the score towards 0 or 1.
        constexpr double kNeutralLow = 0.8;
        constexpr double kNeutralHigh = 1.3;
        constexpr double kLevelLowest = 0.35;
        constexpr double kLevelHighest = 3.2;

        constexpr const char* kTemplate = "rumourengine/gossip_score";
        constexpr auto        kRecheck = std::chrono::minutes(10);  // real time between looks at one person's bio
        constexpr int         kMaxInFlight = 2;
        constexpr int         kLooksPerPump = 4;

        // The blocks of a bio the reading is made from. Only a change to one of these asks again.
        constexpr std::string_view kBlocks[] = { "summary", "personality", "occupation", "skills", "speech_style" };

        struct Table {
            double                                  fallback = kDefaultScore;
            std::unordered_map<std::string, double> jobs;    // lowercased faction editor ID -> score
            std::unordered_map<std::string, double> people;  // lowercased name or NPC editor ID -> score
        };

        struct Reading {
            double        level = -1.0;  // 0 to 4; below 0, never read
            std::uint64_t bio = 0;       // fingerprint of the blocks it was read from
        };

        struct Asked {
            RE::FormID  key = 0;  // the person
            RE::FormID  ref = 0;  // their reference, for SkyrimNet
            std::string name;
        };

        std::mutex                                                            g_lock;
        std::shared_ptr<const Table>                                          g_table;
        std::unordered_map<RE::FormID, Reading>                               g_readings;
        std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> g_looked;
        std::deque<Asked>                                                     g_queue;
        std::vector<std::filesystem::path>                                    g_roots;  // prompt roots, best first
        bool                                                                  g_rootsFound = false;
        int                                                                   g_inFlight = 0;
        bool                                                                  g_noRoute = false;
        std::uint64_t                                                         g_gen = 0;
        std::atomic<std::uint64_t>                                            g_version = 0;

        double Mix(std::uint64_t a, std::uint64_t b) {
            std::uint64_t x = a * 0x9E3779B97F4A7C15ull + b * 0xBF58476D1CE4E5B9ull;
            x ^= x >> 31;
            x *= 0x94D049BB133111EBull;
            x ^= x >> 29;
            return double(x & 0xFFFFFFFFull) / 4294967296.0;
        }

        std::uint64_t Fnv(std::string_view s, std::uint64_t h = 0xCBF29CE484222325ull) {
            for (unsigned char c : s) {
                h ^= c;
                h *= 0x100000001B3ull;
            }
            return h;
        }

        std::optional<double> ParseScore(const std::string& s) {
            double out = 0.0;
            auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
            if (ec != std::errc{} || p != s.data() + s.size() || out < 0.0 || out > 1.0) return std::nullopt;
            return out;
        }

        std::shared_ptr<const Table> BuiltIn() {
            auto t = std::make_shared<Table>();
            for (const auto& [f, s] : kBuiltInJobs) t->jobs.emplace(f, s);
            return t;
        }

        std::shared_ptr<const Table> ReadTable() {
            const auto ini = Ini::Read("gossips");
            if (ini.names.empty()) {
                logger::warn("Gossip: no gossips.ini, so the built-in job scores are used and nobody is overridden");
                return BuiltIn();
            }
            auto t = std::make_shared<Table>();
            for (const auto& e : Ini::Keyed(ini, "jobs")) {
                const auto s = ParseScore(e.value);
                if (!s) {
                    logger::warn("Gossip: {} [Jobs] {} = {}: not a score from 0 to 1", e.file, e.key, e.value);
                    continue;
                }
                if (e.key == "default") t->fallback = *s;
                else t->jobs[e.key] = *s;
            }
            for (const auto& e : Ini::Keyed(ini, "people")) {
                const auto s = ParseScore(e.value);
                if (!s) {
                    logger::warn("Gossip: {} [People] {} = {}: not a score from 0 to 1", e.file, e.key, e.value);
                    continue;
                }
                t->people[e.key] = *s;
            }
            std::string files;
            for (const auto& n : ini.names) files += (files.empty() ? "" : ", ") + n;
            logger::info("Gossip: read {}: {} job score(s), default {:.2f}, {} override(s)", files, t->jobs.size(), t->fallback,
                         t->people.size());
            return t;
        }

        std::shared_ptr<const Table> TableNow() {
            std::lock_guard lk(g_lock);
            if (!g_table) g_table = BuiltIn();
            return g_table;
        }

        const double* OverrideOf(const Table& t, const Mills::Person& p) {
            if (t.people.empty()) return nullptr;
            if (!p.editorID.empty()) {
                if (auto it = t.people.find(Lower(p.editorID)); it != t.people.end()) return &it->second;
            }
            if (!p.name.empty()) {
                if (auto it = t.people.find(Lower(p.name)); it != t.people.end()) return &it->second;
            }
            return nullptr;
        }

        /** From their job alone, with a fixed nudge per person so not every innkeeper is the same. */
        double JobScore(const Table& t, const Mills::Person& p) {
            double job = t.fallback;
            bool   any = false;
            for (const auto& f : p.factions) {
                auto it = t.jobs.find(Lower(f));
                if (it == t.jobs.end()) continue;
                job = any ? std::max(job, it->second) : it->second;  // of several jobs, the most talkative
                any = true;
            }
            return std::clamp(job + (Mix(p.key, 7) - 0.5) * 0.2, 0.0, 1.0);
        }

        /** -1 (plainly tight-lipped) to 1 (plainly a gossip); 0 where the bio doesn't say. */
        double Lean(double level) {
            if (level > kNeutralHigh) return std::min(1.0, (level - kNeutralHigh) / (kLevelHighest - kNeutralHigh));
            if (level < kNeutralLow) return std::max(-1.0, (level - kNeutralLow) / (kNeutralLow - kLevelLowest));
            return 0.0;
        }

        double Combine(double job, double level) {
            if (level < 0.0) return job;
            const double w = Settings::BioWeight();
            const double lean = Lean(level);
            return std::clamp(lean >= 0.0 ? job + lean * (1.0 - job) * w : job * (1.0 + lean * w), 0.0, 1.0);
        }

        // ---- Bio files ----

        std::filesystem::path Utf8Path(const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); }

        /** Where SkyrimNet keeps character bios, best first: this save's layer (the only home of
         *  generated bios), the player's overlay, plugin layers, then SkyrimNet's own. */
        std::vector<std::filesystem::path> PromptRoots() {
            std::vector<std::filesystem::path> roots;
            std::error_code                    ec;
            const auto net = std::filesystem::current_path() / "Data" / "SKSE" / "Plugins" / "SkyrimNet";
            if (const auto save = SkyrimNet::SaveUniqueID(); !save.empty()) roots.push_back(net / "saves" / Utf8Path(save) / "prompts");
            roots.push_back(net / "overlay" / "prompts");
            for (const char* group : { "external", "library" }) {
                std::vector<std::filesystem::path> found;
                for (std::filesystem::directory_iterator it(net / group, ec), end; !ec && it != end; it.increment(ec)) {
                    std::error_code de;
                    if (it->is_directory(de) && it->path().filename() != "skyrimnet.base") found.push_back(it->path() / "prompts");
                }
                std::sort(found.begin(), found.end());
                roots.insert(roots.end(), found.begin(), found.end());
            }
            roots.push_back(net / "library" / "skyrimnet.base" / "prompts");
            return roots;
        }

        std::string ReadFile(const std::filesystem::path& p) {
            std::ifstream f(p, std::ios::binary);
            if (!f) return {};
            return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        }

        /** The text of `{% block name %}...{% endblock %}`, or empty. */
        std::string BlockOf(const std::string& text, std::string_view name) {
            std::size_t at = 0;
            while ((at = text.find("{%", at)) != std::string::npos) {
                const auto close = text.find("%}", at);
                if (close == std::string::npos) break;
                const auto tag = Ini::Trim(std::string_view(text).substr(at + 2, close - at - 2));
                at = close + 2;
                if (!tag.starts_with("block")) continue;
                if (Ini::Trim(std::string_view(tag).substr(5)) != name) continue;
                const auto end = text.find("{%", at);
                return end == std::string::npos ? std::string{} : Ini::Trim(std::string_view(text).substr(at, end - at));
            }
            return {};
        }

        /** A fingerprint of the blocks a reading is made from, in the generated bio and the authored
         *  one; 0 when the person has neither. */
        std::uint64_t Fingerprint(const std::vector<std::filesystem::path>& roots, const std::string& bioTemplate) {
            std::error_code ec;
            std::uint64_t   h = 0xCBF29CE484222325ull;
            bool            any = false;
            const std::pair<std::filesystem::path, std::filesystem::path> kinds[] = {
                { std::filesystem::path("characters") / "dynamic", Utf8Path(bioTemplate + ".dynamic.prompt") },
                { std::filesystem::path("characters"), Utf8Path(bioTemplate + ".prompt") },
            };
            for (const auto& [dir, file] : kinds) {
                for (const auto& root : roots) {
                    const auto path = root / dir / file;
                    if (!std::filesystem::is_regular_file(path, ec)) continue;
                    const auto text = ReadFile(path);
                    for (const auto b : kBlocks) {
                        const auto body = BlockOf(text, b);
                        if (body.empty()) continue;
                        h = Fnv(body, Fnv(b, h));
                        any = true;
                    }
                    break;  // the best layer's file is the one SkyrimNet uses
                }
            }
            return any ? h : 0;
        }

        void Store(const std::string& actor, double level, std::uint64_t bio, double now) {
            Database::With([&](sqlite3* db) {
                sqlite3_stmt* st = nullptr;
                if (sqlite3_prepare_v2(db,
                                       "INSERT INTO gossip_scores(actor, score, source, level, bio, game_time) VALUES(?,?,'model',?,?,?) "
                                       "ON CONFLICT(actor) DO UPDATE SET score = excluded.score, source = excluded.source, "
                                       "level = excluded.level, bio = excluded.bio, game_time = excluded.game_time;",
                                       -1, &st, nullptr) != SQLITE_OK) {
                    logger::error("Gossip: {}", sqlite3_errmsg(db));
                    return;
                }
                const auto hex = fmt::format("{:016X}", bio);
                sqlite3_bind_text(st, 1, actor.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_double(st, 2, level / 4.0);
                sqlite3_bind_double(st, 3, level);
                sqlite3_bind_text(st, 4, hex.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_double(st, 5, now);
                if (sqlite3_step(st) != SQLITE_DONE) logger::error("Gossip: {}", sqlite3_errmsg(db));
                sqlite3_finalize(st);
            });
        }

        /** The person an actor form (reference or base) is, or 0. */
        RE::FormID PersonKey(const Mills::MillSet& set, RE::FormID form) {
            if (set.persons.contains(form)) return form;
            if (auto it = set.baseOfRef.find(form); it != set.baseOfRef.end() && set.persons.contains(it->second)) return it->second;
            if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(form)) {
                if (const auto* base = actor->GetActorBase(); base && set.persons.contains(base->GetFormID())) return base->GetFormID();
            }
            return 0;
        }

        void Send(const Asked& a, std::uint64_t bio, std::uint64_t uuid, std::uint64_t gen, const Mills::Person& person) {
            const double now = Rumours::Now();
            const json   ctx = { { "gossip_npc", uuid }, { "gossip_name", a.name } };
            const bool   queued = SkyrimNet::SendCustomDecision(
                kTemplate, Text::Dump(ctx), [a, bio, uuid, gen, now, person](std::string, std::string text, bool ok) {
                    const auto j = json::parse(text, nullptr, false);
                    double     level = -1.0;
                    if (ok && j.is_object()) {
                        const auto answers = j.value("answers", json::object());
                        if (auto it = answers.find("gossip"); it != answers.end() && it->is_object()) {
                            // The expected level, counted from 0 whatever the template's lines are numbered.
                            if (it->contains("score") && (*it)["score"].is_number()) level = (*it)["score"].get<double>();
                        }
                    }
                    double before = 0.0, was = -1.0;
                    {
                        std::lock_guard lk(g_lock);
                        if (gen != g_gen) return;  // another save by now
                        --g_inFlight;
                        if (level < 0.0) {
                            if (j.is_object() && j.value("error", "") == "no_decisions_route" && !std::exchange(g_noRoute, true)) {
                                logger::info("Gossip: SkyrimNet has no decision provider, so bios aren't read and scores come from jobs alone");
                            } else if (!g_noRoute) {
                                logger::warn("Gossip: no reading of {}'s bio: {}", a.name, text.substr(0, 200));
                            }
                            return;
                        }
                        auto& r = g_readings[a.key];
                        was = r.level;
                        before = Combine(JobScore(*g_table, person), was);
                        r.level = std::clamp(level, 0.0, 4.0);
                        r.bio = bio;
                    }
                    g_version.fetch_add(1);
                    Store(std::to_string(uuid), level, bio, now);
                    const double after = ScoreOf(person);
                    if (was < 0.0) {
                        logger::info("Gossip: {}'s bio reads {:.2f} of 4: score {:.2f} -> {:.2f}", a.name, level, before, after);
                    } else {
                        logger::info("Gossip: {}'s bio has changed and now reads {:.2f} of 4 (was {:.2f}): score {:.2f} -> {:.2f}", a.name,
                                     level, was, before, after);
                    }
                });
            if (!queued) {
                std::lock_guard lk(g_lock);
                if (gen == g_gen) --g_inFlight;
            }
        }
    }

    void Load() {
        auto       table = ReadTable();
        auto       set = Mills::Current();
        std::unordered_map<RE::FormID, Reading> readings;
        int        rows = 0;
        if (set) {
            Database::With([&](sqlite3* db) {
                sqlite3_stmt* st = nullptr;
                if (sqlite3_prepare_v2(db, "SELECT actor, level, bio FROM gossip_scores WHERE source = 'model' AND level IS NOT NULL;", -1,
                                       &st, nullptr) != SQLITE_OK) {
                    return;
                }
                while (sqlite3_step(st) == SQLITE_ROW) {
                    ++rows;
                    const auto* actor = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
                    const auto* bio = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
                    if (!actor || !bio) continue;
                    std::uint64_t uuid = 0, fp = 0;
                    const std::string_view a(actor), b(bio);
                    if (std::from_chars(a.data(), a.data() + a.size(), uuid).ec != std::errc{}) continue;
                    if (std::from_chars(b.data(), b.data() + b.size(), fp, 16).ec != std::errc{}) continue;
                    const auto form = SkyrimNet::FormOf(uuid);
                    const auto key = form ? PersonKey(*set, form) : 0;
                    if (key) readings[key] = { sqlite3_column_double(st, 1), fp };
                }
                sqlite3_finalize(st);
            });
        }
        std::lock_guard lk(g_lock);
        g_table = std::move(table);
        g_readings = std::move(readings);
        g_looked.clear();
        g_queue.clear();
        g_roots.clear();
        g_rootsFound = false;
        g_noRoute = false;
        g_version.fetch_add(1);
        logger::info("Gossip: {} bio reading(s) kept from this save ({} stored)", g_readings.size(), rows);
    }

    void Reset() {
        std::lock_guard lk(g_lock);
        ++g_gen;
        g_inFlight = 0;
        g_readings.clear();
        g_looked.clear();
        g_queue.clear();
        g_roots.clear();
        g_rootsFound = false;
        g_version.fetch_add(1);
    }

    double ScoreOf(const Mills::Person& person) {
        const auto t = TableNow();
        if (const double* fixed = OverrideOf(*t, person)) return *fixed;
        double level = -1.0;
        {
            std::lock_guard lk(g_lock);
            if (auto it = g_readings.find(person.key); it != g_readings.end()) level = it->second.level;
        }
        return Combine(JobScore(*t, person), level);
    }

    std::uint64_t RollKey(const Mills::Person& person) {
        // A named NPC's editor ID outlives a load-order change; their form ID doesn't.
        if (person.key == person.npc && !person.editorID.empty()) return Fnv(Lower(person.editorID));
        return person.key;
    }

    void Want(RE::Actor* actor, const Mills::Person& person) {
        // Named people only: everyone else shares a generic bio that says nothing about them.
        if (!actor || person.key != person.npc || !Settings::RateBios()) return;
        const auto      now = std::chrono::steady_clock::now();
        std::lock_guard lk(g_lock);
        if (g_noRoute) return;
        auto [it, fresh] = g_looked.try_emplace(person.key, now);
        if (!fresh) {
            if (now - it->second < kRecheck) return;
            it->second = now;
        }
        g_queue.push_back({ person.key, actor->GetFormID(), person.name });
    }

    void Pump() {
        if (!Settings::RateBios() || !Database::IsOpen()) return;
        auto set = Mills::Current();
        if (!set) return;
        for (int i = 0; i < kLooksPerPump; ++i) {
            Asked                              a;
            std::uint64_t                      gen;
            std::vector<std::filesystem::path> roots;
            {
                std::lock_guard lk(g_lock);
                if (g_queue.empty() || g_noRoute || g_inFlight >= kMaxInFlight) return;
                a = std::move(g_queue.front());
                g_queue.pop_front();
                gen = g_gen;
                if (!g_rootsFound) {
                    g_roots = PromptRoots();
                    g_rootsFound = true;
                }
                roots = g_roots;
            }
            auto p = set->persons.find(a.key);
            if (p == set->persons.end() || OverrideOf(*TableNow(), p->second)) continue;
            const auto bioTemplate = SkyrimNet::BioTemplateName(a.ref);
            if (bioTemplate.empty()) continue;
            const auto bio = Fingerprint(roots, bioTemplate);
            if (!bio) continue;  // no bio of their own
            {
                std::lock_guard lk(g_lock);
                if (gen != g_gen) return;
                if (auto it = g_readings.find(a.key); it != g_readings.end() && it->second.level >= 0.0 && it->second.bio == bio) continue;
                ++g_inFlight;
            }
            const auto uuid = SkyrimNet::UUIDOf(a.ref);
            if (!uuid) {
                std::lock_guard lk(g_lock);
                if (gen == g_gen) --g_inFlight;
                continue;
            }
            Send(a, bio, uuid, gen, p->second);
        }
    }

    std::uint64_t Version() { return g_version.load(); }
}
