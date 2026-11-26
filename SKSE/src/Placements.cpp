#include "Placements.h"
#include "Database.h"
#include "Delivery.h"
#include "Mills.h"
#include "Rumours.h"
#include "Session.h"
#include "Settings.h"
#include "SkyrimNet.h"
#include "Text.h"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <chrono>

namespace RumourEngine::Placements {

    namespace {

        using json = nlohmann::json;

        constexpr double kCheckDays = 1.0 / 24.0;  // a look every game hour

        struct State {
            std::string           key;  // the database row: SkyrimNet UUID, or form:XXXXXXXX
            RE::FormID            ref = 0;
            bool                  following = false;
            std::optional<double> joined;
            double                dismissed = 0.0;
            bool                  catchup = false;  // dismissed; time away still to harvest once settled
            std::string           placed;           // where they settled; empty for their built home
            double                settled = 0.0;
            std::string           pendingMill;  // a move under way: where they've been since pendingSince
            double                pendingSince = -1.0;
        };

        /** One watched person, as the main thread found them. */
        struct Seen {
            RE::FormID  ref = 0, base = 0, location = 0;
            Mills::Kind kind = Mills::Kind::Commons;
            bool        byPlace = true;
            bool        following = false;
        };

        std::mutex                            g_lock;
        std::atomic<unsigned>                 g_generation = 0;
        std::unordered_map<RE::FormID, State> g_state;  // by base form
        double                                g_lastCheck = -1.0;
        bool                                  g_firstCheck = true;
        bool                                  g_lookPending = false;
        std::optional<std::vector<Seen>>      g_seen;
        std::vector<Catchup>                  g_catchups;
        bool                                  g_timed = false;

        RE::TESWorldSpace* WorldOf(RE::Actor* a) {
            if (auto* ws = a->GetWorldspace()) return ws;
            auto* cell = a->GetParentCell();  // out of sight, a worldspace's persistent cell
            return cell && !cell->IsInteriorCell() ? cell->GetRuntimeData().worldSpace : nullptr;
        }

        std::string KeyOf(RE::FormID ref) {
            if (const auto uuid = SkyrimNet::UUIDOf(ref)) return std::to_string(uuid);
            return fmt::format("form:{:08X}", ref);
        }

        std::string NameOf(RE::FormID ref) {
            auto*       a = RE::TESForm::LookupByID<RE::Actor>(ref);
            const char* n = a ? a->GetDisplayFullName() : nullptr;
            return n ? Text::FromGame(n) : fmt::format("{:08X}", ref);
        }

        /** Nothing for the placements table. */
        bool Trivial(const State& s) { return !s.following && !s.catchup && s.placed.empty(); }

        /** Nothing to remember at all. */
        bool Idle(const State& s) { return Trivial(s) && s.pendingMill.empty(); }

