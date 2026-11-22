#include "Quests.h"
#include "Database.h"
#include "Ini.h"
#include "Mills.h"
#include "Rumours.h"
#include "Session.h"
#include "Settings.h"
#include "SkyrimNet.h"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <array>
#include <charconv>
#include <chrono>
#include <optional>

namespace RumourEngine::Quests {

    namespace {

        using json = nlohmann::json;
        using Clock = std::chrono::steady_clock;

        constexpr const char* kDecision = "rumourengine/quest";
        constexpr const char* kPrompt = "rumour_quest";
        constexpr const char* kVariant = "rumour_quest";
        constexpr int         kMaxAttempts = 3;
        constexpr auto        kCallTimeout = std::chrono::minutes(3);  // a call with no reply by then counts as failed
        // After a stage is set, how long its quest is looked at for having ended. A quest's
        // script may complete it a little after the stage that leads there.
        constexpr auto kWatchFor = std::chrono::seconds(30);
        constexpr int  kNearCells = 8;  // how far out the wilderness looks for a place to name (cells)

        enum class Rule { None, Always, Never };

        /** A quests.ini line: always or never, or a notability (always, at that notability). */
        struct Override {
            Rule   rule = Rule::None;
            double notability = -1.0;
        };

        struct Ending {
            std::string key;  // the quest's editor ID lowercased, else its form id in hex
            RE::FormID  form = 0;
            int         stage = 0;
            std::string name;     // "Waking Nightmare (a Daedric quest)"
            std::string journal;  // the first entry and the ending's, as the journal has them
            std::string place;    // "Nightcaller Temple, in the Pale"
            std::string player;   // "<their name>, a Breton man"
            std::string origin;   // the location form it starts from, in hex
            double      day = 0.0;
            std::string state;  // asking, telling
            double      heard = -1.0, far = -1.0, notability = -1.0;
            Override    rule;
            bool        preview = false;  // the debug inbox's quest_preview: logged, never stored or seeded
            // Under way, this session only.
            bool              inFlight = false;
            int               failures = 0;
            Clock::time_point retryAt{};
            Clock::time_point sentAt{};
        };

        /** What a call gave, from SkyrimNet's thread to Pump. */
        struct Reply {
            bool        ok = false;
            bool        noRoute = false;
            double      heard = -1.0, far = -1.0;
            std::string text;
        };

        struct Watch {
            std::uint16_t     stage = 0;
            Clock::time_point until{};
        };

        std::mutex                                g_lock;
        unsigned                                  g_generation = 0;
        std::shared_ptr<const PluginScan::Result> g_scan;
        std::map<std::string, Override>           g_overrides;  // "edid" or "edid:stage", lowercased
        std::set<std::string>                     g_known;      // quests with a row: handled, or finished before
        std::map<RE::FormID, Watch>               g_watch;
        std::map<RE::FormID, std::uint16_t>       g_homeless;  // ended where there's no location: waiting for the player to reach one
        RE::FormID                                g_lastLoc = 0;  // the last location the player was in, this session
        std::map<std::string, Ending>             g_pending;
        std::map<std::string, Reply>              g_replies;
        bool                                      g_noRoute = false;  // SkyrimNet has no decision provider
        bool                                      g_loaded = false;   // rows read and the save's finished quests swept
        bool                                      g_swept = false;
        bool                                      g_rowsRead = false;
        std::atomic<bool>                         g_checkQueued = false;
        std::atomic<bool>                         g_sweepQueued = false;

        // ---- Text ----

        std::string OneLine(std::string s) {
            for (std::size_t at = 0; (at = s.find('\n', at)) != std::string::npos; at += 3) s.replace(at, 1, " / ");
            return s;
        }

        bool IsUtf8(std::string_view s) {
            for (std::size_t i = 0; i < s.size();) {
                const auto c = static_cast<unsigned char>(s[i]);
                const int  n = c < 0x80 ? 0 : (c >> 5) == 0x6 ? 1 : (c >> 4) == 0xE ? 2 : (c >> 3) == 0x1E ? 3 : -1;
                if (n < 0 || i + n >= s.size() + (n == 0 ? 1 : 0)) return false;
                for (int k = 1; k <= n; ++k) {
                    if ((static_cast<unsigned char>(s[i + k]) >> 6) != 0x2) return false;
                }
                i += n + 1;
            }
            return true;
        }

