#include "Rumours.h"
#include "Api.h"
#include "Database.h"
#include "Gossip.h"
#include "Mills.h"
#include "Retell.h"
#include "Runtime.h"
#include "Settings.h"
#include "SkyrimNet.h"
#include "Spread.h"
#include "Text.h"

#include <sqlite3.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <unordered_set>

namespace RumourEngine::Rumours {

    namespace {

        // ---- SQLite, briefly ----

        class Stmt {
        public:
            Stmt(sqlite3* db, const char* sql) : db_(db) {
                if (sqlite3_prepare_v2(db, sql, -1, &st_, nullptr) != SQLITE_OK) {
                    logger::error("Rumours: SQL error {} in: {}", sqlite3_errmsg(db), sql);
                    st_ = nullptr;
                }
            }
            ~Stmt() { sqlite3_finalize(st_); }
            Stmt(const Stmt&) = delete;
            Stmt& operator=(const Stmt&) = delete;

            Stmt& Bind(int i, double v) {
                if (st_) sqlite3_bind_double(st_, i, v);
                return *this;
            }
            Stmt& Bind(int i, std::int64_t v) {
                if (st_) sqlite3_bind_int64(st_, i, v);
                return *this;
            }
            Stmt& Bind(int i, int v) { return Bind(i, std::int64_t(v)); }
            Stmt& Bind(int i, const std::string& v) {
                if (st_) sqlite3_bind_text(st_, i, v.c_str(), int(v.size()), SQLITE_TRANSIENT);
                return *this;
            }
            Stmt& Null(int i) {
                if (st_) sqlite3_bind_null(st_, i);
                return *this;
            }

            /** Next row; false at the end or on error. */
            bool Row() { return st_ && sqlite3_step(st_) == SQLITE_ROW; }

            /** Run to completion. */
            bool Run() {
                if (!st_) return false;
                const int rc = sqlite3_step(st_);
                if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
                    logger::error("Rumours: SQL error {}", sqlite3_errmsg(db_));
                    return false;
                }
                return true;
            }

            double       D(int c) { return sqlite3_column_double(st_, c); }
            std::int64_t I(int c) { return sqlite3_column_int64(st_, c); }
            bool         IsNull(int c) { return sqlite3_column_type(st_, c) == SQLITE_NULL; }
            std::string  S(int c) {
                auto* t = reinterpret_cast<const char*>(sqlite3_column_text(st_, c));
                return t ? t : "";
            }

        private:
            sqlite3*      db_;
            sqlite3_stmt* st_ = nullptr;
        };

        // ---- State ----

        std::mutex                                    g_lock;
        bool                                          g_loaded = false;
        std::map<std::int32_t, Rumour>                g_rumours;
        std::map<std::pair<std::string, std::string>, std::int32_t> g_byKey;
        std::set<std::pair<std::int32_t, std::string>> g_heard;  // (rumour, actor key) in earshot of a telling
        // The same people by form, so a prompt decorator can ask without calling into SkyrimNet.
        std::set<std::pair<std::int32_t, RE::FormID>> g_heardForms;

        // Retired, and kept as world knowledge or about to be: what get_rumourengine_heard needs to
        // say who had heard them.
        std::map<std::int32_t, Rumour>                g_promoted;
        std::vector<std::int32_t>                     g_withdrawn;  // entries of promoted rumours that are news again
        std::uint64_t                                 g_promotedVersion = 0;
        std::unordered_map<RE::FormID, std::pair<std::uint64_t, std::string>> g_heardPromoted;  // by actor

        struct Aired {
            std::chrono::steady_clock::time_point at;
            RE::FormID                            teller = 0;
        };
        std::map<std::int32_t, Aired> g_aired;

        const char* ScopeName(Scope s) {
            switch (s) {
            case Scope::Proclaimed: return "proclaimed";
            case Scope::Local: return "local";
            default: return "spread";
            }
        }

        std::optional<Scope> ParseScope(std::string_view s) {
            if (s.empty() || s == "spread") return Scope::Spread;
            if (s == "proclaimed") return Scope::Proclaimed;
            if (s == "local") return Scope::Local;
            return std::nullopt;
        }

        std::string Lower(std::string_view s) {
            std::string out(s);
            for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return out;
        }

        /** An actor's key: SkyrimNet's UUID, which survives load-order changes, else its form. */
        std::string ActorKey(RE::FormID formId) {
            if (const auto uuid = SkyrimNet::UUIDOf(formId)) return std::to_string(uuid);
            return fmt::format("form:{:08X}", formId);
        }

        // ---- Names (ADR-001, "Who can be told") ----

        std::vector<std::string> Words(std::string_view text) {
            std::vector<std::string> out;
            std::string              w;
            for (unsigned char c : text) {
                // Letters, digits, and anything outside ASCII (accented names) make words.
                if (std::isalnum(c) || c >= 0x80) {
                    w += static_cast<char>(std::tolower(c));
                } else if (!w.empty()) {
                    out.push_back(std::move(w));
                    w.clear();
                }
            }
            if (!w.empty()) out.push_back(std::move(w));
            return out;
        }

        bool WithinOneEdit(const std::string& a, const std::string& b) {
            if (a == b) return true;
            const std::size_t n = a.size(), m = b.size();
            if (n > m + 1 || m > n + 1) return false;
            std::size_t i = 0, j = 0;
            bool        edited = false;
            while (i < n && j < m) {
                if (a[i] == b[j]) {
                    ++i, ++j;
                    continue;
                }
                if (edited) return false;
                edited = true;
                if (n > m) ++i;
                else if (m > n) ++j;
                else ++i, ++j;
            }
            return !edited || (i == n && j == m);
        }

        /** The text names this person: their full name, or their first name alone, as whole words,
         *  ignoring case; a name of five letters or more also at one edit away, since retellings
         *  misspell them ("Rennart", "Renard"). */
        bool TextNames(const std::vector<std::string>& words, const std::string& name) {
            const auto parts = Words(name);
            if (parts.empty()) return false;
            for (std::size_t i = 0; i + parts.size() <= words.size(); ++i) {
                if (std::equal(parts.begin(), parts.end(), words.begin() + i)) return true;
            }
            const auto& first = parts.front();
            for (const auto& w : words) {
                if (w == first) return true;
                if (first.size() >= 5 && WithinOneEdit(w, first)) return true;
            }
            return false;
        }

        std::vector<std::string> NamedSubjects(const std::string& text, const std::vector<Subject>& subjects) {
            std::vector<std::string> out;
            const auto               words = Words(text);
            for (const auto& s : subjects) {
                if (!s.name.empty() && TextNames(words, s.name)) out.push_back(s.key);
            }
            return out;
        }

        // ---- Forms from a request ----

        /** "Plugin.esp:0xLOCALID", "0x14" or a runtime form ID number. */
        RE::TESForm* ParseForm(const nlohmann::json& j) {
            if (j.is_number_unsigned() || j.is_number_integer()) return RE::TESForm::LookupByID(j.get<std::uint32_t>());
            if (!j.is_string()) return nullptr;
            const auto  s = j.get<std::string>();
            const auto  colon = s.rfind(':');
            const auto  idText = colon == std::string::npos ? s : s.substr(colon + 1);
            std::uint32_t id = 0;
            try {
                id = static_cast<std::uint32_t>(std::stoul(idText, nullptr, 16));
            } catch (...) {
                return nullptr;
            }
            if (colon == std::string::npos) return RE::TESForm::LookupByID(id);
            return Runtime::LookupForm(id, s.substr(0, colon));
        }