        /** `rows` changed in placements, `moves` in what is under way. */
        void Write(const std::vector<State>& rows, const std::vector<State>& moves, double now) {
            if (rows.empty() && moves.empty()) return;
            Database::With([&](sqlite3* db) {
                sqlite3_exec(db, "BEGIN;", nullptr, nullptr, nullptr);
                for (const auto& s : moves) {
                    sqlite3_stmt* st = nullptr;
                    const char*   sql = s.pendingMill.empty() ? "DELETE FROM pending_moves WHERE actor = ?;"
                                                              : "INSERT OR REPLACE INTO pending_moves(actor, form_id, mill, since, game_time) "
                                                                "VALUES(?,?,?,?,?);";
                    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) {
                        logger::error("Placements: {}", sqlite3_errmsg(db));
                        continue;
                    }
                    sqlite3_bind_text(st, 1, s.key.c_str(), -1, SQLITE_TRANSIENT);
                    if (!s.pendingMill.empty()) {
                        sqlite3_bind_int64(st, 2, s.ref);
                        sqlite3_bind_text(st, 3, s.pendingMill.c_str(), -1, SQLITE_TRANSIENT);
                        sqlite3_bind_double(st, 4, s.pendingSince);
                        sqlite3_bind_double(st, 5, s.pendingSince);
                    }
                    if (sqlite3_step(st) != SQLITE_DONE) logger::error("Placements: {}", sqlite3_errmsg(db));
                    sqlite3_finalize(st);
                }
                for (const auto& s : rows) {
                    sqlite3_stmt* st = nullptr;
                    if (Trivial(s)) {
                        if (sqlite3_prepare_v2(db, "DELETE FROM placements WHERE actor = ?;", -1, &st, nullptr) == SQLITE_OK) {
                            sqlite3_bind_text(st, 1, s.key.c_str(), -1, SQLITE_TRANSIENT);
                            sqlite3_step(st);
                        }
                        sqlite3_finalize(st);
                        continue;
                    }
                    if (sqlite3_prepare_v2(db,
                                           "INSERT OR REPLACE INTO placements(actor, joined_time, dismissed_time, settled_time, mills, "
                                           "following, catchup, form_id, game_time) VALUES(?,?,?,?,?,?,?,?,?);",
                                           -1, &st, nullptr) != SQLITE_OK) {
                        logger::error("Placements: {}", sqlite3_errmsg(db));
                        continue;
                    }
                    const auto mills = s.placed.empty() ? std::string("[]") : json::array({ s.placed }).dump();
                    sqlite3_bind_text(st, 1, s.key.c_str(), -1, SQLITE_TRANSIENT);
                    if (s.joined) sqlite3_bind_double(st, 2, *s.joined);
                    else sqlite3_bind_null(st, 2);
                    if (s.dismissed > 0.0) sqlite3_bind_double(st, 3, s.dismissed);
                    else sqlite3_bind_null(st, 3);
                    if (s.settled > 0.0) sqlite3_bind_double(st, 4, s.settled);
                    else sqlite3_bind_null(st, 4);
                    sqlite3_bind_text(st, 5, mills.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int(st, 6, s.following ? 1 : 0);
                    sqlite3_bind_int(st, 7, s.catchup ? 1 : 0);
                    sqlite3_bind_int64(st, 8, s.ref);
                    sqlite3_bind_double(st, 9, now);
                    if (sqlite3_step(st) != SQLITE_DONE) logger::error("Placements: {}", sqlite3_errmsg(db));
                    sqlite3_finalize(st);
                }
                sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr);
            });
        }