        /** The English strings and older plugins are Windows-1252; SkyrimNet wants UTF-8. */
        std::string Utf8(std::string_view s) {
            if (IsUtf8(s)) return std::string(s);
            static constexpr char16_t k80[32] = { 0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                                                  0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD, 0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                                                  0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178 };
            std::string out;
            out.reserve(s.size() + 8);
            for (char ch : s) {
                const auto     c = static_cast<unsigned char>(ch);
                const char16_t u = c < 0x80 ? c : c < 0xA0 ? k80[c - 0x80] : c;
                if (u < 0x80) {
                    out += static_cast<char>(u);
                } else if (u < 0x800) {
                    out += static_cast<char>(0xC0 | (u >> 6));
                    out += static_cast<char>(0x80 | (u & 0x3F));
                } else {
                    out += static_cast<char>(0xE0 | (u >> 12));
                    out += static_cast<char>(0x80 | ((u >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (u & 0x3F));
                }
            }
            return out;
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

        // ---- Journal entries ----

        /** A DLSTRINGS file's directory, read once a session. The main thread only. */
        struct Table {
            bool                                         read = false;
            bool                                         ok = false;
            std::uint32_t                                base = 0;
            std::unordered_map<std::uint32_t, std::uint32_t> offsets;
        };
        std::map<std::string, Table> g_tables;

        std::string Language() {
            if (auto* s = RE::GetINISetting("sLanguage:General")) {
                if (const char* v = s->GetString(); v && *v) return v;
            }
            return "english";
        }

        /** A localized plugin's journal text, from Strings/<plugin>_<language>.DLSTRINGS, loose or in
         *  an archive, through the game's own file system. The main thread. */
        std::string DLString(const std::string& plugin, std::uint32_t id) {
            if (!id) return {};
            const auto stem = plugin.substr(0, plugin.rfind('.'));
            const auto path = fmt::format("Strings\\{}_{}.DLSTRINGS", stem, Language());
            auto&      t = g_tables[Ini::Lower(path)];
            if (t.read && !t.ok) return {};
            RE::BSResourceNiBinaryStream s(path);
            if (!s.good()) {
                if (!t.read) logger::warn("Quests: can't open {}, so {}'s journal can't be read", path, plugin);
                t.read = true;
                return {};
            }
            if (!t.read) {
                t.read = true;
                std::uint32_t head[2]{};
                if (!s.read(reinterpret_cast<char*>(head), 8) || head[0] > 4'000'000) {
                    logger::warn("Quests: {} isn't a strings file this can read", path);
                    return {};
                }
                std::vector<std::uint32_t> dir(std::size_t(head[0]) * 2);
                if (head[0] && !s.read(reinterpret_cast<char*>(dir.data()), head[0] * 8)) {
                    logger::warn("Quests: {} ends early", path);
                    return {};
                }
                for (std::size_t i = 0; i < head[0]; ++i) t.offsets[dir[2 * i]] = dir[2 * i + 1];
                t.base = 8 + head[0] * 8;
                t.ok = true;
            }
            auto it = t.offsets.find(id);
            if (it == t.offsets.end()) return {};
            const auto at = static_cast<std::int64_t>(t.base) + it->second;
            s.seek(static_cast<std::int32_t>(at - static_cast<std::int64_t>(s.tell())));
            std::uint32_t len = 0;
            if (!s.read(reinterpret_cast<char*>(&len), 4) || len > (1u << 20)) return {};
            std::string out(len, '\0');
            if (len && !s.read(out.data(), len)) return {};
            if (auto z = out.find('\0'); z != std::string::npos) out.resize(z);
            return Utf8(out);
        }

        std::string EntryText(const PluginScan::QuestJournal& j, const PluginScan::LogEntry& e) {
            return j.localized ? DLString(j.file, e.stringId) : Utf8(e.text);
        }

        /** The quest's text-replacement data: who its aliases name, and which entry the journal shows. */
        const RE::BGSQuestInstanceText* Instance(const RE::TESQuest* q) {
            const RE::BGSQuestInstanceText* found = nullptr;
            for (const auto* inst : q->instanceData) {
                if (!inst) continue;
                if (inst->id == q->currentInstanceID) return inst;
                found = inst;
            }
            return found;
        }

        RE::TESForm* AliasForm(RE::TESQuest* q, const RE::BGSQuestInstanceText* inst, const RE::BGSBaseAlias* a) {
            if (inst) {
                for (const auto& sd : inst->stringData) {
                    if (sd.aliasID != a->aliasID) continue;
                    if (auto* f = RE::TESForm::LookupByID(sd.fullNameFormID)) return f;
                }
            }
            RE::ObjectRefHandle h;
            q->CreateRefHandleByAliasID(h, a->aliasID);
            if (auto ref = h.get()) return ref.get();
            return nullptr;
        }

        std::string NameOf(RE::TESForm* f) {
            if (!f) return {};
            if (auto* r = f->As<RE::TESObjectREFR>()) {
                const char* n = r->GetDisplayFullName();
                return n ? Utf8(n) : std::string{};
            }
            const char* n = f->GetName();
            return n ? Utf8(n) : std::string{};
        }

        /** 0 for a man, 1 for a woman, -1 if it isn't a person. */
        int SexOf(RE::TESForm* f) {
            if (!f) return -1;
            RE::TESNPC* npc = nullptr;
            if (auto* a = f->As<RE::Actor>()) npc = a->GetActorBase();
            else npc = f->As<RE::TESNPC>();
            if (!npc) return -1;
            return npc->GetSex() == RE::SEX::kFemale ? 1 : 0;
        }

        std::string Pronoun(std::string field, int sex) {
            const bool cap = field.find("cap") != std::string::npos;
            if (cap) field.erase(field.find("cap"), 3);
            static const std::map<std::string, std::array<const char*, 3>, std::less<>> kWords = {
                { "pronoun", { "he", "she", "they" } },          { "pronounobj", { "him", "her", "them" } },
                { "pronounpos", { "his", "hers", "theirs" } },   { "pronounposobj", { "his", "her", "their" } },
                { "pronounref", { "himself", "herself", "themselves" } }, { "pronounint", { "himself", "herself", "themselves" } },
            };
            auto        it = kWords.find(field);
            std::string w = it == kWords.end() ? "they" : it->second[sex < 0 ? 2 : sex];
            if (cap && !w.empty()) w[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(w[0])));
            return w;
        }

        /** One <...> tag, as the journal would fill it in. */
        std::string Tag(RE::TESQuest* q, const RE::BGSQuestInstanceText* inst, std::string_view tag) {
            const auto eq = tag.find('=');
            if (eq == std::string_view::npos) return {};  // <BaseName> and the like: nothing a rumour needs
            std::string       kind = Ini::Lower(std::string(tag.substr(0, eq)));
            const std::string name = Ini::Trim(tag.substr(eq + 1));
            std::string       field;
            if (auto dot = kind.find('.'); dot != std::string::npos) {
                field = kind.substr(dot + 1);
                kind.resize(dot);
            }
            if (kind == "global") {
                std::optional<float> value;
                if (inst) {
                    for (const auto& v : inst->valueData) {
                        const char* id = v.global ? v.global->GetFormEditorID() : nullptr;
                        if (id && Ini::Lower(id) == Ini::Lower(name)) value = v.value;
                    }
                }
                if (!value) {
                    if (auto* g = RE::TESForm::LookupByEditorID<RE::TESGlobal>(name)) value = g->value;
                }
                if (!value) return {};
                if (*value == std::floor(*value)) return fmt::format("{}", static_cast<long long>(*value));
                return fmt::format("{:.1f}", *value);
            }
            if (kind != "alias") return name;
            const RE::BGSBaseAlias* alias = nullptr;
            for (const auto* a : q->aliases) {
                if (a && Ini::Lower(a->aliasName.c_str()) == Ini::Lower(name)) {
                    alias = a;
                    break;
                }
            }
            RE::TESForm* form = alias ? AliasForm(q, inst, alias) : nullptr;
            if (field.starts_with("pronoun")) return Pronoun(field, SexOf(form));
            if (field == "race" && form) {
                if (auto* a = form->As<RE::Actor>(); a && a->GetRace()) return NameOf(a->GetRace());
            }
            auto n = NameOf(form);
            return n.empty() ? name : n;  // unfilled: the alias's own name, which mostly says what it stands for
        }

        /** Text with its <Alias=...> and <Global=...> tags filled in, as the journal shows it. */
        std::string Fill(RE::TESQuest* q, std::string_view text) {
            const auto* inst = Instance(q);
            std::string out;
            out.reserve(text.size());
            std::size_t i = 0;
            while (i < text.size()) {
                const auto open = text.find('<', i);
                const auto close = open == std::string_view::npos ? open : text.find('>', open);
                if (close == std::string_view::npos) {
                    out += text.substr(i);
                    break;
                }
                out += text.substr(i, open - i);
                out += Tag(q, inst, text.substr(open + 1, close - open - 1));
                i = close + 1;
            }
            return Ini::Trim(out);
        }

        /** Whether the quest got to this stage: the game's own GetStageDone condition, which
         *  holds after the quest has stopped. (The quest's executedStages list doesn't say.)
         *  Main thread only. */
        bool StageDone(RE::TESQuest* q, std::uint16_t index) {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!q || !player) return false;
            RE::TESConditionItem item;
            item.data.object = RE::CONDITIONITEMOBJECT::kSelf;
            item.data.functionData.function = RE::FUNCTION_DATA::FunctionID::kGetStageDone;
            item.data.functionData.params[0] = q;
            item.data.functionData.params[1] = reinterpret_cast<void*>(static_cast<std::uintptr_t>(index));
            item.data.flags.opCode = RE::CONDITION_ITEM_DATA::OpCode::kEqualTo;
            item.data.comparisonValue.f = 1.0f;
            RE::ConditionCheckParams params(player, player);
            return item(params);
        }

        std::string KeyOf(const RE::TESQuest* q) {
            const char* edid = q->GetFormEditorID();
            return edid && *edid ? Ini::Lower(edid) : fmt::format("{:08x}", q->GetFormID());
        }

        const char* TypeWord(RE::QUEST_DATA::Type t) {
            using T = RE::QUEST_DATA::Type;
            switch (t) {
            case T::kMainQuest: return "main";
            case T::kMagesGuild: return "College of Winterhold";
            case T::kThievesGuild: return "Thieves Guild";
            case T::kDarkBrotherhood: return "Dark Brotherhood";
            case T::kCompanionsQuest: return "Companions";
            case T::kMiscellaneous: return "miscellaneous";
            case T::kDaedric: return "Daedric";
            case T::kSideQuest: return "side";
            case T::kCivilWar: return "Civil War";
            case T::kDLC01_Vampire: return "Dawnguard";
            case T::kDLC02_Dragonborn: return "Dragonborn";
            default: return "";
            }
        }

        /** quests.ini's line for this ending ("edid:stage"), else for the quest. Under g_lock. */
        Override RuleFor(const std::string& key, int stage) {
            if (auto it = g_overrides.find(fmt::format("{}:{}", key, stage)); it != g_overrides.end()) return it->second;
            if (auto it = g_overrides.find(key); it != g_overrides.end()) return it->second;
            return {};
        }

        // ---- Rows ----

        json Detail(const Ending& e) {
            return { { "name", e.name },   { "journal", e.journal }, { "place", e.place },
                     { "player", e.player }, { "origin", e.origin }, { "notability", e.notability } };
        }

        void Store(const Ending& e, std::int32_t rumourId = 0) {
            if (e.preview) return;
            Database::With([&](sqlite3* db) {
                sqlite3_stmt* st = nullptr;
                if (sqlite3_prepare_v2(db,
                                       "INSERT INTO quest_endings(quest, form_id, stage, state, heard, far, rumour_id, detail, game_time) "
                                       "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?) ON CONFLICT(quest) DO UPDATE SET state = excluded.state, "
                                       "heard = excluded.heard, far = excluded.far, rumour_id = excluded.rumour_id, detail = excluded.detail;",
                                       -1, &st, nullptr) != SQLITE_OK) {
                    logger::error("Quests: {}", sqlite3_errmsg(db));
                    return;
                }
                const auto detail = Detail(e).dump();
                sqlite3_bind_text(st, 1, e.key.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(st, 2, e.form);
                sqlite3_bind_int(st, 3, e.stage);
                sqlite3_bind_text(st, 4, e.state.c_str(), -1, SQLITE_TRANSIENT);
                if (e.heard >= 0.0) sqlite3_bind_double(st, 5, e.heard);
                if (e.far >= 0.0) sqlite3_bind_double(st, 6, e.far);
                if (rumourId) sqlite3_bind_int(st, 7, rumourId);
                sqlite3_bind_text(st, 8, detail.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_double(st, 9, e.day);
                if (sqlite3_step(st) != SQLITE_DONE) logger::error("Quests: {}", sqlite3_errmsg(db));
                sqlite3_finalize(st);
            });
        }

        /** Done with it: its row says how it went, and it leaves the queue. */
        void Finish(Ending e, const char* state, std::int32_t rumourId = 0) {
            e.state = state;
            Store(e, rumourId);
            std::lock_guard lk(g_lock);
            g_pending.erase(e.key);
        }

        /** Back in the queue, as it stands now. */
        void Requeue(const Ending& e) {
            Store(e);
            std::lock_guard lk(g_lock);
            if (auto it = g_pending.find(e.key); it != g_pending.end()) it->second = e;
        }

        // ---- Seeing a quest end (the main thread) ----

        /** A quest has ended: what its journal says, and where. With `preview` (the debug inbox), a
         *  quest the save finished long ago, logged and, with `ask`, taken through the calls too. */
        void Consider(RE::TESQuest* q, std::uint16_t stage, unsigned gen, bool preview = false, bool ask = false) {
            const auto                                key = (preview ? std::string("preview:") : std::string{}) + KeyOf(q);
            std::shared_ptr<const PluginScan::Result> scan;
            Override                                  rule;
            {
                std::lock_guard lk(g_lock);
                if (gen != g_generation || (!preview && (!g_loaded || g_known.contains(key)))) return;
                scan = g_scan;
                rule = RuleFor(KeyOf(q), stage);
            }
            if (!scan) return;
            const auto jit = scan->quests.find(q->GetFormID());
            if (jit == scan->quests.end()) {
                if (preview) logger::info("Quests: preview: {} has no journal in the plugins", key);
                return;  // no journal, so nothing to tell from
            }
            const auto  type = q->GetType();
            const char* rawName = q->GetName();
            if (!preview && rule.rule == Rule::None &&
                (type == RE::QUEST_DATA::Type::kNone || type == RE::QUEST_DATA::Type::kMiscellaneous || !rawName || !*rawName)) {
                return;  // errands, chores and the game's own bookkeeping
            }

            Ending e;
            e.preview = preview;
            e.key = key;
            e.form = q->GetFormID();
            e.stage = stage;
            e.rule = rule;
            auto* calendar = RE::Calendar::GetSingleton();
            e.day = calendar ? calendar->GetCurrentGameTime() : 0.0;
            const auto quest = Fill(q, rawName ? Utf8(rawName) : key);
            const char* typeWord = TypeWord(type);
            e.name = *typeWord ? fmt::format("{} (a {} quest)", quest, typeWord) : quest;

            auto takeIt = [&](const char* state, const std::string& why) {
                e.state = state;
                Store(e);
                std::lock_guard lk(g_lock);
                if (gen == g_generation && !preview) g_known.insert(key);
                logger::info("Quests: {}{} ended (stage {}): {}", preview ? "preview: " : "", e.name, stage, why);
            };
            if (rule.rule == Rule::Never) {
                takeIt("never", "quests.ini says never");
                return;
            }

            // The ending: the entry the journal shows now, else the stage that ended it.
            const auto& j = jit->second;
            auto        stageOf = [&](int index) -> const PluginScan::QuestStage* {
                for (const auto& s : j.stages) {
                    if (s.index == index) return &s;
                }
                return nullptr;
            };
            std::string ending;
            int         endStage = -1, endItem = -1;
            if (const auto* inst = Instance(q)) {
                if (const auto* s = stageOf(inst->journalStage);
                    s && inst->journalStageItem >= 0 && inst->journalStageItem < static_cast<int>(s->entries.size())) {
                    ending = EntryText(j, s->entries[inst->journalStageItem]);
                    endStage = s->index;
                    endItem = inst->journalStageItem;
                }
            }
            if (ending.empty()) {
                if (const auto* s = stageOf(stage)) {
                    for (int pass = 0; pass < 2 && ending.empty(); ++pass) {
                        for (std::size_t i = 0; i < s->entries.size() && ending.empty(); ++i) {
                            if (pass == 0 && !(s->entries[i].flags & 1)) continue;
                            ending = EntryText(j, s->entries[i]);
                            endStage = s->index;
                            endItem = static_cast<int>(i);
                        }
                    }
                }
            }
            if (ending.empty()) {
                takeIt("untold", "no journal entry to tell it from");
                return;
            }
            ending = Fill(q, ending);

            // The opening: the first stage before the ending that the player got to, with one entry
            // (or several saying the same), which sets the scene. Many entries restate the earlier
            // ones, and then the ending is enough. Should the game say no stage of the journal was
            // done, it can't be asked here, and the first such stage at all will do.
            std::set<std::uint16_t> done;
            for (const auto& s : j.stages) {
                if (StageDone(q, s.index)) done.insert(s.index);
            }
            if (preview) {
                std::string list;
                for (auto i : done) list += fmt::format(" {}", i);
                logger::info("Quests: preview: {} has {} of the journal's {} stage(s) done:{}", key, done.size(), j.stages.size(), done.empty() ? " none, so any may open it" : list);
            }
            std::string opening;
            for (int pass = 0; pass < 2 && opening.empty(); ++pass) {
                if (pass == 1 && !done.empty()) break;
                for (const auto& s : j.stages) {
                    if (endStage >= 0 && s.index >= endStage) continue;
                    if (pass == 0 && !done.contains(s.index)) continue;
                    std::set<std::string> texts;
                    for (const auto& en : s.entries) {
                        if (en.flags & 3) continue;
                        if (auto t = EntryText(j, en); !t.empty()) texts.insert(t);
                    }
                    if (texts.size() == 1) {
                        opening = Fill(q, *texts.begin());
                        break;
                    }
                }
            }
            e.journal = opening.empty() || ending.starts_with(opening.substr(0, 40)) ? ending : opening + "\n...\n" + ending;

            // Where it ended, which is where the talk starts: the player's location, else their
            // cell's, else (in the wilderness) the nearest exterior cell's, else the last one they
            // were in. With none at all, it waits until they reach one.
            auto*            player = RE::PlayerCharacter::GetSingleton();
            const auto       set = Mills::Current();
            RE::BGSLocation* loc = player ? player->GetCurrentLocation() : nullptr;
            bool             nearby = false;
            if (!loc && player) {
                if (auto* cell = player->GetParentCell()) loc = cell->GetLocation();
            }
            if (!loc && player && set) {
                const auto* world = player->GetWorldspace();
                auto        at = set->LocationAt(world ? world->GetFormID() : 0, player->GetPosition());
                if (!at && world) {
                    at = set->LocationNear(world->GetFormID(), player->GetPosition(), kNearCells);
                    nearby = at != 0;
                }
                if (at) loc = RE::TESForm::LookupByID<RE::BGSLocation>(at);
            }
            if (loc && !nearby) {
                std::lock_guard lk(g_lock);
                if (gen == g_generation) g_lastLoc = loc->GetFormID();
            } else if (!loc) {
                RE::FormID last;
                {
                    std::lock_guard lk(g_lock);
                    last = g_lastLoc;
                }
                if (last && (loc = RE::TESForm::LookupByID<RE::BGSLocation>(last))) nearby = true;
            }
            if (!loc) {
                if (preview) {
                    logger::info("Quests: preview: {} ended (stage {}; the journal's entry {} of stage {}), but the player is in no location, so it "
                                 "would wait for one: {}",
                                 e.name, stage, endItem, endStage, OneLine(e.journal));
                    return;
                }
                {
                    std::lock_guard lk(g_lock);
                    if (gen != g_generation) return;
                    g_homeless[q->GetFormID()] = stage;
                }
                logger::info("Quests: {} ended (stage {}) where there's no location, so it waits for the player to reach one", e.name, stage);
                return;
            }
            e.origin = fmt::format("0x{:08X}", loc->GetFormID());
            e.place = (nearby ? "near " : "") + NameOf(loc);
            if (set) {
                if (auto h = set->holdNames.find(set->HoldOf(loc->GetFormID())); h != set->holdNames.end() && h->second != e.place) {
                    e.place += (e.place.empty() ? "" : ", in ") + h->second;
                }
            }
            if (player) {
                const auto* base = player->GetActorBase();
                const auto  race = player->GetRace() ? NameOf(player->GetRace()) : std::string{};
                const char* sex = base && base->GetSex() == RE::SEX::kFemale ? "woman" : "man";
                const bool  an = !race.empty() && std::string_view("AEIOUaeiou").find(race[0]) != std::string_view::npos;
                e.player = race.empty() ? fmt::format("{}, a {}", NameOf(player), sex)
                                        : fmt::format("{}, a{} {} {}", NameOf(player), an ? "n" : "", race, sex);
            }

            if (rule.notability >= 0.0) {
                e.state = "telling";
                e.notability = rule.notability;
            } else {
                e.state = "asking";
            }
            logger::info("Quests: {}{} ended {}{} (stage {}; the journal's entry {} of stage {}){}; {}: {}", preview ? "preview: " : "", e.name,
                         nearby ? "" : "at ", e.place, stage, endItem, endStage, rule.rule == Rule::Always ? ", and quests.ini says always" : "", e.player, OneLine(e.journal));
            if (preview && !ask) return;
            Store(e);
            {
                std::lock_guard lk(g_lock);
                if (gen != g_generation) return;
                if (!preview) g_known.insert(key);
                g_pending[key] = e;
            }
        }

        void CheckWatched(unsigned gen) {
            std::vector<std::pair<RE::FormID, std::uint16_t>> ended;
            {
                std::lock_guard lk(g_lock);
                if (gen != g_generation) return;
                const auto now = Clock::now();
                for (auto it = g_watch.begin(); it != g_watch.end();) {
                    auto* q = RE::TESForm::LookupByID<RE::TESQuest>(it->first);
                    if (q && q->IsCompleted()) {
                        ended.emplace_back(it->first, it->second.stage);
                        it = g_watch.erase(it);
                    } else if (!q || now > it->second.until) {
                        it = g_watch.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            for (const auto& [id, stage] : ended) {
                if (auto* q = RE::TESForm::LookupByID<RE::TESQuest>(id)) Consider(q, stage, gen);
            }
        }

        /** The quests this save had finished before RumourEngine saw them end: old news, never told. */
        void Sweep(unsigned gen) {
            std::shared_ptr<const PluginScan::Result> scan;
            std::set<std::string>                     known;
            {
                std::lock_guard lk(g_lock);
                if (gen != g_generation) return;
                scan = g_scan;
                known = g_known;
            }
            auto* handler = RE::TESDataHandler::GetSingleton();
            if (!scan || !handler) return;
            std::vector<std::pair<std::string, RE::FormID>> before;
            for (auto* q : handler->GetFormArray<RE::TESQuest>()) {
                if (!q || !q->IsCompleted() || !scan->quests.contains(q->GetFormID())) continue;
                if (auto key = KeyOf(q); !known.contains(key)) before.emplace_back(std::move(key), q->GetFormID());
            }
            auto*        calendar = RE::Calendar::GetSingleton();
            const double day = calendar ? calendar->GetCurrentGameTime() : 0.0;
            if (!before.empty()) {
                Database::With([&](sqlite3* db) {
                    sqlite3_exec(db, "BEGIN;", nullptr, nullptr, nullptr);
                    sqlite3_stmt* st = nullptr;
                    if (sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO quest_endings(quest, form_id, state, game_time) VALUES(?, ?, 'before', ?);", -1,
                                           &st, nullptr) == SQLITE_OK) {
                        for (const auto& [key, form] : before) {
                            sqlite3_bind_text(st, 1, key.c_str(), -1, SQLITE_TRANSIENT);
                            sqlite3_bind_int64(st, 2, form);
                            sqlite3_bind_double(st, 3, day);
                            sqlite3_step(st);
                            sqlite3_reset(st);
                        }
                    }
                    sqlite3_finalize(st);
                    sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr);
                });
            }
            std::size_t total, waiting;
            {
                std::lock_guard lk(g_lock);
                if (gen != g_generation) return;
                for (const auto& [key, form] : before) g_known.insert(key);
                g_loaded = true;
                g_swept = true;
                total = g_known.size();
                waiting = g_pending.size();
            }
            logger::info("Quests: watching for quest endings; {} on record ({} newly found finished before RumourEngine saw them end), "
                         "{} under way",
                         total, before.size(), waiting);
        }

        class StageSink final : public RE::BSTEventSink<RE::TESQuestStageEvent> {
        public:
            RE::BSEventNotifyControl ProcessEvent(const RE::TESQuestStageEvent* e, RE::BSTEventSource<RE::TESQuestStageEvent>*) override {
                if (!e || !Session::IsReady() || !Settings::QuestsEnabled()) return RE::BSEventNotifyControl::kContinue;
                std::lock_guard lk(g_lock);
                if (g_loaded && g_scan && g_scan->quests.contains(e->formID)) g_watch[e->formID] = { e->stage, Clock::now() + kWatchFor };
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        /** The player's last location, and the endings that waited for one. */
        class LocationSink final : public RE::BSTEventSink<RE::TESActorLocationChangeEvent> {
        public:
            RE::BSEventNotifyControl ProcessEvent(const RE::TESActorLocationChangeEvent* e,
                                                  RE::BSTEventSource<RE::TESActorLocationChangeEvent>*) override {
                if (!e || !e->actor || !e->actor->IsPlayerRef()) return RE::BSEventNotifyControl::kContinue;
                std::lock_guard lk(g_lock);
                if (e->newLoc) {
                    g_lastLoc = e->newLoc->GetFormID();
                    for (const auto& [id, stage] : g_homeless) g_watch[id] = { stage, Clock::now() + kWatchFor };
                    g_homeless.clear();
                } else if (e->oldLoc) {
                    g_lastLoc = e->oldLoc->GetFormID();
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        // ---- Calls (the worker thread; replies arrive on SkyrimNet's) ----

        void Deliver(unsigned gen, const std::string& key, Reply r) {
            std::lock_guard lk(g_lock);
            if (gen == g_generation) g_replies[key] = std::move(r);
        }

        void Ask(unsigned gen, const Ending& e) {
            const json ctx = { { "quest_text", e.name }, { "journal_text", e.journal } };
            const auto key = e.key;
            const bool queued = SkyrimNet::SendCustomDecision(kDecision, ctx.dump(), [gen, key](std::string, std::string text, bool ok) {
                // A SkyrimNet worker thread: no RE:: here.
                const auto j = json::parse(text, nullptr, false);
                Reply      r;
                if (ok && j.is_object()) {
                    const auto answers = j.value("answers", json::object());
                    auto       noul = [&](const char* q) {
                        auto a = answers.find(q);
                        return a != answers.end() && a->is_object() && a->contains("noul") && (*a)["noul"].is_number()
                                   ? (*a)["noul"].get<double>()
                                   : -1.0;
                    };
                    r.heard = noul("heard");
                    r.far = noul("far");
                    r.ok = r.heard >= 0.0 && r.far >= 0.0;
                }
                if (!r.ok) {
                    r.noRoute = j.is_object() && j.value("error", "") == "no_decisions_route";
                    r.text = text.substr(0, 200);
                }
                Deliver(gen, key, std::move(r));
            });
            if (!queued) Deliver(gen, key, Reply{ false, true, -1.0, -1.0, "SkyrimNet can't ask the decision model (needs API v12+)" });
        }

        void Tell(unsigned gen, const Ending& e) {
            const json ctx = { { "questName", e.name }, { "questJournal", e.journal }, { "questPlayer", e.player }, { "questPlace", e.place } };
            const auto key = e.key;
            const bool queued = SkyrimNet::SendCustomPrompt(kPrompt, kVariant, ctx.dump(), [gen, key](std::string text, bool ok) {
                // A SkyrimNet worker thread: no RE:: here.
                Reply r;
                r.text = ok ? CleanAnswer(text) : text.substr(0, 200);
                r.ok = ok && !r.text.empty();
                Deliver(gen, key, std::move(r));
            });
            if (!queued) Deliver(gen, key, Reply{ false, false, -1.0, -1.0, "SkyrimNet couldn't queue the telling (needs API v8+)" });
        }

        void Seed(Ending e, const std::string& text, bool retellOrigin) {
            if (e.preview) {
                logger::info("Quests: preview: {} would be a rumour at notability {:.2f}, starting from {}: {}", e.name, e.notability, e.origin,
                             text);
                std::lock_guard lk(g_lock);
                g_pending.erase(e.key);
                return;
            }
            const json request = { { "source", "RumourEngine" },
                                   { "key", "quest:" + e.key },
                                   { "text", text },
                                   { "origin", { { "location", e.origin } } },
                                   { "notability", e.notability },
                                   { "subjects", json::array({ "0x14" }) },
                                   { "topics", json::array({ "the_player" }) },
                                   { "happened", e.day },
                                   { "retell_origin", retellOrigin },
                                   { "if_absent", true } };
            std::string error;
            const auto  rid = Rumours::Add(request.dump(), error);
            if (!rid) {
                logger::warn("Quests: couldn't make a rumour of how {} ended ({})", e.name, error);
                Finish(std::move(e), "failed");
                return;
            }
            logger::info("Quests: rumour #{} from how {} ended (notability {:.2f}){}: {}", rid, e.name, e.notability,
                         retellOrigin ? ", from the journal, as the telling failed" : "", text);
            Finish(std::move(e), "seeded", rid);
        }

        /** Without an answer from a decision model: only what quests.ini says always is told. */
        void Unasked(Ending e, const std::string& why) {
            if (e.rule.rule != Rule::Always) {
                logger::info("Quests: {} isn't told ({}, and quests.ini doesn't say always)", e.name, why);
                Finish(std::move(e), "unasked");
                return;
            }
            e.notability = e.rule.notability >= 0.0 ? e.rule.notability : Settings::QuestsNotability();
            e.state = "telling";
            e.inFlight = false;
            e.failures = 0;
            Requeue(e);
            logger::info("Quests: {} will be told, as quests.ini says always ({}; notability {:.2f})", e.name, why, e.notability);
        }

        void Apply(Ending e, const Reply& r) {
            e.inFlight = false;
            if (e.state == "asking") {
                if (!r.ok) {
                    if (r.noRoute) {
                        {
                            std::lock_guard lk(g_lock);
                            g_noRoute = true;
                        }
                        Unasked(std::move(e), "no decision provider");
                        return;
                    }
                    if (++e.failures < kMaxAttempts) {
                        e.retryAt = Clock::now() + std::chrono::seconds(30 * e.failures);
                        Requeue(e);
                        logger::warn("Quests: no answer on {} ({}); asking again shortly", e.name, r.text);
                        return;
                    }
                    Unasked(std::move(e), "the decision model didn't answer");
                    return;
                }
                e.heard = r.heard;
                e.far = r.far;
                const bool gets = r.heard >= Settings::QuestsHeard() - 1e-6;
                const bool told = gets || e.rule.rule == Rule::Always;
                logger::info("Quests: {}: word gets out {:.2f}, other holds hear of it {:.2f}: {}", e.name, r.heard, r.far,
                             gets   ? "it will be told"
                             : told ? "told anyway, as quests.ini says always"
                                    : "it stays with those who were there");
                if (!told) {
                    Finish(std::move(e), "private");
                    return;
                }
                e.notability = std::clamp(0.5 + 0.4 * r.far, 0.5, 0.9);
                e.state = "telling";
                e.failures = 0;
                Requeue(e);
                return;
            }
            // Telling.
            if (r.ok) {
                Seed(std::move(e), r.text, false);
                return;
            }
            if (++e.failures < kMaxAttempts) {
                e.retryAt = Clock::now() + std::chrono::seconds(30 * e.failures);
                Requeue(e);
                logger::warn("Quests: the telling of {} failed ({}); trying again shortly", e.name, r.text);
                return;
            }
            // The journal itself, for the mills where it starts to put into their own words.
            auto account = fmt::format("{} finished {} {}{}. From their journal: {}", e.player, e.name, e.place.starts_with("near ") ? "" : "at ",
                                       e.place, e.journal);
            Seed(std::move(e), account, true);
        }
    }

    void SetScan(std::shared_ptr<const PluginScan::Result> scan) {
        std::lock_guard lk(g_lock);
        g_scan = std::move(scan);
    }

    void Preview(const std::string& editorID, bool ask) {
        auto* task = SKSE::GetTaskInterface();
        if (!task) return;
        task->AddTask([editorID, ask]() {
            unsigned gen;
            {
                std::lock_guard lk(g_lock);
                gen = g_generation;
            }
            auto* q = RE::TESForm::LookupByEditorID<RE::TESQuest>(editorID);
            if (!q) {
                logger::warn("Quests: preview: no quest {}", editorID);
                return;
            }
            logger::info("Quests: preview of {} (stage {}, {}completed)", editorID, q->GetCurrentStageID(), q->IsCompleted() ? "" : "not ");
            Consider(q, q->GetCurrentStageID(), gen, true, ask);
        });
    }

    void Install() {
        static StageSink    sink;
        static LocationSink moves;
        if (auto* events = RE::ScriptEventSourceHolder::GetSingleton()) {
            events->AddEventSink<RE::TESQuestStageEvent>(&sink);
            events->AddEventSink<RE::TESActorLocationChangeEvent>(&moves);
        }
    }

    void Load() {
        // quests.ini: EditorID (or EditorID:stage) = always, never, or a notability.
        std::map<std::string, Override> rules;
        const auto                      ini = Ini::Read("quests");
        for (const auto& line : Ini::Keyed(ini, "quests")) {  // sections are lowercased as they're read
            const auto v = Ini::Lower(line.value);
            Override   o;
            double     n = -1.0;
            if (v == "always") o.rule = Rule::Always;
            else if (v == "never") o.rule = Rule::Never;
            else if (auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), n); ec == std::errc{} && n >= 0.0 && n <= 1.0) {
                o.rule = Rule::Always;
                o.notability = n;
            } else {
                logger::warn("Quests: {}: {} = {} isn't always, never or a notability from 0 to 1", line.file, line.key, line.value);
                continue;
            }
            rules[line.key] = o;
        }

        std::set<std::string>         known;
        std::map<std::string, Ending> pending;
        Database::With([&](sqlite3* db) {
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db, "SELECT quest, form_id, stage, state, heard, far, detail, game_time FROM quest_endings;", -1, &st,
                                   nullptr) != SQLITE_OK) {
                logger::error("Quests: {}", sqlite3_errmsg(db));
                return;
            }
            while (sqlite3_step(st) == SQLITE_ROW) {
                const auto* key = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
                const auto* state = reinterpret_cast<const char*>(sqlite3_column_text(st, 3));
                if (!key || !state) continue;
                known.insert(key);
                const std::string_view s = state;
                if (s != "asking" && s != "telling") continue;
                Ending e;
                e.key = key;
                e.form = static_cast<RE::FormID>(sqlite3_column_int64(st, 1));
                e.stage = sqlite3_column_int(st, 2);
                e.state = state;
                e.heard = sqlite3_column_type(st, 4) == SQLITE_NULL ? -1.0 : sqlite3_column_double(st, 4);
                e.far = sqlite3_column_type(st, 5) == SQLITE_NULL ? -1.0 : sqlite3_column_double(st, 5);
                const auto* detail = reinterpret_cast<const char*>(sqlite3_column_text(st, 6));
                const auto  d = json::parse(detail ? detail : "", nullptr, false);
                if (!d.is_object()) continue;
                e.name = d.value("name", "");
                e.journal = d.value("journal", "");
                e.place = d.value("place", "");
                e.player = d.value("player", "");
                e.origin = d.value("origin", "");
                e.notability = d.value("notability", -1.0);
                e.day = sqlite3_column_double(st, 7);
                pending[e.key] = std::move(e);
            }
            sqlite3_finalize(st);
        });

        std::lock_guard lk(g_lock);
        g_overrides = std::move(rules);
        for (auto& [key, e] : pending) e.rule = RuleFor(key, e.stage);
        g_known = std::move(known);
        g_pending = std::move(pending);
        g_rowsRead = true;
        logger::info("Quests: {} line(s) in quests.ini{}, {} quest endings on record, {} still to finish", g_overrides.size(),
                     ini.names.empty() ? " (no quests.ini)" : "", g_known.size(), g_pending.size());
    }

    void Reset() {
        std::lock_guard lk(g_lock);
        ++g_generation;
        g_known.clear();
        g_watch.clear();
        g_homeless.clear();
        g_lastLoc = 0;
        g_pending.clear();
        g_replies.clear();
        g_noRoute = false;
        g_loaded = false;
        g_swept = false;
        g_rowsRead = false;
    }

    void Pump() {
        if (!Session::IsReady()) return;
        unsigned gen;
        bool     watching, sweep, noRoute;
        {
            std::lock_guard lk(g_lock);
            if (!g_rowsRead) return;
            gen = g_generation;
            watching = !g_watch.empty();
            sweep = !g_swept && g_scan;
            noRoute = g_noRoute;
        }
        auto* task = SKSE::GetTaskInterface();
        if (!task) return;
        if (sweep && !g_sweepQueued.exchange(true)) {
            task->AddTask([gen]() {
                g_sweepQueued = false;
                Sweep(gen);
            });
        }
        if (watching && !g_checkQueued.exchange(true)) {
            task->AddTask([gen]() {
                g_checkQueued = false;
                CheckWatched(gen);
            });
        }

        // What came back since the last look.
        std::vector<std::pair<Ending, Reply>> replies;
        {
            std::lock_guard lk(g_lock);
            for (auto& [key, r] : g_replies) {
                if (auto it = g_pending.find(key); it != g_pending.end()) replies.emplace_back(it->second, std::move(r));
            }
            g_replies.clear();
        }
        for (auto& [e, r] : replies) Apply(std::move(e), r);

        if (!Settings::QuestsEnabled()) return;
        // One call at a time: a few quests end in a game day at most.
        std::optional<Ending> next;
        {
            std::lock_guard lk(g_lock);
            if (gen != g_generation) return;
            const auto now = Clock::now();
            for (auto& [key, e] : g_pending) {
                if (!e.inFlight) continue;
                if (now - e.sentAt < kCallTimeout) return;
                // SkyrimNet never answered: another try, or the fallback once it has had them.
                e.inFlight = false;
                if (++e.failures >= kMaxAttempts) e.retryAt = now + std::chrono::hours(24 * 365);
                logger::warn("Quests: no reply on {} in {} minutes{}", e.name, kCallTimeout.count(),
                             e.failures >= kMaxAttempts ? "; leaving it until the next load" : "; trying again");
            }
            for (auto& [key, e] : g_pending) {
                if (now < e.retryAt) continue;
                e.inFlight = true;
                e.sentAt = now;
                next = e;
                break;
            }
        }
        if (!next) return;
        if (next->state == "asking") {
            if (noRoute) {
                Unasked(*next, "no decision provider");
                return;
            }
            Ask(gen, *next);
        } else {
            Tell(gen, *next);
        }
    }
}