        /** People by plain name: the player, or every named NPC the mills know by it, each as their
         *  reference where they have one, so their key is SkyrimNet's UUID. Mostly that's one person;
         *  a quest's stand-in for them (the Sanctuary's burned Astrid) or a mod's copy shares their
         *  name, and is taken with them. */
        std::vector<RE::TESForm*> PeopleByName(const std::string& name, const Mills::MillSet* set) {
            std::vector<RE::TESForm*> out;
            const auto                want = Lower(name);
            if (want.empty()) return out;
            if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                const char* n = player->GetDisplayFullName();
                if (n && Lower(Text::FromGame(n)) == want) {
                    out.push_back(player);
                    return out;
                }
            }
            if (!set) return out;
            std::vector<RE::FormID> found;
            auto                    consider = [&](RE::FormID base, std::string_view n) {
                if (Lower(n) == want && std::find(found.begin(), found.end(), base) == found.end()) found.push_back(base);
            };
            for (const auto& [k, p] : set->persons) {
                if (p.unique) consider(p.npc, p.name);
            }
            for (const auto& w : set->watch) {  // named people with no mill yet
                if (set->persons.contains(w.base)) continue;
                const auto* npc = RE::TESForm::LookupByID<RE::TESNPC>(w.base);
                if (npc && npc->GetName()) consider(w.base, Text::FromGame(npc->GetName()));
            }
            for (const auto base : found) {
                RE::TESForm* form = nullptr;
                for (const auto& w : set->watch) {
                    if (w.base == base) form = RE::TESForm::LookupByID(w.ref);
                    if (form) break;
                }
                if (!form) form = RE::TESForm::LookupByID(base);
                if (form) out.push_back(form);
            }
            if (out.size() > 1) logger::debug("Rumours: {} people are called {}; taking them all", out.size(), name);
            return out;
        }

        /** An actor field of a request: a form ("Plugin.esp:0xID", "0x14", a number), else a plain
         *  name, which may be several people. Empty for nobody. */
        std::vector<RE::TESForm*> ParseActors(const nlohmann::json& j, const Mills::MillSet* set) {
            const bool isForm = !j.is_string() || [&] {
                const auto s = j.get<std::string>();
                return s.find(':') != std::string::npos || s.starts_with("0x") || s.starts_with("0X");
            }();
            if (!isForm) return PeopleByName(j.get<std::string>(), set);
            if (auto* form = ParseForm(j)) return { form };
            return {};
        }

        std::vector<Subject> SubjectsOf(const nlohmann::json& j, const Mills::MillSet* set) {
            std::vector<Subject> out;
            for (auto* form : ParseActors(j, set)) {
                Subject s;
                s.formId = form->GetFormID();
                s.key = ActorKey(s.formId);
                if (auto* actor = form->As<RE::Actor>()) {
                    s.name = Text::FromGame(actor->GetDisplayFullName());
                } else if (auto* npc = form->As<RE::TESNPC>()) {
                    s.name = Text::FromGame(npc->GetName());
                }
                out.push_back(std::move(s));
            }
            return out;
        }

        /** Someone by form, as the lists keep them: their key and name. */
        Subject SubjectFor(RE::FormID formId) {
            Subject s;
            s.formId = formId;
            s.key = ActorKey(formId);
            if (auto* form = RE::TESForm::LookupByID(formId)) {
                if (auto* actor = form->As<RE::Actor>()) s.name = Text::FromGame(actor->GetDisplayFullName());
                else if (auto* npc = form->As<RE::TESNPC>()) s.name = Text::FromGame(npc->GetName());
            }
            return s;
        }

        /** On the list already, by key or by form. */
        bool Listed(const std::vector<Subject>& list, const Subject& s) {
            return std::any_of(list.begin(), list.end(),
                               [&](const Subject& x) { return (!s.key.empty() && x.key == s.key) || (s.formId && x.formId == s.formId); });
        }

        /** The person by reference or base form. */
        bool Is(const Subject& s, RE::FormID self, RE::FormID selfBase) { return s.formId && (s.formId == self || s.formId == selfBase); }

        /** origin.teller: as stored ({key, form, name}), or as a request gives it (a form or a name). */
        std::optional<Subject> TellerOf(const nlohmann::json& origin, const Mills::MillSet* set) {
            if (!origin.is_object()) return std::nullopt;
            const auto t = origin.find("teller");
            if (t == origin.end() || t->is_null()) return std::nullopt;
            if (t->is_object()) {
                Subject s{ t->value("key", ""), t->value("form", RE::FormID(0)), t->value("name", "") };
                if (s.key.empty() && !s.formId) return std::nullopt;
                return s;
            }
            auto who = SubjectsOf(*t, set);
            if (who.empty()) return std::nullopt;
            return who.front();
        }

        /** The first telling as anyone but its teller should see it: a first-person account says
         *  whose it is. */
        std::string FirstTelling(const Rumour& r) {
            if (!r.teller || r.teller->name.empty()) return r.text;
            return fmt::format("{} has been telling everyone: {}", r.teller->name, r.text);
        }

        /** The mills a request's origin names. */
        std::vector<std::string> OriginMills(const nlohmann::json& origin, const Mills::MillSet& set, std::string& error) {
            std::vector<std::string> out;
            if (!origin.is_object()) {
                error = "origin must be an object";
                return out;
            }
            if (origin.contains("mills")) {
                if (!origin["mills"].is_array()) {
                    error = "origin.mills must be an array";
                    return out;
                }
                for (const auto& m : origin["mills"]) {
                    if (m.is_string() && set.Find(m.get<std::string>())) out.push_back(m.get<std::string>());
                    else error = "unknown mill " + m.dump();
                }
            } else if (origin.contains("hold")) {
                out = set.MillsInHold(origin["hold"].is_string() ? origin["hold"].get<std::string>() : "");
                if (out.empty()) error = "unknown hold " + origin["hold"].dump();
            } else if (origin.contains("settlement")) {
                // A name, or a location in the settlement as a form.
                const auto& j = origin["settlement"];
                const std::string s = j.is_string() ? j.get<std::string>() : "";
                if (j.is_number() || s.find(':') != std::string::npos || s.starts_with("0x") || s.starts_with("0X")) {
                    auto* form = ParseForm(j);
                    if (form && form->Is(RE::FormType::Location)) out = set.PlaceMillsAt(form->GetFormID());
                    if (out.empty()) error = "no settlement at " + j.dump();
                } else {
                    int matches = 0;
                    out = set.MillsInSettlement(s, &matches);
                    if (matches > 1) error = fmt::format("{} settlements are called {}; give a location form", matches, j.dump());
                    else if (out.empty()) error = "unknown settlement " + j.dump();
                }
            } else if (origin.contains("actor")) {
                const auto forms = ParseActors(origin["actor"], &set);
                if (forms.empty()) {
                    error = "unknown actor " + origin["actor"].dump();
                    return out;
                }
                // Everyone by that name. A stand-in waiting disabled for its quest adds nothing.
                for (auto* form : forms) {
                    std::vector<std::string> mills;
                    auto*                    ref = form->As<RE::TESObjectREFR>();
                    if (auto* actor = form->As<RE::Actor>()) mills = set.MillsFor(actor);
                    else if (!ref) mills = set.MillsFor(form->GetFormID());
                    // In nobody's mill (in the party, say): wherever they are now.
                    if (mills.empty() && ref && !ref->IsDisabled()) {
                        if (auto* loc = ref->GetCurrentLocation()) mills = set.MillsForLocation(loc->GetFormID());
                    }
                    out.insert(out.end(), mills.begin(), mills.end());
                }
                if (out.empty()) error = "no mill for actor " + origin["actor"].dump();
            } else if (origin.contains("location")) {
                auto* form = ParseForm(origin["location"]);
                if (!form || !form->Is(RE::FormType::Location)) {
                    error = "unknown location " + origin["location"].dump();
                    return out;
                }
                out = set.MillsForLocation(form->GetFormID());
                if (out.empty()) error = "no mill for location " + origin["location"].dump();
            } else {
                error = "origin needs actor, location, settlement, mills or hold";
            }
            // Only one mill per id, in order.
            std::vector<std::string> unique;
            for (auto& m : out) {
                if (std::find(unique.begin(), unique.end(), m) == unique.end()) unique.push_back(m);
            }
            return unique;
        }

        double FreshDays(const Rumour& r, double notability) {
            return r.freshDays ? *r.freshDays : Settings::BaseFreshDays() + Settings::FreshDaysPerNotability() * notability;
        }

        /** The text a mill gets without retelling: the caller's version for that mill, else for its
         *  hold, else their "*" for everywhere else, else, when the caller asked for no retelling,
         *  the first telling. `fact` says whether the caller marked that version as_fact. */
        std::optional<std::string> GivenFor(const Rumour& r, const std::string& mill, const Mills::MillSet* set,
                                            bool* fact = nullptr) {
            if (fact) *fact = false;
            auto found = [&](const std::string& key, const std::string& text) -> std::optional<std::string> {
                if (fact) *fact = r.givenFact.contains(key);
                return text;
            };
            if (auto g = r.given.find("mill:" + mill); g != r.given.end()) return found(g->first, g->second);
            if (set) {
                for (const auto& [k, text] : r.given) {
                    if (!k.starts_with("hold:")) continue;
                    const auto in = set->MillsInHold(k.substr(5));
                    if (std::find(in.begin(), in.end(), mill) != in.end()) return found(k, text);
                }
            }
            if (auto g = r.given.find("*"); g != r.given.end()) return found(g->first, g->second);
            if (!r.retell) return r.text;
            return std::nullopt;
        }

        /** One of the caller's versions: a string, or { "text": ..., "as_fact": true }. */
        bool ReadGiven(Rumour& r, const std::string& key, const nlohmann::json& v) {
            if (v.is_string()) {
                r.given[key] = v.get<std::string>();
                return true;
            }
            if (!v.is_object() || !v.contains("text") || !v["text"].is_string()) return false;
            r.given[key] = v["text"].get<std::string>();
            if (v.contains("as_fact") && v["as_fact"].is_boolean() && v["as_fact"].get<bool>()) r.givenFact.insert(key);
            return true;
        }

        /** The caller's versions as stored, and as a patch starts from. */
        nlohmann::json GivenJson(const Rumour& r) {
            nlohmann::json out = nlohmann::json::object();
            for (const auto& [k, text] : r.given) {
                if (r.givenFact.contains(k)) out[k] = { { "text", text }, { "as_fact", true } };
                else out[k] = text;
            }
            return out;
        }

        nlohmann::json KnownJson(const Rumour& r) {
            nlohmann::json out = nlohmann::json::array();
            for (const auto& s : r.knownBetterBy) out.push_back({ { "key", s.key }, { "form", s.formId }, { "name", s.name } });
            return out;
        }

        /** Gone stale there: past its fresh days. Tellings only stop gossips raising it unprompted. */
        bool Stale(const Presence& p, double now) { return now >= p.freshUntil; }

        // ---- Who has heard it (ADR-001) ----

        /** The form an actor key ("<uuid>" or "form:XXXXXXXX") stands for, or 0. */
        RE::FormID FormOfKey(const std::string& key) {
            if (key.starts_with("form:")) {
                RE::FormID f = 0;
                std::from_chars(key.data() + 5, key.data() + key.size(), f, 16);
                return f;
            }
            std::uint64_t uuid = 0;
            if (std::from_chars(key.data(), key.data() + key.size(), uuid).ec != std::errc{}) return 0;
            return SkyrimNet::FormOf(uuid);
        }

        /** Who is asking whether they've heard something. */
        struct Listener {
            bool          known = false;  // one of the mills' people; anyone else is taken to know what their mills do
            std::uint64_t roll = 0;
            double        gossip = 0.0;
            RE::FormID    form = 0, base = 0;
        };

        Listener ListenerOf(const Mills::MillSet* set, RE::FormID form, RE::FormID base) {
            Listener who;
            who.form = form;
            who.base = base;
            if (!set) return who;
            const Mills::Person* p = nullptr;
            for (RE::FormID f : { form, base }) {
                if (!f || p) continue;
                if (auto it = set->persons.find(f); it != set->persons.end()) {
                    p = &it->second;
                } else if (auto b = set->baseOfRef.find(f); b != set->baseOfRef.end()) {
                    if (auto pit = set->persons.find(b->second); pit != set->persons.end()) p = &pit->second;
                }
            }
            if (!p) return who;
            who.known = true;
            who.roll = Gossip::RollKey(*p);
            who.gossip = Gossip::ScoreOf(*p);
            if (!who.base && p->key != form) who.base = p->key;
            return who;
        }

        /** Told it, in earshot of a telling, or a witness. Call with g_lock held. */
        bool CertainlyHeard(std::int32_t id, const Listener& who) {
            return (who.form && g_heardForms.contains({ id, who.form })) || (who.base && g_heardForms.contains({ id, who.base }));
        }

        /** Whether the rumour has reached this person in this mill, where it has arrived. */
        bool HeardIn(const Rumour& r, const std::string& mill, const Presence& p, const Listener& who, const Mills::MillSet* set,
                     double now) {
            if (!who.known || !Settings::ReachEnabled()) return true;
            // Cried in the square; their own people's account; or a mill another mod reads out, whose
            // people know what reaches it as soon as it does.
            if (r.scope == Scope::Proclaimed) return true;
            if (auto v = r.versions.find(mill); v != r.versions.end() && v->second.fact) return true;
            if (set && set->IsEager(mill)) return true;
            // The reach stops growing once the talk has gone stale there.
            return HeardRoll(who.roll, r.id) < Reach(p.notability, who.gossip, std::min(now, p.freshUntil) - p.arrived);
        }

        /** A live or promoted rumour. Call with g_lock held. */
        Rumour* FindAny(std::int32_t id) {
            if (auto it = g_rumours.find(id); it != g_rumours.end()) return &it->second;
            if (auto it = g_promoted.find(id); it != g_promoted.end()) return &it->second;
            return nullptr;
        }

        /** The share of the mills' people in the mills it had reached by `day`. */
        double ShareReached(const Rumour& r, const Mills::MillSet* set, double day) {
            if (!set || set->persons.empty()) return 0.0;
            std::unordered_set<RE::FormID> reached;
            for (const auto& [m, p] : r.mills) {
                if (p.arrived > day) continue;
                if (const auto* mill = set->Find(m)) reached.insert(mill->members.begin(), mill->members.end());
            }
            return double(reached.size()) / double(set->persons.size());
        }

        /** The holds of the place mills it had reached by `day`, in mill order. */
        std::vector<RE::FormID> HoldsReached(const Rumour& r, const Mills::MillSet* set, double day) {
            std::vector<RE::FormID> out;
            if (!set) return out;
            for (const auto& [m, p] : r.mills) {
                if (p.arrived > day) continue;
                const auto* mill = set->Find(m);
                if (mill && mill->hold && mill->kind != Mills::Kind::Institution &&
                    std::find(out.begin(), out.end(), mill->hold) == out.end()) {
                    out.push_back(mill->hold);
                }
            }
            return out;
        }

        /** Worth asking about keeping as world knowledge, now it retires (ADR-001, "Lifecycle"):
         *  talk a fair share of people came to hear. Promote.cpp asks whether it will last. */
        bool WorthPromoting(const Rumour& r, bool byRequest, const Mills::MillSet* set, double day) {
            if (!Settings::PromoteEnabled() || r.promote == "never") return false;
            if (r.promote == "always") return true;
            // Its caller took it back: not something people go on knowing.
            if (byRequest) return false;
            return ShareReached(r, set, day) >= Settings::PromoteMinReach();
        }

        nlohmann::json DescribeLocked(const Rumour& r) {
            nlohmann::json subjects = nlohmann::json::array();
            for (const auto& s : r.subjects) subjects.push_back({ { "key", s.key }, { "name", s.name }, { "form", s.formId } });
            nlohmann::json mills = nlohmann::json::array();
            for (const auto& [id, p] : r.mills) {
                auto v = r.versions.find(id);
                mills.push_back({ { "id", id },
                                  { "arrived", p.arrived },
                                  { "fresh_until", p.freshUntil },
                                  { "notability", p.notability },
                                  { "hops", p.hops },
                                  { "airings", p.airings },
                                  { "version", v != r.versions.end() ? v->second.text : FirstTelling(r) },
                                  { "fact", v != r.versions.end() && v->second.fact },
                                  { "names_subjects", v != r.versions.end() ? v->second.names : std::vector<std::string>{} } });
            }
            nlohmann::json out = { { "id", r.id },         { "source", r.source },       { "key", r.key },
                                   { "text", r.text },     { "notability", r.notability }, { "secrecy", r.secrecy }, { "scope", ScopeName(r.scope) },
                                   { "retell_origin", r.retellOrigin }, { "known_better_by", KnownJson(r) },
                                   { "created", r.created }, { "topics", r.topics },     { "subjects", subjects },
                                   { "mills", mills } };
            if (r.teller) out["teller"] = { { "key", r.teller->key }, { "form", r.teller->formId }, { "name", r.teller->name } };
            return out;
        }

        /** Write a rumour's subjects, presences and versions. Caller holds g_lock. */
        void WriteDetails(sqlite3* db, const Rumour& r, double now) {
            Stmt(db, "DELETE FROM rumour_subjects WHERE rumour_id = ?;").Bind(1, r.id).Run();
            for (const auto& s : r.subjects) {
                Stmt(db, "INSERT OR REPLACE INTO rumour_subjects(rumour_id, subject, name, form_id, game_time) VALUES(?,?,?,?,?);")
                    .Bind(1, r.id).Bind(2, s.key).Bind(3, s.name).Bind(4, std::int64_t(s.formId)).Bind(5, now).Run();
            }
            for (const auto& [mill, p] : r.mills) {
                Stmt st(db, "INSERT INTO rumour_mills(rumour_id, mill, parent_mill, hops, notability, fresh_until, airings, arrived, game_time) "
                            "VALUES(?,?,?,?,?,?,?,?,?) ON CONFLICT(rumour_id, mill) DO UPDATE SET "
                            "notability = excluded.notability, fresh_until = excluded.fresh_until;");
                st.Bind(1, r.id).Bind(2, mill);
                if (p.parent.empty()) st.Null(3);
                else st.Bind(3, p.parent);
                st.Bind(4, p.hops).Bind(5, p.notability).Bind(6, p.freshUntil).Bind(7, p.airings).Bind(8, p.arrived)
                    .Bind(9, p.decided).Run();
            }
            Stmt(db, "DELETE FROM versions WHERE rumour_id = ?;").Bind(1, r.id).Run();
            for (const auto& [mill, v] : r.versions) {
                Stmt(db, "INSERT INTO versions(rumour_id, mill, text, names_subjects, model, game_time) VALUES(?,?,?,?,NULL,?);")
                    .Bind(1, r.id).Bind(2, mill).Bind(3, v.text).Bind(4, nlohmann::json(v.names).dump()).Bind(5, now).Run();
            }
        }

        void RetireLocked(std::int32_t id, double now, const char* why, bool byRequest, const Mills::MillSet* set) {
            auto it = g_rumours.find(id);
            if (it == g_rumours.end()) return;
            const bool keep = WorthPromoting(it->second, byRequest, set, now);
            Database::With([&](sqlite3* db) {
                if (keep) {
                    Stmt(db, "UPDATE rumours SET retired_time = ?, promoted_time = ? WHERE id = ?;").Bind(1, now).Bind(2, now).Bind(3, id).Run();
                } else {
                    Stmt(db, "UPDATE rumours SET retired_time = ? WHERE id = ?;").Bind(1, now).Bind(2, id).Run();
                }
            });
            logger::info("Rumour #{} retired ({}){}: {}", id, why, keep ? ", a candidate for world knowledge" : "",
                         it->second.text.substr(0, 100));
            Api::DispatchEvent("retired", nlohmann::json{ { "type", "retired" }, { "id", id }, { "source", it->second.source },
                                                          { "key", it->second.key }, { "why", why }, { "promote", keep } }.dump());
            g_byKey.erase({ it->second.source, it->second.key });
            if (keep) {
                it->second.promoted = now;
                g_promoted[id] = std::move(it->second);
                ++g_promotedVersion;
            }
            g_rumours.erase(it);
        }
    }

    double Now() {
        auto* cal = RE::Calendar::GetSingleton();
        return cal ? cal->GetCurrentGameTime() : 0.0;
    }

    void Clear() {
        std::lock_guard lk(g_lock);
        g_loaded = false;
        g_promoted.clear();
        g_withdrawn.clear();
        g_heardPromoted.clear();
        ++g_promotedVersion;
        g_rumours.clear();
        g_byKey.clear();
        g_heard.clear();
        g_heardForms.clear();
        g_aired.clear();
    }

    double HeardRoll(std::uint64_t who, std::int32_t id) {
        std::uint64_t x = who * 0x9E3779B97F4A7C15ull + (0x48454152ull + static_cast<std::uint64_t>(id)) * 0xBF58476D1CE4E5B9ull;
        x ^= x >> 31;
        x *= 0x94D049BB133111EBull;
        x ^= x >> 29;
        return double(x & 0xFFFFFFFFull) / 4294967296.0;
    }

    double Reach(double notability, double gossip, double days) {
        if (days <= 0.0) return 0.0;
        // Notable talk reaches everyone in time; trivia only the gossips.
        const double ceiling = std::min(1.0, notability + gossip);
        const double rate = Settings::ReachBaseRate() + Settings::ReachGossipRate() * gossip;
        return ceiling * (1.0 - std::exp(-rate * days));
    }

    void Load() {
        auto            set = Mills::Current();
        std::lock_guard lk(g_lock);
        g_rumours.clear();
        g_byKey.clear();
        g_heard.clear();
        g_heardForms.clear();
        g_aired.clear();
        g_promoted.clear();
        g_withdrawn.clear();
        g_heardPromoted.clear();
        ++g_promotedVersion;
        Database::With([&](sqlite3* db) {
            // Once, at the first load that can promote: what had retired before then, and is worth
            // keeping, is kept as if it had just retired.
            if (Settings::PromoteEnabled() && set) {
                Stmt seen(db, "SELECT 1 FROM meta WHERE key = 'promotion_backfill';");
                if (!seen.Row()) {
                    std::map<std::int32_t, Rumour> old;
                    std::map<std::int32_t, double> retired;
                    Stmt q(db, "SELECT id, notability, promote, retired_time FROM rumours WHERE retired_time IS NOT NULL AND "
                               "promoted_time IS NULL;");
                    while (q.Row()) {
                        Rumour x;
                        x.id = static_cast<std::int32_t>(q.I(0));
                        x.notability = q.D(1);
                        x.promote = q.S(2);
                        retired[x.id] = q.D(3);
                        old.emplace(x.id, std::move(x));
                    }
                    Stmt qm(db, "SELECT rumour_id, mill, COALESCE(arrived, game_time) FROM rumour_mills;");
                    while (qm.Row()) {
                        auto it = old.find(static_cast<std::int32_t>(qm.I(0)));
                        if (it == old.end()) continue;
                        Presence p;
                        p.arrived = qm.D(2);
                        it->second.mills[qm.S(1)] = p;
                    }
                    int kept = 0;
                    for (const auto& [id, x] : old) {
                        if (!WorthPromoting(x, false, set.get(), retired[id])) continue;
                        Stmt(db, "UPDATE rumours SET promoted_time = retired_time WHERE id = ?;").Bind(1, id).Run();
                        ++kept;
                    }
                    Stmt(db, "INSERT OR REPLACE INTO meta(key, value) VALUES('promotion_backfill', ?);").Bind(1, std::to_string(kept)).Run();
                    logger::info("Rumours: {} of {} rumours retired before promotion was built are candidates for world knowledge",
                                 kept, old.size());
                }
            }

            Stmt r(db, "SELECT id, source, key, origin, text, topics, notability, scope, retell, fresh_days, promote, game_time, given, "
                       "secrecy, known_better_by, retell_origin, retired_time, promoted_time, promoted_text, promotion_id, lasting "
                       "FROM rumours WHERE retired_time IS NULL OR promoted_time IS NOT NULL;");
            while (r.Row()) {
                Rumour x;
                x.id = static_cast<std::int32_t>(r.I(0));
                x.source = r.S(1);
                x.key = r.S(2);
                x.origin = nlohmann::json::parse(r.S(3), nullptr, false);
                x.teller = TellerOf(x.origin, nullptr);
                x.text = r.S(4);
                auto topics = nlohmann::json::parse(r.S(5), nullptr, false);
                if (topics.is_array()) {
                    for (auto& t : topics) {
                        if (t.is_string()) x.topics.push_back(t.get<std::string>());
                    }
                }
                x.notability = r.D(6);
                x.scope = ParseScope(r.S(7)).value_or(Scope::Spread);
                x.retell = r.I(8) != 0;
                if (!r.IsNull(9)) x.freshDays = r.D(9);
                x.promote = r.S(10);
                x.created = r.D(11);
                auto given = nlohmann::json::parse(r.S(12), nullptr, false);
                if (given.is_object()) {
                    for (auto& [k, v] : given.items()) {
                        ReadGiven(x, k, v);
                    }
                }
                x.secrecy = r.D(13);
                auto known = nlohmann::json::parse(r.S(14), nullptr, false);
                if (known.is_array()) {
                    for (const auto& k : known) {
                        if (!k.is_object()) continue;
                        x.knownBetterBy.push_back({ k.value("key", ""), k.value("form", RE::FormID(0)), k.value("name", "") });
                    }
                }
                x.retellOrigin = r.I(15) != 0;
                if (!r.IsNull(16)) {
                    x.promoted = r.D(17);
                    x.promotedText = r.S(18);
                    x.knowledgeId = r.IsNull(19) ? 0 : static_cast<std::int32_t>(r.I(19));
                    x.lasting = r.IsNull(20) ? -1.0 : r.D(20);
                    g_promoted.emplace(x.id, std::move(x));
                    continue;
                }
                if (!x.key.empty()) g_byKey[{ x.source, x.key }] = x.id;
                g_rumours.emplace(x.id, std::move(x));
            }
            Stmt s(db, "SELECT rumour_id, subject, name, form_id FROM rumour_subjects;");
            while (s.Row()) {
                auto* x = FindAny(static_cast<std::int32_t>(s.I(0)));
                if (!x) continue;
                x->subjects.push_back({ s.S(1), static_cast<RE::FormID>(s.I(3)), s.S(2) });
            }
            Stmt m(db, "SELECT rumour_id, mill, parent_mill, hops, notability, fresh_until, airings, game_time, "
                       "COALESCE(arrived, game_time) FROM rumour_mills;");
            while (m.Row()) {
                auto* x = FindAny(static_cast<std::int32_t>(m.I(0)));
                if (!x) continue;
                Presence p;
                p.parent = m.S(2);
                p.hops = static_cast<int>(m.I(3));
                p.notability = m.D(4);
                p.freshUntil = m.D(5);
                p.airings = static_cast<int>(m.I(6));
                p.decided = m.D(7);
                p.arrived = m.D(8);
                x->mills[m.S(1)] = p;
            }
            Stmt v(db, "SELECT rumour_id, mill, text, names_subjects FROM versions;");
            while (v.Row()) {
                auto* x = FindAny(static_cast<std::int32_t>(v.I(0)));
                if (!x) continue;
                Version ver;
                ver.text = v.S(2);
                auto names = nlohmann::json::parse(v.S(3), nullptr, false);
                if (names.is_array()) {
                    for (auto& n : names) {
                        if (n.is_string()) ver.names.push_back(n.get<std::string>());
                    }
                }
                x->versions[v.S(1)] = std::move(ver);
            }
            Stmt h(db, "SELECT rumour_id, actor FROM heard;");
            while (h.Row()) {
                const auto id = static_cast<std::int32_t>(h.I(0));
                const auto key = h.S(1);
                if (const auto f = FormOfKey(key)) g_heardForms.insert({ id, f });
                g_heard.insert({ id, key });
            }
        });
        // Which versions are the caller's facts: those still as the caller gave them.
        for (auto* rumours : { &g_rumours, &g_promoted }) {
            for (auto& [id, r] : *rumours) {
                for (auto& [m, v] : r.versions) {
                    bool       fact = false;
                    const auto given = GivenFor(r, m, set.get(), &fact);
                    v.fact = fact && given && *given == v.text;
                }
            }
        }
        g_loaded = true;
        std::size_t waiting = 0;
        for (const auto& [id, r] : g_promoted) waiting += r.knowledgeId ? 0 : 1;
        logger::info("Rumours: {} live, {} kept as world knowledge{}", g_rumours.size(), g_promoted.size() - waiting,
                     waiting ? fmt::format(", {} more being considered or written", waiting) : "");
    }

    namespace {
        std::int32_t AddChecked(const nlohmann::json& j, std::string& error, double& catchUpFrom);

        /** The id of the rumour with this source and key, live or retired, or 0. */
        std::int32_t ExistingId(const std::string& source, const std::string& key) {
            std::lock_guard lk(g_lock);
            if (!g_loaded) return 0;
            if (auto it = g_byKey.find({ source, key }); it != g_byKey.end()) return it->second;
            std::int32_t id = 0;
            Database::With([&](sqlite3* db) {
                Stmt st(db, "SELECT id FROM rumours WHERE source = ? AND key = ?;");
                st.Bind(1, source).Bind(2, key);
                if (st.Row()) id = static_cast<std::int32_t>(st.I(0));
            });
            return id;
        }

        /** A patch over the live rumour with this key: its stored request with the patch's fields on
         *  top and the versions merged, a null version removing one. Nullopt if none is live. */
        std::optional<nlohmann::json> Patched(const nlohmann::json& patch, const std::string& source, const std::string& key) {
            std::lock_guard lk(g_lock);
            auto            it = g_byKey.find({ source, key });
            if (it == g_byKey.end()) return std::nullopt;
            const Rumour&  r = g_rumours.at(it->second);
            nlohmann::json out = { { "source", r.source },        { "key", r.key },          { "text", r.text },
                                   { "origin", r.origin },        { "notability", r.notability },
                                   { "secrecy", r.secrecy },      { "scope", std::string(ScopeName(r.scope)) },
                                   { "retell", r.retell },        { "promote", r.promote },  { "topics", r.topics },
                                   { "versions", GivenJson(r) },  { "retell_origin", r.retellOrigin } };
            if (r.freshDays) out["fresh_days"] = *r.freshDays;
            auto known = nlohmann::json::array();
            for (const auto& s : r.knownBetterBy) {
                if (s.formId) known.push_back(s.formId);
            }
            out["known_better_by"] = known;
            auto subjects = nlohmann::json::array();
            for (const auto& s : r.subjects) {
                if (s.formId) subjects.push_back(s.formId);
            }
            out["subjects"] = subjects;
            for (const auto& [k, v] : patch.items()) {
                if (k == "versions" && v.is_object()) {
                    if (!out["versions"].is_object()) out["versions"] = nlohmann::json::object();
                    for (const auto& [vk, vv] : v.items()) {
                        if (vv.is_null()) out["versions"].erase(vk);
                        else out["versions"][vk] = vv;
                    }
                } else if (k != "patch") {
                    out[k] = v;
                }
            }
            return out;
        }
    }

    std::int32_t Add(const std::string& requestJson, std::string& error) {
        try {
            // Papyrus strings and ModEvents are in the game's code page, and C++ callers may pass game names.
            auto j = nlohmann::json::parse(Text::FromGame(requestJson), nullptr, false);
            if (j.is_discarded() || !j.is_object()) {
                error = "the request is not a JSON object";
                return 0;
            }
            const auto source = j.contains("source") && j["source"].is_string() ? j["source"].get<std::string>() : std::string{};
            const auto key = j.contains("key") && j["key"].is_string() ? j["key"].get<std::string>() : std::string{};
            if (!source.empty() && !key.empty()) {
                // Safe to send again: a caller can resend everything recent at every load.
                if (j.value("if_absent", false)) {
                    if (const auto id = ExistingId(source, key)) {
                        logger::debug("Rumour #{} already exists for {} key {}; left as it is", id, source, key);
                        return id;
                    }
                }
                if (j.value("patch", false)) {
                    if (auto merged = Patched(j, source, key)) j = std::move(*merged);
                }
            }
            double     catchUpFrom = -1.0;
            const auto id = AddChecked(j, error, catchUpFrom);
            if (id && catchUpFrom >= 0.0) Spread::CatchUp(id, catchUpFrom);
            return id;
        } catch (const std::exception& e) {
            // A field of the wrong type, mostly.
            error = std::string("bad request: ") + e.what();
            return 0;
        }
    }

    namespace {
    std::int32_t AddChecked(const nlohmann::json& j, std::string& error, double& catchUpFrom) {
        auto str = [&](const char* k) { return j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : std::string{}; };

        Rumour r;
        r.source = str("source");
        r.key = str("key");
        r.text = str("text");
        if (r.source.empty()) {
            error = "source is required";
            return 0;
        }
        if (r.text.empty()) {
            error = "text is required";
            return 0;
        }
        r.notability = std::clamp(j.value("notability", 0.5), 0.0, 1.0);
        r.secrecy = std::clamp(j.value("secrecy", 0.0), 0.0, 1.0);
        const auto scope = ParseScope(str("scope"));
        if (!scope) {
            error = "scope must be spread, proclaimed or local";
            return 0;
        }
        r.scope = *scope;
        r.retell = j.value("retell", true);
        r.retellOrigin = j.value("retell_origin", false);
        if (j.contains("fresh_days") && j["fresh_days"].is_number() && j["fresh_days"].get<double>() > 0) {
            r.freshDays = j["fresh_days"].get<double>();
        }
        r.promote = j.contains("promote") && j["promote"].is_string() ? j["promote"].get<std::string>() : "auto";
        if (j.contains("topics") && j["topics"].is_array()) {
            for (auto& t : j["topics"]) {
                if (t.is_string()) r.topics.push_back(t.get<std::string>());
            }
        }
        auto set = Mills::Current();
        if (!set) {
            error = "mills are not built yet";
            return 0;
        }
        if (j.contains("subjects") && j["subjects"].is_array()) {
            for (auto& s : j["subjects"]) {
                auto who = SubjectsOf(s, set.get());
                if (who.empty()) logger::warn("AddRumour ({}): unknown subject {} (not a form, and nobody's name)", r.source, s.dump());
                r.subjects.insert(r.subjects.end(), who.begin(), who.end());
            }
        }
        // People a client gives a fuller account of it, so it stays out of their talk.
        if (j.contains("known_better_by") && j["known_better_by"].is_array()) {
            for (auto& k : j["known_better_by"]) {
                auto who = SubjectsOf(k, set.get());
                if (who.empty()) logger::warn("AddRumour ({}): unknown known_better_by {} (not a form, and nobody's name)", r.source, k.dump());
                r.knownBetterBy.insert(r.knownBetterBy.end(), who.begin(), who.end());
            }
        }
        r.origin = j.value("origin", nlohmann::json::object());
        // Someone's own account in the first person: they tell it, and their own mills hear it
        // retold, as everyone else does. Stored with the origin, as the lists keep people. They
        // know it better than the talk, too.
        if (r.origin.is_object() && r.origin.contains("teller")) {
            r.teller = TellerOf(r.origin, set.get());
            if (r.teller) {
                r.origin["teller"] = { { "key", r.teller->key }, { "form", r.teller->formId }, { "name", r.teller->name } };
                r.retellOrigin = true;
                if (!Listed(r.knownBetterBy, *r.teller)) r.knownBetterBy.push_back(*r.teller);
            } else {
                logger::warn("AddRumour ({}): unknown teller {} (not a form, and nobody's name)", r.source, r.origin["teller"].dump());
                r.origin.erase("teller");
            }
        }
        auto targets = OriginMills(r.origin, *set, error);
        if (targets.empty()) {
            if (error.empty()) error = "the origin names no mill";
            return 0;
        }
        error.clear();
        // Mills that know it first-hand without being where it started: a faction that learned of it.
        std::vector<std::string> reach;
        if (j.contains("reach") && j["reach"].is_array()) {
            for (const auto& m : j["reach"]) {
                if (!m.is_string() || !set->Find(m.get<std::string>())) {
                    logger::warn("AddRumour ({}): unknown reach mill {}", r.source, m.dump());
                    continue;
                }
                const auto id = m.get<std::string>();
                if (std::find(targets.begin(), targets.end(), id) == targets.end() &&
                    std::find(reach.begin(), reach.end(), id) == reach.end()) {
                    reach.push_back(id);
                }
            }
        }
        // A spread rumour hops on from here (Spread.cpp); a proclaimed one reaches every
        // other mill once news could have travelled there from the nearest origin mill.
        std::map<std::string, std::pair<std::string, double>> proclaimed;  // mill -> (from, days)
        if (r.scope == Scope::Proclaimed) {
            for (const auto& m : set->mills) {
                if (std::find(targets.begin(), targets.end(), m.id) != targets.end()) continue;
                std::pair<std::string, double> best{ "", 1e9 };
                for (const auto& o : targets) {
                    const double d = Spread::TravelDays(*set, o, m.id);
                    if (d < best.second) best = { o, d };
                }
                proclaimed[m.id] = best;
            }
        }

        // The caller's own tellings, by mill or hold. Kept, for mills the rumour reaches later.
        if (j.contains("versions") && j["versions"].is_object()) {
            for (auto& [k, v] : j["versions"].items()) {
                if (v.is_null()) continue;
                if (!k.starts_with("mill:") && !k.starts_with("hold:") && k != "*") {
                    logger::warn("AddRumour ({}): version key {} is not mill:, hold: or *", r.source, k);
                } else if (!ReadGiven(r, k, v)) {
                    logger::warn("AddRumour ({}): version {} is neither text nor {{\"text\", \"as_fact\"}}", r.source, k);
                }
            }
        }

        // Game hours before anyone at the origin knows: news of an event takes a while
        // to get round even the town it happened in.
        double delay = 0.0;
        if (j.contains("delay_hours") && j["delay_hours"].is_number()) delay = std::max(0.0, j["delay_hours"].get<double>()) / 24.0;

        // People who certainly know it, whatever their mill: witnesses, the one who remembers it.
        std::vector<std::string> heardBy;
        if (j.contains("heard_by") && j["heard_by"].is_array()) {
            for (const auto& h : j["heard_by"]) {
                const auto who = ParseActors(h, set.get());
                if (who.empty()) logger::warn("AddRumour ({}): unknown heard_by {} (not a form, and nobody's name)", r.source, h.dump());
                for (auto* f : who) heardBy.push_back(ActorKey(f->GetFormID()));
            }
        }
        for (const auto& s : r.knownBetterBy) heardBy.push_back(s.key);

        const double now = Now();
        const double fresh = FreshDays(r, r.notability);
        // When it happened, for news sent late: a new rumour starts then, and has spread since as if
        // it had been there all along. At most a month back.
        double start = now;
        if (j.contains("happened") && j["happened"].is_number()) start = std::clamp(j["happened"].get<double>(), now - 30.0, now);

        std::lock_guard lk(g_lock);
        if (!g_loaded) {
            error = "no save's database is open yet";
            return 0;
        }

        // The same source and key update the rumour in place.
        Rumour* target = nullptr;
        if (!r.key.empty()) {
            if (auto it = g_byKey.find({ r.source, r.key }); it != g_byKey.end()) target = &g_rumours[it->second];
        }
        bool         ok = true;
        std::int32_t id = target ? target->id : 0;
        if (!target && !r.key.empty()) {
            // A retired one with this key comes back.
            Database::With([&](sqlite3* db) {
                Stmt st(db, "SELECT id, game_time FROM rumours WHERE source = ? AND key = ?;");
                st.Bind(1, r.source).Bind(2, r.key);
                if (st.Row()) {
                    id = static_cast<std::int32_t>(st.I(0));
                    r.created = st.D(1);
                }
            });
        }

        // While the story stays the same an update keeps the retellings; the caller's own versions are
        // always taken afresh, so one dropped from the request goes.
        std::map<std::string, Version>  retold;
        if (target && target->text == r.text) {
            for (const auto& [m, v] : target->versions) {
                if (!GivenFor(*target, m, set.get())) retold[m] = v;
            }
        }
        const bool revived = id && !target;

        Rumour& out = target ? *target : r;
        if (target) {
            out.text = r.text;
            out.origin = r.origin;
            out.topics = r.topics;
            out.notability = r.notability;
            out.secrecy = r.secrecy;
            out.scope = r.scope;
            out.retell = r.retell;
            out.freshDays = r.freshDays;
            out.promote = r.promote;
            out.subjects = r.subjects;
            out.given = r.given;
            out.givenFact = r.givenFact;
            out.retellOrigin = r.retellOrigin;
            out.knownBetterBy = r.knownBetterBy;
            out.teller = r.teller;
        } else if (!id) {
            out.created = start;
        }
        const auto millsBefore = out.mills.size();
        for (const auto& m : targets) {
            auto& p = out.mills[m];
            if (p.arrived == 0.0) {
                p.decided = start;
                p.arrived = start + delay;
            }
            p.freshUntil = std::max(p.freshUntil, p.arrived + fresh);
            p.notability = out.notability;
        }
        // A mill it reaches knows it from then, with no delay: the faction was told, or was there.
        for (const auto& m : reach) {
            auto& p = out.mills[m];
            if (p.arrived == 0.0) {
                p.decided = start;
                p.arrived = start;
            }
            p.freshUntil = std::max(p.freshUntil, p.arrived + fresh);
            p.notability = out.notability;
        }
        for (const auto& [m, from] : proclaimed) {
            auto& p = out.mills[m];
            if (p.arrived == 0.0) {
                p.decided = start;
                p.arrived = start + delay + from.second;
                p.parent = from.first;
                p.hops = 1;
            }
            p.freshUntil = std::max(p.freshUntil, p.arrived + fresh);
            p.notability = out.notability;
        }
        // The origin mills tell it as given; elsewhere the caller's version, the retelling already
        // there if the story hasn't changed, or a retelling once someone there needs it.
        // With retell_origin the origin's version is written from the text too, as hop 0.
        const bool added = out.mills.size() > millsBefore;
        out.versions.clear();
        for (const auto& [m, p] : out.mills) {
            const bool origin = std::find(targets.begin(), targets.end(), m) != targets.end() ||
                                std::find(reach.begin(), reach.end(), m) != reach.end();
            bool       fact = false;
            auto       text = GivenFor(out, m, set.get(), &fact);
            if (!text && origin && !out.retellOrigin) text = out.text;
            if (!text) {
                if (auto kept = retold.find(m); kept != retold.end()) text = kept->second.text;
            }
            if (!text) continue;
            Version v;
            v.text = *text;
            v.names = NamedSubjects(v.text, out.subjects);
            v.fact = fact;
            out.versions[m] = std::move(v);
        }

        Database::With([&](sqlite3* db) {
            Stmt(db, "BEGIN;").Run();
            if (id) {
                Stmt st(db, "UPDATE rumours SET origin = ?, text = ?, topics = ?, notability = ?, scope = ?, retell = ?, "
                            "fresh_days = ?, promote = ?, given = ?, secrecy = ?, known_better_by = ?, retell_origin = ?, "
                            "retired_time = NULL, promoted_time = NULL, promoted_text = NULL, promotion_id = NULL, lasting = NULL "
                            "WHERE id = ?;");
                st.Bind(1, out.origin.dump()).Bind(2, out.text).Bind(3, nlohmann::json(out.topics).dump())
                    .Bind(4, out.notability).Bind(5, std::string(ScopeName(out.scope))).Bind(6, out.retell ? 1 : 0);
                if (out.freshDays) st.Bind(7, *out.freshDays);
                else st.Null(7);
                st.Bind(8, out.promote).Bind(9, GivenJson(out).dump()).Bind(10, out.secrecy).Bind(11, KnownJson(out).dump())
                    .Bind(12, out.retellOrigin ? 1 : 0).Bind(13, id);
                ok = st.Run();
            } else {
                Stmt st(db, "INSERT INTO rumours(source, key, origin, text, topics, notability, scope, retell, fresh_days, promote, given, "
                            "game_time, secrecy, known_better_by, retell_origin) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);");
                st.Bind(1, out.source);
                if (out.key.empty()) st.Null(2);
                else st.Bind(2, out.key);
                st.Bind(3, out.origin.dump()).Bind(4, out.text).Bind(5, nlohmann::json(out.topics).dump())
                    .Bind(6, out.notability).Bind(7, std::string(ScopeName(out.scope))).Bind(8, out.retell ? 1 : 0);
                if (out.freshDays) st.Bind(9, *out.freshDays);
                else st.Null(9);
                st.Bind(10, out.promote).Bind(11, GivenJson(out).dump()).Bind(12, out.created).Bind(13, out.secrecy)
                    .Bind(14, KnownJson(out).dump()).Bind(15, out.retellOrigin ? 1 : 0);
                ok = st.Run();
                if (ok) id = static_cast<std::int32_t>(sqlite3_last_insert_rowid(db));
            }
            if (ok) {
                out.id = id;
                // Brought back: it starts again from the origin, not from where it had got to.
                if (revived) Stmt(db, "DELETE FROM rumour_mills WHERE rumour_id = ?;").Bind(1, id).Run();
                WriteDetails(db, out, now);
                for (const auto& k : heardBy) {
                    Stmt(db, "INSERT OR IGNORE INTO heard(rumour_id, actor, game_time) VALUES(?,?,?);").Bind(1, id).Bind(2, k).Bind(3, now).Run();
                }
            }
            Stmt(db, ok ? "COMMIT;" : "ROLLBACK;").Run();
        });
        if (!ok || !id) {
            error = "the database refused it";
            return 0;
        }
        // News again: what people had come to know of it gives way to the talk.
        if (revived) {
            if (auto p = g_promoted.find(id); p != g_promoted.end()) {
                if (p->second.knowledgeId) g_withdrawn.push_back(p->second.knowledgeId);
                g_promoted.erase(p);
                ++g_promotedVersion;
                logger::info("Rumour #{} is news again, so its world knowledge is taken out", id);
            }
        }
        for (const auto& k : heardBy) {
            g_heard.insert({ id, k });
            if (const auto f = FormOfKey(k)) g_heardForms.insert({ id, f });
        }

        const bool updated = target != nullptr;
        if (!target) {
            out.id = id;
            if (!out.key.empty()) g_byKey[{ out.source, out.key }] = id;
            g_rumours[id] = std::move(r);
        }
        const Rumour& stored = g_rumours[id];
        const double heard = stored.mills.at(targets.front()).arrived;
        logger::info("Rumour #{} {} by {}{}: n={:.2f}{}, {}, in {}{}: {}", id, updated ? "updated" : "added", stored.source,
                     stored.key.empty() ? "" : " (key " + stored.key + ")", stored.notability,
                     stored.secrecy > 0 ? fmt::format(", secrecy {:.2f}", stored.secrecy) : "", ScopeName(stored.scope),
                     targets.size() == 1 ? targets.front() : fmt::format("{} mills", targets.size()),
                     heard > now ? fmt::format(" from day {:.2f}", heard)
                     : heard < now - 0.01 ? fmt::format(" since day {:.2f}", heard) : "",
                     stored.text.substr(0, 120));
        nlohmann::json mills = targets;
        Api::DispatchEvent("seeded", nlohmann::json{ { "type", "seeded" }, { "id", id }, { "source", stored.source },
                                                     { "key", stored.key }, { "updated", updated }, { "mills", mills },
                                                     { "arrives", heard } }.dump());
        // Backdated: the hops it would have made by now, made now. An update only replays when it
        // added mills, so sending the same request again changes nothing.
        if (start < now && stored.scope == Scope::Spread && (!updated || added)) catchUpFrom = start;
        return id;
    }
    }

    bool Retire(std::int32_t id, std::string& error) {
        auto            set = Mills::Current();
        std::lock_guard lk(g_lock);
        if (!g_rumours.contains(id)) {
            error = "no live rumour #" + std::to_string(id);
            return false;
        }
        RetireLocked(id, Now(), "by request", true, set.get());
        return true;
    }

    bool RetireByKey(const std::string& source, const std::string& key, std::string& error) {
        auto            set = Mills::Current();
        std::lock_guard lk(g_lock);
        auto            it = g_byKey.find({ source, key });
        if (it == g_byKey.end()) {
            error = "no live rumour from " + source + " with key " + key;
            return false;
        }
        RetireLocked(it->second, Now(), "by request", true, set.get());
        return true;
    }

    std::optional<nlohmann::json> Describe(std::int32_t id) {
        std::lock_guard lk(g_lock);
        auto            it = g_rumours.find(id);
        if (it == g_rumours.end()) return std::nullopt;
        return DescribeLocked(it->second);
    }

    std::optional<nlohmann::json> DescribeByKey(const std::string& source, const std::string& key) {
        std::lock_guard lk(g_lock);
        auto            it = g_byKey.find({ source, key });
        if (it == g_byKey.end()) return std::nullopt;
        return DescribeLocked(g_rumours.at(it->second));
    }

    std::vector<Heard> InMills(const std::vector<std::string>& mills, RE::FormID self, RE::FormID selfBase, double now,
                               std::size_t max) {
        struct Ranked {
            Heard h;
            bool  faction = false;  // their guild's or faction's telling
            bool  local = false;
        };
        std::vector<Ranked> found;
        std::vector<std::pair<std::int32_t, std::string>> unwritten;
        auto                set = Mills::Current();
        // Where several of their mills have it, a guild's or faction's telling beats the street's.
        std::vector<std::string> order;
        for (const auto& m : mills) {
            const auto* mill = set ? set->Find(m) : nullptr;
            if (mill && mill->kind == Mills::Kind::Institution) order.push_back(m);
        }
        for (const auto& m : mills) {
            if (std::find(order.begin(), order.end(), m) == order.end()) order.push_back(m);
        }
        const auto who = ListenerOf(set.get(), self, selfBase);
        {
            std::lock_guard lk(g_lock);
            for (const auto& [id, r] : g_rumours) {
                // A client gives them a fuller account of it.
                if (std::any_of(r.knownBetterBy.begin(), r.knownBetterBy.end(),
                                [&](const Subject& s) { return s.formId && (s.formId == self || s.formId == selfBase); })) {
                    continue;
                }
                const bool certain = CertainlyHeard(id, who);
                for (const auto& m : order) {
                    auto p = r.mills.find(m);
                    if (p == r.mills.end() || Stale(p->second, now) || p->second.arrived > now) continue;
                    // In their mill, but it hasn't got to them yet.
                    if (!certain && !HeardIn(r, m, p->second, who, set.get(), now)) continue;
                    auto v = r.versions.find(m);
                    if (v == r.versions.end()) {
                        // Still to be written. Their other mills may have it.
                        unwritten.emplace_back(id, m);
                        continue;
                    }
                    Ranked x;
                    x.h.id = id;
                    x.h.mill = m;
                    if (const auto* mill = set ? set->Find(m) : nullptr; mill && mill->kind == Mills::Kind::Institution) x.faction = true;
                    x.h.arrived = p->second.arrived;
                    x.h.notability = p->second.notability;
                    x.h.secrecy = r.secrecy;
                    x.h.airings = p->second.airings;
                    x.h.fact = v->second.fact;
                    x.h.text = v->second.text;
                    const auto& names = v->second.names;
                    for (const auto& s : r.subjects) {
                        const bool named = std::find(names.begin(), names.end(), s.key) != names.end();
                        if (named) x.h.named.push_back(s.formId);
                        if (named && s.formId && (s.formId == self || s.formId == selfBase)) x.h.aboutThem = true;
                        if (set && !x.local) {
                            for (const auto& sm : set->MillsFor(s.formId)) {
                                if (std::find(mills.begin(), mills.end(), sm) != mills.end()) x.local = true;
                            }
                        }
                    }
                    found.push_back(std::move(x));
                    break;
                }
            }
        }
        for (const auto& [id, m] : unwritten) Retell::Request(id, m);
        // What concerns them comes first: talk about them, then their own faction's, then the freshest.
        std::sort(found.begin(), found.end(), [](const Ranked& a, const Ranked& b) {
            if (a.h.aboutThem != b.h.aboutThem) return a.h.aboutThem;
            if (a.faction != b.faction) return a.faction;
            if (a.h.arrived != b.h.arrived) return a.h.arrived > b.h.arrived;
            if (a.h.notability != b.h.notability) return a.h.notability > b.h.notability;
            return a.local > b.local;
        });
        std::vector<Heard> out;
        for (auto& f : found) {
            if (out.size() >= max) break;
            out.push_back(std::move(f.h));
        }
        return out;
    }

    bool HasHeard(std::int32_t id, RE::FormID formId) {
        auto set = Mills::Current();
        const auto mills = set ? set->MillsFor(formId) : std::vector<std::string>{};
        const auto key = ActorKey(formId);
        const auto who = ListenerOf(set.get(), formId, 0);
        std::lock_guard lk(g_lock);
        auto            it = g_rumours.find(id);
        if (it == g_rumours.end()) return false;
        if (g_heard.contains({ id, key }) || CertainlyHeard(id, who)) return true;
        const double now = Now();
        for (const auto& m : mills) {
            auto p = it->second.mills.find(m);
            if (p != it->second.mills.end() && p->second.arrived <= now && HeardIn(it->second, m, p->second, who, set.get(), now)) {
                return true;
            }
        }
        return false;
    }

    void NoteAiring(std::int32_t id, const std::string& mill, RE::FormID teller, RE::FormID listener,
                    const std::vector<RE::FormID>& earshot, double now) {
        std::vector<std::string> keys;
        for (auto f : earshot) keys.push_back(ActorKey(f));
        const auto tellerKey = ActorKey(teller);
        const auto listenerKey = listener ? ActorKey(listener) : std::string{};

        std::lock_guard lk(g_lock);
        auto            it = g_rumours.find(id);
        if (it == g_rumours.end()) return;
        g_aired[id] = { std::chrono::steady_clock::now(), teller };
        auto p = it->second.mills.find(mill);
        if (p != it->second.mills.end()) p->second.airings++;
        for (const auto& k : keys) g_heard.insert({ id, k });
        for (auto f : earshot) g_heardForms.insert({ id, f });

        Database::With([&](sqlite3* db) {
            Stmt(db, "BEGIN;").Run();
            Stmt st(db, "INSERT INTO airings(rumour_id, mill, teller, listener, earshot, game_time) VALUES(?,?,?,?,?,?);");
            st.Bind(1, id).Bind(2, mill).Bind(3, tellerKey);
            if (listenerKey.empty()) st.Null(4);
            else st.Bind(4, listenerKey);
            st.Bind(5, nlohmann::json(keys).dump()).Bind(6, now).Run();
            Stmt(db, "UPDATE rumour_mills SET airings = airings + 1 WHERE rumour_id = ? AND mill = ?;").Bind(1, id).Bind(2, mill).Run();
            for (const auto& k : keys) {
                Stmt(db, "INSERT OR IGNORE INTO heard(rumour_id, actor, game_time) VALUES(?,?,?);").Bind(1, id).Bind(2, k).Bind(3, now).Run();
            }
            Stmt(db, "COMMIT;").Run();
        });
        Api::DispatchEvent("aired", nlohmann::json{ { "type", "aired" }, { "id", id }, { "mill", mill }, { "teller", tellerKey },
                                                    { "listener", listenerKey }, { "earshot", keys } }.dump());
    }

    bool AiredByOtherRecently(std::int32_t id, RE::FormID asker) {
        const int minutes = Settings::ItemCooldownMinutes();
        if (minutes <= 0) return false;
        std::lock_guard lk(g_lock);
        auto            it = g_aired.find(id);
        if (it == g_aired.end() || it->second.teller == asker) return false;
        return std::chrono::steady_clock::now() - it->second.at < std::chrono::minutes(minutes);
    }

    void CarryHome(std::int32_t id, const std::string& mill, const std::vector<RE::FormID>& carriers, double now) {
        auto set = Mills::Current();
        if (!set) return;
        double n0 = 0.0;
        int    hops = 0;
        {
            std::lock_guard lk(g_lock);
            auto            it = g_rumours.find(id);
            if (it == g_rumours.end() || it->second.scope != Scope::Spread) return;
            auto p = it->second.mills.find(mill);
            if (p == it->second.mills.end()) return;
            n0 = p->second.notability;
            hops = p->second.hops;
        }
        const auto* from = set->Find(mill);
        std::vector<Arrival> out;
        for (auto c : carriers) {
            const auto mills = set->MillsFor(c);
            if (mills.empty() || mills.front() == mill) continue;
            const auto& home = mills.front();  // institutions hear it from them by the usual hops
            if (std::any_of(out.begin(), out.end(), [&](const Arrival& a) { return a.mill == home; })) continue;
            const auto* h = set->Find(home);
            const bool  local = from && h && from->settlement && from->settlement == h->settlement;
            const double n = n0 * (local ? Settings::HopDecayLocal() : Settings::HopDecayTravel());
            if (n < Settings::NotabilityFloor()) continue;
            Arrival a;
            a.id = id;
            a.mill = home;
            a.parent = mill;
            a.decided = now;
            a.arrives = now + Spread::TravelDays(*set, mill, home);
            a.notability = n;
            a.hops = hops + 1;
            out.push_back(a);
        }
        // Mills that already have it are skipped there.
        if (!out.empty()) AddArrivals(out);
    }

    void Reinforce(std::int32_t id, const std::vector<std::string>& mills, RE::FormID witness, double now, bool firstHand) {
        auto            set = Mills::Current();
        const auto      key = witness ? ActorKey(witness) : std::string{};
        const Subject   was = firstHand && witness ? SubjectFor(witness) : Subject{};
        std::lock_guard lk(g_lock);
        auto            it = g_rumours.find(id);
        if (it == g_rumours.end()) return;
        Rumour& r = it->second;
        // They were there: their own memory of it, not the talk, is what they go by.
        std::string knownJson;
        if (firstHand && witness && !Listed(r.knownBetterBy, was)) {
            r.knownBetterBy.push_back(was);
            knownJson = KnownJson(r).dump();
        }
        std::vector<std::string> refreshed, reached;
        for (const auto& m : mills) {
            auto p = r.mills.find(m);
            if (p != r.mills.end()) {
                if (p->second.arrived > now) continue;  // on its way; the witness will be there first, but no matter
                const double until = now + FreshDays(r, p->second.notability);
                if (until > p->second.freshUntil) {
                    p->second.freshUntil = until;
                    refreshed.push_back(m);
                }
                continue;
            }
            if (r.scope == Scope::Local || static_cast<int>(r.mills.size()) >= Settings::MaxMillsPerRumour()) continue;
            // They saw it themselves, so their mill has it first-hand: the first telling, not a retelling.
            Presence np;
            np.arrived = np.decided = now;
            np.notability = r.notability;
            np.freshUntil = now + FreshDays(r, np.notability);
            r.mills[m] = np;
            bool fact = false;
            auto given = GivenFor(r, m, set.get(), &fact);
            // Someone's own first-person account is theirs alone to tell: the mill has it retold.
            if (given || !r.teller) {
                Version v;
                v.text = given.value_or(r.text);
                v.names = NamedSubjects(v.text, r.subjects);
                v.fact = fact;
                r.versions[m] = std::move(v);
            }
            reached.push_back(m);
        }
        if (!key.empty()) {
            g_heard.insert({ id, key });
            g_heardForms.insert({ id, witness });
        }
        Database::With([&](sqlite3* db) {
            Stmt(db, "BEGIN;").Run();
            for (const auto& m : refreshed) {
                Stmt(db, "UPDATE rumour_mills SET fresh_until = ? WHERE rumour_id = ? AND mill = ?;")
                    .Bind(1, r.mills.at(m).freshUntil).Bind(2, id).Bind(3, m).Run();
            }
            for (const auto& m : reached) {
                const auto& p = r.mills.at(m);
                Stmt(db, "INSERT OR IGNORE INTO rumour_mills(rumour_id, mill, parent_mill, hops, notability, fresh_until, airings, "
                         "arrived, game_time) VALUES(?,?,NULL,0,?,?,0,?,?);")
                    .Bind(1, id).Bind(2, m).Bind(3, p.notability).Bind(4, p.freshUntil).Bind(5, p.arrived).Bind(6, p.decided).Run();
                const auto v = r.versions.find(m);
                if (v == r.versions.end()) continue;
                Stmt(db, "INSERT OR REPLACE INTO versions(rumour_id, mill, text, names_subjects, model, game_time) VALUES(?,?,?,?,'witness',?);")
                    .Bind(1, id).Bind(2, m).Bind(3, v->second.text).Bind(4, nlohmann::json(v->second.names).dump()).Bind(5, now).Run();
            }
            if (!key.empty()) {
                Stmt(db, "INSERT OR IGNORE INTO heard(rumour_id, actor, game_time) VALUES(?,?,?);").Bind(1, id).Bind(2, key).Bind(3, now).Run();
            }
            if (!knownJson.empty()) Stmt(db, "UPDATE rumours SET known_better_by = ? WHERE id = ?;").Bind(1, knownJson).Bind(2, id).Run();
            Stmt(db, "COMMIT;").Run();
        });
        if (!knownJson.empty()) logger::info("Rumour #{}: {} was there, so it stays out of their own talk", id, was.name);
        for (const auto& m : reached) {
            logger::info("Rumour #{} reaches {} first-hand, through a witness", id, m);
            Api::DispatchEvent("arrived", nlohmann::json{ { "type", "arrived" }, { "id", id }, { "mill", m } }.dump());
        }
        if (!refreshed.empty()) logger::info("Rumour #{} is fresh again in {} mill(s), through a witness", id, refreshed.size());
    }

    std::vector<std::pair<std::int32_t, std::string>> KnownBy(RE::FormID formId, const std::vector<std::string>& mills,
                                                              double now, std::size_t max) {
        const auto      key = ActorKey(formId);
        auto            set = Mills::Current();
        const auto      who = ListenerOf(set.get(), formId, 0);
        std::lock_guard lk(g_lock);
        struct K {
            std::int32_t id;
            std::string  text;
            double       arrived;
        };
        std::vector<K> found;
        for (const auto& [id, r] : g_rumours) {
            std::optional<std::string> text;
            double                     arrived = 0.0;
            const bool                 certain = g_heard.contains({ id, key }) || CertainlyHeard(id, who);
            for (const auto& m : mills) {
                auto p = r.mills.find(m);
                if (p == r.mills.end() || p->second.arrived > now || Stale(p->second, now)) continue;
                if (!certain && !HeardIn(r, m, p->second, who, set.get(), now)) continue;
                auto v = r.versions.find(m);
                text = v != r.versions.end() ? v->second.text : FirstTelling(r);
                arrived = p->second.arrived;
                break;
            }
            if (!text && certain) {
                text = FirstTelling(r);
                arrived = r.created;
            }
            if (text) found.push_back({ id, std::move(*text), arrived });
        }
        std::sort(found.begin(), found.end(), [](const K& a, const K& b) { return a.arrived > b.arrived; });
        std::vector<std::pair<std::int32_t, std::string>> out;
        for (auto& f : found) {
            if (out.size() >= max) break;
            out.emplace_back(f.id, std::move(f.text));
        }
        return out;
    }

    bool IsLive(std::int32_t id) {
        std::lock_guard lk(g_lock);
        return g_rumours.contains(id);
    }

    int AddKnownBetter(const std::vector<std::pair<std::int32_t, RE::FormID>>& who) {
        std::vector<std::pair<std::int32_t, Subject>> people;
        for (const auto& [id, form] : who) {
            if (id && form) people.emplace_back(id, SubjectFor(form));
        }
        const double    now = Now();
        std::lock_guard lk(g_lock);
        std::set<std::int32_t>                            changed;
        std::vector<std::pair<std::int32_t, std::string>> heard;
        int                                               added = 0;
        for (const auto& [id, s] : people) {
            auto it = g_rumours.find(id);
            if (it == g_rumours.end() || Listed(it->second.knownBetterBy, s)) continue;
            it->second.knownBetterBy.push_back(s);
            changed.insert(id);
            ++added;
            if (!s.key.empty() && g_heard.insert({ id, s.key }).second) heard.emplace_back(id, s.key);
            g_heardForms.insert({ id, s.formId });
        }
        if (changed.empty()) return 0;
        Database::With([&](sqlite3* db) {
            Stmt(db, "BEGIN;").Run();
            for (auto id : changed) {
                Stmt(db, "UPDATE rumours SET known_better_by = ? WHERE id = ?;").Bind(1, KnownJson(g_rumours.at(id)).dump()).Bind(2, id).Run();
            }
            for (const auto& [id, key] : heard) {
                Stmt(db, "INSERT OR IGNORE INTO heard(rumour_id, actor, game_time) VALUES(?,?,?);").Bind(1, id).Bind(2, key).Bind(3, now).Run();
            }
            Stmt(db, "COMMIT;").Run();
        });
        return added;
    }

    std::vector<Heard> FirstHand(const std::vector<std::string>& mills, RE::FormID self, RE::FormID selfBase, double now,
                                 std::size_t max) {
        std::vector<Heard>                                out;
        std::vector<std::pair<std::int32_t, std::string>> unwritten;
        {
            std::lock_guard lk(g_lock);
            for (const auto& [id, r] : g_rumours) {
                // The teller's own account is theirs to tell, whoever added it. Otherwise only
                // RumourEngine's own lists are of people who were there; a client's are the people
                // it tells more fully itself.
                const bool own = r.teller && Is(*r.teller, self, selfBase);
                if (!own && (r.source != "RumourEngine" || std::none_of(r.knownBetterBy.begin(), r.knownBetterBy.end(),
                                                                        [&](const Subject& s) { return Is(s, self, selfBase); }))) {
                    continue;
                }
                for (const auto& m : mills) {
                    auto p = r.mills.find(m);
                    if (p == r.mills.end() || p->second.arrived > now || Stale(p->second, now)) continue;
                    // Their own, as they told it; anyone else's as their own people tell it, once written.
                    std::string              text = r.text;
                    std::vector<std::string> names;
                    if (own) {
                        names = NamedSubjects(r.text, r.subjects);
                    } else if (auto v = r.versions.find(m); v != r.versions.end()) {
                        text = v->second.text;
                        names = v->second.names;
                    } else {
                        unwritten.emplace_back(id, m);
                        break;
                    }
                    Heard h;
                    h.id = id;
                    h.mill = m;
                    h.text = std::move(text);
                    h.arrived = p->second.arrived;
                    h.notability = p->second.notability;
                    h.secrecy = r.secrecy;
                    h.airings = p->second.airings;
                    h.firstHand = true;
                    h.own = own;
                    for (const auto& s : r.subjects) {
                        if (std::find(names.begin(), names.end(), s.key) == names.end()) continue;
                        h.named.push_back(s.formId);
                        if (Is(s, self, selfBase)) h.aboutThem = true;
                    }
                    out.push_back(std::move(h));
                    break;
                }
            }
        }
        for (const auto& [id, m] : unwritten) Retell::Request(id, m);
        std::sort(out.begin(), out.end(), [](const Heard& a, const Heard& b) { return a.arrived > b.arrived; });
        if (out.size() > max) out.resize(max);
        return out;
    }

    std::vector<Rumour> Snapshot() {
        std::lock_guard     lk(g_lock);
        std::vector<Rumour> out;
        out.reserve(g_rumours.size());
        for (const auto& [id, r] : g_rumours) out.push_back(r);
        return out;
    }

    void AddArrivals(const std::vector<Arrival>& arrivals) {
        if (arrivals.empty()) return;
        auto            set = Mills::Current();
        const int       maxMills = Settings::MaxMillsPerRumour();
        std::lock_guard lk(g_lock);
        std::vector<std::pair<const Rumour*, std::string>> added;
        for (const auto& a : arrivals) {
            auto it = g_rumours.find(a.id);
            if (it == g_rumours.end()) continue;
            Rumour& r = it->second;
            if (r.mills.contains(a.mill) || static_cast<int>(r.mills.size()) >= maxMills) continue;
            Presence p;
            p.decided = a.decided;
            p.arrived = a.arrives;
            p.notability = a.notability;
            p.hops = a.hops;
            p.parent = a.parent;
            p.freshUntil = a.arrives + FreshDays(r, a.notability);
            r.mills[a.mill] = p;
            bool fact = false;
            if (auto text = GivenFor(r, a.mill, set.get(), &fact)) {
                Version v;
                v.text = *text;
                v.names = NamedSubjects(v.text, r.subjects);
                v.fact = fact;
                r.versions[a.mill] = std::move(v);
            }
            added.emplace_back(&r, a.mill);
        }
        if (added.empty()) return;
        Database::With([&](sqlite3* db) {
            Stmt(db, "BEGIN;").Run();
            for (const auto& [r, mill] : added) {
                const auto& p = r->mills.at(mill);
                Stmt(db, "INSERT OR IGNORE INTO rumour_mills(rumour_id, mill, parent_mill, hops, notability, fresh_until, airings, "
                         "arrived, game_time) VALUES(?,?,?,?,?,?,0,?,?);")
                    .Bind(1, r->id).Bind(2, mill).Bind(3, p.parent).Bind(4, p.hops).Bind(5, p.notability)
                    .Bind(6, p.freshUntil).Bind(7, p.arrived).Bind(8, p.decided).Run();
                if (auto v = r->versions.find(mill); v != r->versions.end()) {
                    Stmt(db, "INSERT OR REPLACE INTO versions(rumour_id, mill, text, names_subjects, model, game_time) "
                             "VALUES(?,?,?,?,'given',?);")
                        .Bind(1, r->id).Bind(2, mill).Bind(3, v->second.text).Bind(4, nlohmann::json(v->second.names).dump())
                        .Bind(5, p.decided).Run();
                }
            }
            Stmt(db, "COMMIT;").Run();
        });
        for (const auto& [r, mill] : added) {
            const auto& p = r->mills.at(mill);
            logger::info("Rumour #{} hops {} -> {} (n={:.2f}, hop {}, arrives day {:.2f})", r->id, p.parent, mill, p.notability,
                         p.hops, p.arrived);
        }
    }

    std::vector<std::pair<std::int32_t, std::string>> ArrivalsBetween(double after, double upTo) {
        std::lock_guard lk(g_lock);
        std::vector<std::pair<std::int32_t, std::string>> out;
        for (const auto& [id, r] : g_rumours) {
            for (const auto& [m, p] : r.mills) {
                // Hops, and origins added with a delay; otherwise an origin's arrival was the
                // rumour being added.
                const bool arrives = !p.parent.empty() || p.arrived > p.decided;
                if (arrives && p.arrived > after && p.arrived <= upTo) out.emplace_back(id, m);
            }
        }
        return out;
    }

    std::vector<std::pair<std::int32_t, std::string>> Unwritten(const std::vector<std::string>& mills, double now) {
        std::lock_guard lk(g_lock);
        std::vector<std::pair<std::int32_t, std::string>> out;
        for (const auto& [id, r] : g_rumours) {
            for (const auto& m : mills) {
                auto p = r.mills.find(m);
                if (p == r.mills.end() || p->second.arrived > now || now >= p->second.freshUntil) continue;
                if (!r.versions.contains(m)) out.emplace_back(id, m);
            }
        }
        return out;
    }

    std::optional<Chain> ChainTo(std::int32_t id, const std::string& mill, std::size_t maxSteps) {
        std::lock_guard lk(g_lock);
        auto            it = g_rumours.find(id);
        if (it == g_rumours.end()) return std::nullopt;
        const Rumour& r = it->second;
        if (!r.mills.contains(mill) || r.versions.contains(mill)) return std::nullopt;

        // Back along the parents to the nearest mill with a version.
        Chain                    c;
        std::vector<std::string> back;
        std::set<std::string>    seen;
        std::string              at = mill;
        while (true) {
            if (!seen.insert(at).second) break;  // a loop; shouldn't happen
            back.push_back(at);
            auto p = r.mills.find(at);
            if (p == r.mills.end() || p->second.parent.empty()) break;
            if (auto v = r.versions.find(p->second.parent); v != r.versions.end()) {
                c.from = p->second.parent;
                c.fromText = v->second.text;
                break;
            }
            at = p->second.parent;
        }
        if (c.from.empty()) {  // lost the trail, or it's the origin's own: start from the first telling
            c.fromText = r.text;
            if (r.teller) c.teller = r.teller->name;
        }
        c.id = id;
        c.steps.assign(back.rbegin(), back.rend());
        if (c.steps.size() > maxSteps) c.steps.resize(maxSteps);
        return c;
    }

    void SetVersions(std::int32_t id, const std::vector<std::pair<std::string, std::string>>& texts, const std::string& model) {
        const double    now = Now();
        std::lock_guard lk(g_lock);
        auto            it = g_rumours.find(id);
        if (it == g_rumours.end()) return;
        Rumour&                                             r = it->second;
        std::vector<std::pair<std::string, const Version*>> written;
        for (const auto& [mill, text] : texts) {
            if (text.empty() || !r.mills.contains(mill) || r.versions.contains(mill)) continue;
            Version v;
            v.text = text;
            v.names = NamedSubjects(text, r.subjects);
            written.emplace_back(mill, &(r.versions[mill] = std::move(v)));
        }
        if (written.empty()) return;
        Database::With([&](sqlite3* db) {
            Stmt(db, "BEGIN;").Run();
            for (const auto& [mill, v] : written) {
                Stmt(db, "INSERT OR REPLACE INTO versions(rumour_id, mill, text, names_subjects, model, game_time) VALUES(?,?,?,?,?,?);")
                    .Bind(1, id).Bind(2, mill).Bind(3, v->text).Bind(4, nlohmann::json(v->names).dump()).Bind(5, model)
                    .Bind(6, now).Run();
            }
            Stmt(db, "COMMIT;").Run();
        });
        for (const auto& [mill, v] : written) {
            logger::info("Rumour #{} as {} tell it{}: {}", id, mill,
                         v->names.empty() ? "" : fmt::format(" (names {})", v->names.size()), v->text);
        }
    }

    void Sweep(double now) {
        auto            set = Mills::Current();
        std::lock_guard lk(g_lock);
        std::vector<std::int32_t> stale;
        for (const auto& [id, r] : g_rumours) {
            bool any = false;
            for (const auto& [m, p] : r.mills) any = any || !Stale(p, now);
            if (!any) stale.push_back(id);
        }
        for (auto id : stale) RetireLocked(id, now, "stale everywhere", false, set.get());
    }

    std::size_t Count() {
        std::lock_guard lk(g_lock);
        return g_rumours.size();
    }

    std::vector<Promotion> PromotionsToMake() {
        auto                   set = Mills::Current();
        std::lock_guard        lk(g_lock);
        std::vector<Promotion> out;
        std::size_t            allHolds = 0;
        if (set) {
            std::set<RE::FormID> holds;
            for (const auto& m : set->mills) {
                if (m.hold && m.kind != Mills::Kind::Institution) holds.insert(m.hold);
            }
            allHolds = holds.size();
        }
        for (const auto& [id, r] : g_promoted) {
            if (r.knowledgeId) continue;
            Promotion p;
            p.id = id;
            p.source = r.source;
            p.key = r.key;
            p.text = FirstTelling(r);
            p.promote = r.promote;
            p.lasting = r.lasting;
            p.promotedText = r.promotedText;
            p.notability = r.notability;
            p.created = r.created;
            p.allHolds = allHolds;
            for (auto h : HoldsReached(r, set.get(), r.promoted)) {
                if (auto n = set->holdNames.find(h); n != set->holdNames.end() && !n->second.empty()) p.holds.push_back(n->second);
            }
            if (r.promotedText.empty() && r.lasting >= 0.0) {
                // The talk as it had settled: one telling from each hold (or guild, or faction),
                // the furthest travelled first.
                std::vector<std::tuple<int, std::string, const std::string*>> found;
                for (const auto& [m, v] : r.versions) {
                    auto pr = r.mills.find(m);
                    found.emplace_back(pr != r.mills.end() ? pr->second.hops : 0, m, &v.text);
                }
                std::stable_sort(found.begin(), found.end(),
                                 [](const auto& a, const auto& b) { return std::get<0>(a) > std::get<0>(b); });
                std::set<std::string> groups;
                for (const auto& [hops, m, text] : found) {
                    const auto* mill = set ? set->Find(m) : nullptr;
                    const auto  group = mill && mill->hold && mill->kind != Mills::Kind::Institution
                                            ? std::to_string(mill->hold) : m;
                    if (!groups.insert(group).second) continue;
                    p.versions.emplace_back(mill && !mill->placeName.empty() ? mill->placeName : m, *text);
                    if (p.versions.size() == 8) break;
                }
            }
            out.push_back(std::move(p));
        }
        return out;
    }

    void Decide(std::int32_t id, double lasting, bool keep) {
        std::lock_guard lk(g_lock);
        auto            it = g_promoted.find(id);
        if (it == g_promoted.end()) return;
        Database::With([&](sqlite3* db) {
            if (keep) {
                Stmt(db, "UPDATE rumours SET lasting = ? WHERE id = ?;").Bind(1, lasting).Bind(2, id).Run();
            } else {
                Stmt(db, "UPDATE rumours SET lasting = ?, promoted_time = NULL WHERE id = ?;").Bind(1, lasting).Bind(2, id).Run();
            }
        });
        if (keep) {
            it->second.lasting = lasting;
        } else {
            g_promoted.erase(it);
            ++g_promotedVersion;
        }
    }

    void SetPromotionText(std::int32_t id, const std::string& text) {
        std::lock_guard lk(g_lock);
        auto            it = g_promoted.find(id);
        if (it == g_promoted.end()) return;
        it->second.promotedText = text;
        Database::With([&](sqlite3* db) {
            Stmt(db, "UPDATE rumours SET promoted_text = ? WHERE id = ?;").Bind(1, text).Bind(2, id).Run();
        });
    }

    void SetKnowledgeId(std::int32_t id, std::int32_t knowledgeId) {
        std::lock_guard lk(g_lock);
        auto            it = g_promoted.find(id);
        if (it == g_promoted.end()) return;
        it->second.knowledgeId = knowledgeId;
        ++g_promotedVersion;
        Database::With([&](sqlite3* db) {
            Stmt st(db, "UPDATE rumours SET promotion_id = ? WHERE id = ?;");
            if (knowledgeId) st.Bind(1, knowledgeId);
            else st.Null(1);
            st.Bind(2, id).Run();
        });
    }

    std::vector<std::int32_t> TakeKnowledgeIds() {
        std::lock_guard           lk(g_lock);
        std::vector<std::int32_t> out;
        for (auto& [id, r] : g_promoted) {
            if (!r.knowledgeId) continue;
            out.push_back(r.knowledgeId);
            r.knowledgeId = 0;
        }
        if (!out.empty()) {
            ++g_promotedVersion;
            Database::With([&](sqlite3* db) { Stmt(db, "UPDATE rumours SET promotion_id = NULL WHERE promotion_id IS NOT NULL;").Run(); });
        }
        return out;
    }

    std::vector<std::int32_t> TakeWithdrawn() {
        std::lock_guard lk(g_lock);
        return std::exchange(g_withdrawn, {});
    }

    std::string HeardPromoted(RE::FormID formId) {
        if (!formId) return "";
        const std::uint64_t outside = Mills::PlacementsVersion() * 0x9E3779B97F4A7C15ull ^ Gossip::Version();
        {
            std::lock_guard lk(g_lock);
            if (g_promoted.empty()) return "";
            if (auto c = g_heardPromoted.find(formId);
                c != g_heardPromoted.end() && c->second.first == (g_promotedVersion ^ outside)) {
                return c->second.second;
            }
        }
        auto            set = Mills::Current();
        const auto      mills = set ? set->MillsFor(formId) : std::vector<std::string>{};
        const auto      key = ActorKey(formId);
        const auto      who = ListenerOf(set.get(), formId, 0);
        std::lock_guard lk(g_lock);
        const double    now = Now();
        std::string     out;
        for (const auto& [id, r] : g_promoted) {
            if (!r.knowledgeId) continue;
            bool heard = g_heard.contains({ id, key }) || CertainlyHeard(id, who);
            for (std::size_t i = 0; !heard && i < mills.size(); ++i) {
                auto p = r.mills.find(mills[i]);
                heard = p != r.mills.end() && p->second.arrived <= now && HeardIn(r, mills[i], p->second, who, set.get(), now);
            }
            if (heard) out += (out.empty() ? "|" : "") + std::to_string(id) + "|";
        }
        g_heardPromoted[formId] = { g_promotedVersion ^ outside, out };
        return out;
    }

    std::pair<std::size_t, std::size_t> PromotedCount() {
        std::lock_guard lk(g_lock);
        std::size_t     waiting = 0;
        for (const auto& [id, r] : g_promoted) waiting += r.knowledgeId ? 0 : 1;
        return { g_promoted.size(), waiting };
    }
}