        /** Main thread: where each watched person is, and whether they're following. */
        void Look(unsigned gen, std::shared_ptr<const Mills::MillSet> set) {
            // No world loaded (the main menu, a load that failed): nobody is anywhere, and nobody is
            // following, but that says nothing about who left the party.
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || !player->GetParentCell() || !player->Is3DLoaded()) {
                std::lock_guard lk(g_lock);
                g_lookPending = false;
                g_lastCheck = -1.0;  // look again as soon as there is a world
                return;
            }
            const auto        start = std::chrono::steady_clock::now();
            std::vector<Seen> seen;
            seen.reserve(set->watch.size());
            int unseen = 0, byCell = 0;
            std::vector<std::string> examples;  // the first look of a session: why some couldn't be placed
            const bool explain = !g_timed;
            for (const auto& w : set->watch) {
                auto* a = RE::TESForm::LookupByID<RE::Actor>(w.ref);
                if (!a || a->IsDead() || a->IsDisabled()) continue;
                RE::FormID location = 0;
                if (auto* loc = a->GetCurrentLocation()) {
                    location = loc->GetFormID();
                } else if (!a->Is3DLoaded()) {
                    // Out of sight outdoors, the game keeps someone in no location; the cell they stand in has one.
                    ++unseen;
                    auto* ws = WorldOf(a);
                    if (ws) location = set->LocationAt(ws->GetFormID(), a->GetPosition());
                    byCell += location != 0;
                    if (explain && examples.size() < 10) {
                        auto*      cell = a->GetParentCell();
                        auto*      own = a->GetWorldspace();
                        auto*      cellWs = cell ? cell->GetRuntimeData().worldSpace : nullptr;
                        const auto pos = a->GetPosition();
                        examples.push_back(fmt::format(
                            "{} (worldspace {:08X}, cell {:08X}{}{}, cell's worldspace {:08X}, at {:.0f},{:.0f} = grid {},{} -> {:08X})",
                            NameOf(w.ref), own ? own->GetFormID() : 0, cell ? cell->GetFormID() : 0,
                            cell && cell->IsInteriorCell() ? " interior" : "", ws && ws->persistentCell == cell ? " persistent" : "",
                            cellWs ? cellWs->GetFormID() : 0, pos.x, pos.y, static_cast<int>(std::floor(pos.x / 4096.0f)),
                            static_cast<int>(std::floor(pos.y / 4096.0f)), location));
                    }
                }
                seen.push_back({ w.ref, w.base, location, w.kind, w.byPlace, Delivery::InPlayersCompany(a) });
            }
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            std::lock_guard lk(g_lock);
            g_lookPending = false;
            if (gen != g_generation) return;
            if (!std::exchange(g_timed, true)) {
                logger::info("Placements: watching {} named people with a persistent reference ({} found; {} out of sight with no "
                             "location, {} of them placed by the cell they stand in; {:.1f} ms on the main thread)",
                             set->watch.size(), seen.size(), unseen, byCell, ms);
                std::string list;
                for (const auto& e : examples) list += (list.empty() ? "" : "; ") + e;
                if (!list.empty()) {
                    logger::info("Placements: out of sight with no location ({} exterior cells on the grid): {}", set->exteriorLocations.size(),
                                 list);
                }
            }
            g_seen = std::move(seen);
        }

        /** Act on a look: joins, dismissals, and moves that have lasted. */
        void Settle(const Mills::MillSet& set, const std::vector<Seen>& seen, double now) {
            const double settleDays = Settings::SettleHours() / 24.0;
            const double maxDays = Settings::SettleMaxDays();
            std::vector<State> dirty, moved;
            std::vector<std::string> notes;

            std::lock_guard lk(g_lock);
            const bool first = std::exchange(g_firstCheck, false);
            std::vector<std::string> away;  // first look of a session: who isn't where the builder put them
            int                      awayCount = 0, followingCount = 0;
            for (const auto& s : seen) {
                if (first && s.following) ++followingCount;
                auto it = g_state.find(s.base);

                if (s.following) {
                    if (it != g_state.end() && it->second.following) continue;
                    const bool known = it != g_state.end();
                    State&     st = g_state[s.base];
                    st.ref = s.ref;
                    if (st.key.empty()) st.key = KeyOf(s.ref);
                    st.following = true;
                    // Already following when first seen: when they joined is unknown, so their whole history counts.
                    st.joined = (first && !known) ? std::nullopt : std::optional<double>(now);
                    st.dismissed = 0.0;
                    st.catchup = false;
                    if (!st.pendingMill.empty()) {
                        st.pendingMill.clear();
                        st.pendingSince = -1.0;
                        moved.push_back(st);
                    }
                    if (!st.placed.empty()) {  // a placement holds until they're recruited again
                        st.placed.clear();
                        Mills::ClearPlacement(s.base);
                    }
                    notes.push_back(fmt::format("{} is following the player{}", NameOf(s.ref), st.joined ? "" : " (since before RumourEngine saw)"));
                    dirty.push_back(st);
                    continue;
                }

                if (it != g_state.end() && it->second.following) {
                    auto& st = it->second;
                    st.following = false;
                    st.dismissed = now;
                    st.catchup = Settings::FollowerHarvestMax() > 0;
                    notes.push_back(fmt::format("{} has left the party; their stories wait until they settle", NameOf(s.ref)));
                    dirty.push_back(st);
                }

                auto       here = set.SettleAt(s.location, s.kind, s.byPlace);
                const auto built = set.BuiltHome(s.base);
                it = g_state.find(s.base);
                const auto home = it != g_state.end() && !it->second.placed.empty() ? it->second.placed : built;
                // In their own town, which part of it they stand in doesn't change their stratum: a
                // servant in the streets is still court. Only another place is a move.
                if (!here.empty() && !home.empty() && here != home) {
                    const auto* h = set.Find(here);
                    const auto* o = set.Find(home);
                    if (h && o && h->settlement && h->settlement == o->settlement) here = home;
                }
                if (first && !here.empty() && here != home) {
                    if (++awayCount <= 20) away.push_back(fmt::format("{} ({} -> {})", NameOf(s.ref), home.empty() ? "nowhere" : home, here));
                }

                // On the road, in the wilds or in a dungeon: no move is under way.
                if (here.empty()) {
                    if (it == g_state.end()) continue;
                    auto& st = it->second;
                    if (!st.pendingMill.empty()) {
                        st.pendingMill.clear();
                        st.pendingSince = -1.0;
                        moved.push_back(st);
                    }
                    if (st.catchup && now - st.dismissed >= maxDays) {
                        notes.push_back(fmt::format("{} hasn't settled anywhere in {:.0f} days; their stories go home to {}", NameOf(s.ref),
                                                    maxDays, home.empty() ? "nowhere" : home));
                        if (!home.empty()) g_catchups.push_back({ st.ref, st.joined, st.dismissed });
                        st.catchup = false;
                        st.settled = now;
                        dirty.push_back(st);
                    }
                    if (Idle(st)) g_state.erase(it);
                    continue;
                }
                // Where they belong, with no catch-up waiting on them settling: nothing is under way.
                if (here == home && (it == g_state.end() || !it->second.catchup)) {
                    if (it == g_state.end()) continue;
                    auto& st = it->second;
                    if (!st.pendingMill.empty()) {
                        st.pendingMill.clear();
                        st.pendingSince = -1.0;
                        moved.push_back(st);
                    }
                    if (Idle(st)) g_state.erase(it);
                    continue;
                }
                if (it == g_state.end()) {
                    it = g_state.emplace(s.base, State{}).first;
                    it->second.ref = s.ref;
                    it->second.key = KeyOf(s.ref);
                }
                auto& st = it->second;
                if (here != st.pendingMill) {  // stored, as sessions are often shorter than settle_hours
                    st.pendingMill = here;
                    st.pendingSince = now;
                    moved.push_back(st);
                }
                const bool stayed = now - st.pendingSince >= settleDays || (st.catchup && now - st.dismissed >= maxDays);
                if (!stayed) continue;
                st.pendingMill.clear();
                st.pendingSince = -1.0;
                moved.push_back(st);

                if (here != home) {
                    if (here == built) {
                        notes.push_back(fmt::format("{} is back home in {}", NameOf(s.ref), built));
                        st.placed.clear();
                        Mills::ClearPlacement(s.base);
                    } else {
                        notes.push_back(fmt::format("{} now lives in {} (was {})", NameOf(s.ref), here, home.empty() ? "nowhere" : home));
                        st.placed = here;
                        Mills::SetPlacement(s.base, here);
                    }
                    st.settled = now;
                    dirty.push_back(st);
                }
                if (st.catchup) {
                    notes.push_back(fmt::format("{} has settled in {}; their time away is harvested next", NameOf(s.ref), here));
                    g_catchups.push_back({ st.ref, st.joined, st.dismissed });
                    st.catchup = false;
                    st.settled = now;
                    dirty.push_back(st);
                }
                if (Idle(st)) g_state.erase(it);  // home, and nothing to remember
            }
            if (first) {
                std::string list;
                for (const auto& a : away) list += (list.empty() ? "" : ", ") + a;
                logger::info("Placements: first look: {} following, {} in a town other than their home, and moving there if they "
                             "stay {:.0f} hours{}{}",
                             followingCount, awayCount, Settings::SettleHours(), list.empty() ? "" : ": ", list);
            }
            for (const auto& n : notes) logger::info("Placements: {}", n);
            Write(dirty, moved, now);
        }
    }

    void Load() {
        auto set = Mills::Current();
        struct Row {
            State      st;
            RE::FormID formId = 0;
        };
        std::vector<Row> rows;
        Database::With([&](sqlite3* db) {
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db,
                                   "SELECT actor, joined_time, dismissed_time, settled_time, mills, following, catchup, form_id FROM placements;",
                                   -1, &st, nullptr) != SQLITE_OK) {
                return;
            }
            while (sqlite3_step(st) == SQLITE_ROW) {
                Row r;
                auto text = [&](int c) {
                    auto* t = reinterpret_cast<const char*>(sqlite3_column_text(st, c));
                    return std::string(t ? t : "");
                };
                r.st.key = text(0);
                if (sqlite3_column_type(st, 1) != SQLITE_NULL) r.st.joined = sqlite3_column_double(st, 1);
                r.st.dismissed = sqlite3_column_double(st, 2);
                r.st.settled = sqlite3_column_double(st, 3);
                const auto mills = json::parse(text(4), nullptr, false);
                if (mills.is_array() && !mills.empty() && mills[0].is_string()) r.st.placed = mills[0].get<std::string>();
                r.st.following = sqlite3_column_int(st, 5) != 0;
                r.st.catchup = sqlite3_column_int(st, 6) != 0;
                r.formId = static_cast<RE::FormID>(sqlite3_column_int64(st, 7));
                rows.push_back(std::move(r));
            }
            sqlite3_finalize(st);
            st = nullptr;
            if (sqlite3_prepare_v2(db, "SELECT actor, form_id, mill, since FROM pending_moves;", -1, &st, nullptr) != SQLITE_OK) return;
            while (sqlite3_step(st) == SQLITE_ROW) {
                auto*      t = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
                const auto key = std::string(t ? t : "");
                auto       it = std::find_if(rows.begin(), rows.end(), [&](const Row& r) { return r.st.key == key; });
                if (it == rows.end()) {
                    it = rows.insert(rows.end(), Row{});
                    it->st.key = key;
                    it->formId = static_cast<RE::FormID>(sqlite3_column_int64(st, 1));
                }
                auto* m = reinterpret_cast<const char*>(sqlite3_column_text(st, 2));
                it->st.pendingMill = m ? m : "";
                it->st.pendingSince = sqlite3_column_double(st, 3);
            }
            sqlite3_finalize(st);
        });

        std::lock_guard lk(g_lock);
        int placed = 0, lost = 0, moving = 0;
        for (auto& r : rows) {
            // The UUID survives a load-order change; the stored form is the fallback.
            RE::FormID ref = 0;
            if (!r.st.key.empty() && r.st.key.rfind("form:", 0) != 0) {
                try {
                    ref = SkyrimNet::FormOf(std::stoull(r.st.key));
                } catch (...) {
                }
            }
            if (!ref) ref = r.formId;
            auto* a = RE::TESForm::LookupByID<RE::Actor>(ref);
            auto* base = a ? a->GetActorBase() : nullptr;
            if (!base) {
                ++lost;
                continue;
            }
            r.st.ref = ref;
            if (!r.st.placed.empty() && !(set && set->Find(r.st.placed))) r.st.placed.clear();  // that mill is gone
            if (!r.st.pendingMill.empty() && !(set && set->Find(r.st.pendingMill))) {
                r.st.pendingMill.clear();
                r.st.pendingSince = -1.0;
            }
            moving += !r.st.pendingMill.empty();
            if (!r.st.placed.empty()) {
                Mills::SetPlacement(base->GetFormID(), r.st.placed);
                ++placed;
            }
            g_state[base->GetFormID()] = std::move(r.st);
        }
        if (!rows.empty()) {
            logger::info("Placements: {} people remembered ({} settled away from their built home, {} partway through a move{})",
                         g_state.size(), placed, moving, lost ? fmt::format(", {} no longer found", lost) : "");
        }
    }

    void Reset() {
        Mills::ClearPlacements();
        std::lock_guard lk(g_lock);
        ++g_generation;
        g_state.clear();
        g_lastCheck = -1.0;
        g_firstCheck = true;
        g_lookPending = false;
        g_seen.reset();
        g_catchups.clear();
    }

    void Pump() {
        if (!Session::IsReady()) return;
        auto set = Mills::Current();
        if (!set) return;
        const double now = Rumours::Now();

        std::optional<std::vector<Seen>> seen;
        {
            std::lock_guard lk(g_lock);
            seen.swap(g_seen);
        }
        if (seen) Settle(*set, *seen, now);

        unsigned gen;
        {
            std::lock_guard lk(g_lock);
            if (g_lookPending) return;
            if (g_lastCheck >= 0.0 && now >= g_lastCheck && now - g_lastCheck < kCheckDays) return;
            g_lastCheck = now;
            g_lookPending = true;
            gen = g_generation;
        }
        auto* task = SKSE::GetTaskInterface();
        if (!task) {
            std::lock_guard lk(g_lock);
            g_lookPending = false;
            return;
        }
        task->AddTask([gen, set]() { Look(gen, set); });
    }

    bool Unsettled(RE::FormID base) {
        std::lock_guard lk(g_lock);
        auto            it = g_state.find(base);
        return it != g_state.end() && !it->second.following && it->second.catchup;
    }

    bool Following(RE::FormID base) {
        std::lock_guard lk(g_lock);
        auto            it = g_state.find(base);
        return it != g_state.end() && it->second.following;
    }

    std::vector<Catchup> TakeCatchups() {
        std::lock_guard lk(g_lock);
        return std::exchange(g_catchups, {});
    }
}
