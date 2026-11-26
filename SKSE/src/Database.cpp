#include "Database.h"

#include <sqlite3.h>

namespace RumourEngine::Database {

    namespace {

        std::mutex g_lock;
        sqlite3*   g_db = nullptr;

        constexpr int kSchemaVersion = 12;

        // Tables from ADR-001 ("Persistence"). Later stages fill them; the
        // skeleton only creates them so the timeline handling has something to
        // act on. Every table has game_time, the day the row was written.
        constexpr const char* kSchema = R"SQL(
CREATE TABLE IF NOT EXISTS meta (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS rumours (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    source       TEXT NOT NULL,
    key          TEXT,
    origin       TEXT NOT NULL,       -- JSON: actor, location, mills or hold
    text         TEXT NOT NULL,       -- the first telling
    topics       TEXT,                -- JSON array
    notability   REAL NOT NULL,
    secrecy      REAL NOT NULL DEFAULT 0,  -- how much higher a gossip must rate to pass it on
    known_better_by TEXT,               -- JSON: [{key, form, name}], left out of these people's talk
    retell_origin INTEGER NOT NULL DEFAULT 0,  -- the origin mills' version is written from the text
    scope        TEXT NOT NULL,       -- spread, proclaimed, local
    retell       INTEGER NOT NULL DEFAULT 1,
    fresh_days   REAL,
    promote      TEXT NOT NULL DEFAULT 'auto',
    given        TEXT,                -- JSON: the caller's versions, "mill:..." / "hold:..." -> text
    retired_time REAL,
    promotion_id INTEGER,                -- SkyrimNet's world-knowledge entry, once made
    promoted_time REAL,                  -- when it retired worth keeping as world knowledge
    promoted_text TEXT,                  -- the entry, once the settled account is written
    lasting      REAL,                   -- the decision model's: will people still talk of it in a year
    game_time    REAL NOT NULL
);
CREATE UNIQUE INDEX IF NOT EXISTS rumours_by_key ON rumours(source, key) WHERE key IS NOT NULL;
CREATE TABLE IF NOT EXISTS rumour_subjects (
    rumour_id  INTEGER NOT NULL,
    subject    TEXT NOT NULL,         -- SkyrimNet UUID, or form:XXXXXXXX
    name       TEXT,
    form_id    INTEGER,               -- the runtime form ID when added
    game_time  REAL NOT NULL,
    PRIMARY KEY (rumour_id, subject)
);
CREATE TABLE IF NOT EXISTS rumour_mills (
    rumour_id   INTEGER NOT NULL,
    mill        TEXT NOT NULL,
    parent_mill TEXT,
    hops        INTEGER NOT NULL DEFAULT 0,
    notability  REAL NOT NULL,
    fresh_until REAL NOT NULL,
    airings     INTEGER NOT NULL DEFAULT 0,
    arrived     REAL,                 -- arrival; later than game_time while on its way
    game_time   REAL NOT NULL,        -- when the hop was decided
    PRIMARY KEY (rumour_id, mill)
);
CREATE TABLE IF NOT EXISTS versions (
    rumour_id      INTEGER NOT NULL,
    mill           TEXT NOT NULL,
    text           TEXT NOT NULL,
    names_subjects TEXT,              -- JSON array of the subjects this version still names
    model          TEXT,
    game_time      REAL NOT NULL,
    PRIMARY KEY (rumour_id, mill)
);
CREATE TABLE IF NOT EXISTS airings (
    id        INTEGER PRIMARY KEY AUTOINCREMENT,
    rumour_id INTEGER NOT NULL,
    mill      TEXT NOT NULL,
    teller    TEXT NOT NULL,
    listener  TEXT,
    earshot   TEXT,                   -- JSON array of UUIDs
    game_time REAL NOT NULL
);
CREATE TABLE IF NOT EXISTS heard (
    rumour_id INTEGER NOT NULL,
    actor     TEXT NOT NULL,          -- SkyrimNet UUID
    game_time REAL NOT NULL,
    PRIMARY KEY (rumour_id, actor)
);
-- Hold-back: people a rumour is about, started from, or who were there. Not shown it as talk
-- until it has gone round elsewhere (Rumours::InMills).
CREATE TABLE IF NOT EXISTS involved (
    rumour_id INTEGER NOT NULL,
    actor     TEXT NOT NULL,          -- SkyrimNet UUID, or form:XXXXXXXX
    why       TEXT NOT NULL,          -- started, witness, there
    game_time REAL NOT NULL,
    PRIMARY KEY (rumour_id, actor)
);
CREATE TABLE IF NOT EXISTS placements (
    actor        TEXT PRIMARY KEY,    -- SkyrimNet UUID (or form:XXXXXXXX) of a follower or a moved NPC
    joined_time  REAL,                -- last joined the party; NULL if they were following when first seen
    dismissed_time REAL,
    settled_time REAL,
    mills        TEXT,                -- JSON array: the mill they settled in, replacing their built home
    following    INTEGER NOT NULL DEFAULT 0,
    catchup      INTEGER NOT NULL DEFAULT 0, -- dismissed, and their time away is still to be harvested
    form_id      INTEGER,             -- their persistent reference when written
    game_time    REAL NOT NULL
);
-- A move under way, apart from placements so that a reload to before it began drops only the move.
CREATE TABLE IF NOT EXISTS pending_moves (
    actor     TEXT PRIMARY KEY,       -- as placements.actor
    form_id   INTEGER,
    mill      TEXT NOT NULL,          -- where they've been seen since `since`
    since     REAL NOT NULL,
    game_time REAL NOT NULL           -- = since
);
CREATE TABLE IF NOT EXISTS harvest_watermarks (
    actor          TEXT PRIMARY KEY,
    last_memory_id INTEGER,
    last_game_time REAL,
    game_time      REAL NOT NULL
);
CREATE TABLE IF NOT EXISTS screened (
    memory_id INTEGER PRIMARY KEY,    -- SkyrimNet's memory id
    verdict   TEXT NOT NULL,          -- seeded, same, same_event, rejected, ...
    rumour_id INTEGER,
    actor     TEXT,                   -- whose memory, SkyrimNet UUID
    events    TEXT,                   -- JSON: SkyrimNet's related_event_ids, to match other witnesses
    detail    TEXT,                   -- the screening answers, for the trace
    game_time REAL NOT NULL,
    memory_day REAL                   -- when the memory was formed, to match their others from the same stretch
);
CREATE TABLE IF NOT EXISTS gossip_scores (
    actor     TEXT PRIMARY KEY,
    score     REAL NOT NULL,          -- the reading, 0 to 1
    source    TEXT NOT NULL,          -- model: a Decisions reading of their bio
    level     REAL,                   -- the reading as the model gave it, 0 (tight-lipped) to 4
    bio       TEXT,                   -- fingerprint of the bio blocks it was read from, hex
    game_time REAL NOT NULL
);
CREATE TABLE IF NOT EXISTS quest_endings (
    quest     TEXT PRIMARY KEY,       -- editor ID, lowercased, else the form id in hex
    form_id   INTEGER,
    stage     INTEGER,                -- the stage it ended on, as the stage event gave it
    state     TEXT NOT NULL,          -- before (ended before RumourEngine saw it), never, asking, telling,
                                      -- private, seeded, unasked (no decision model), failed
    heard     REAL,                   -- the decision model's answers, once asked
    far       REAL,
    rumour_id INTEGER,
    detail    TEXT,                   -- JSON: the journal and where it ended, to carry on after a reload
    game_time REAL NOT NULL           -- when it ended
);
)SQL";

        constexpr const char* kTimedTables[] = { "rumours", "rumour_subjects", "rumour_mills", "versions", "airings",
                                                 "heard", "involved", "placements", "pending_moves", "harvest_watermarks", "screened",
                                                 "gossip_scores", "quest_endings" };

        bool Exec(const char* sql) {
            char* err = nullptr;
            if (sqlite3_exec(g_db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
                logger::error("Database: {}", err ? err : "unknown error");
                sqlite3_free(err);
                return false;
            }
            return true;
        }

        bool HasColumn(const char* table, const char* column) {
            sqlite3_stmt* st = nullptr;
            bool          found = false;
            const auto    sql = fmt::format("PRAGMA table_info({});", table);
            if (sqlite3_prepare_v2(g_db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK) {
                while (sqlite3_step(st) == SQLITE_ROW) {
                    auto* name = reinterpret_cast<const char*>(sqlite3_column_text(st, 1));
                    if (name && std::string_view(name) == column) found = true;
                }
            }
            sqlite3_finalize(st);
            return found;
        }

        void CloseLocked() {
            if (g_db) {
                sqlite3_close_v2(g_db);
                g_db = nullptr;
            }
        }
    }

    std::filesystem::path PathFor(const std::string& saveId) {
        return std::filesystem::current_path() / "Data" / "SKSE" / "Plugins" / "RumourEngine" / "data" /
               ("RumourEngine-" + saveId + ".db");
    }

    bool Open(const std::filesystem::path& path) {
        std::lock_guard lk(g_lock);
        CloseLocked();
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);

        const auto utf8 = path.u8string();
        if (sqlite3_open_v2(reinterpret_cast<const char*>(utf8.c_str()), &g_db,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
            logger::error("Database: could not open {}: {}", path.string(), g_db ? sqlite3_errmsg(g_db) : "?");
            CloseLocked();
            return false;
        }
        sqlite3_busy_timeout(g_db, 2000);
        Exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA foreign_keys=ON;");
        if (!Exec(kSchema)) {
            CloseLocked();
            return false;
        }
        // Schema 1 (stage 1) had no form_id on subjects.
        if (!HasColumn("rumour_subjects", "form_id")) Exec("ALTER TABLE rumour_subjects ADD COLUMN form_id INTEGER;");
        // Schema 2 (stage 2) had no hops: game_time was the arrival, and nothing kept given versions.
        if (!HasColumn("rumour_mills", "arrived")) {
            Exec("ALTER TABLE rumour_mills ADD COLUMN arrived REAL; UPDATE rumour_mills SET arrived = game_time;");
        }
        if (!HasColumn("rumours", "given")) Exec("ALTER TABLE rumours ADD COLUMN given TEXT;");
        // Schema 3 (stage 3) screened nothing yet, so kept no witness details.
        if (!HasColumn("screened", "events")) {
            Exec("ALTER TABLE screened ADD COLUMN actor TEXT; ALTER TABLE screened ADD COLUMN events TEXT; "
                 "ALTER TABLE screened ADD COLUMN detail TEXT;");
        }
        // Schema 4 (stage 4a) placed nobody yet.
        if (!HasColumn("placements", "following")) {
            Exec("ALTER TABLE placements ADD COLUMN following INTEGER NOT NULL DEFAULT 0; "
                 "ALTER TABLE placements ADD COLUMN catchup INTEGER NOT NULL DEFAULT 0; "
                 "ALTER TABLE placements ADD COLUMN form_id INTEGER;");
        }
        // Schema 5 kept no secrets.
        if (!HasColumn("rumours", "secrecy")) Exec("ALTER TABLE rumours ADD COLUMN secrecy REAL NOT NULL DEFAULT 0;");
        // Schema 6 had no facts, no one who knew better, and took the first telling as given.
        if (!HasColumn("rumours", "known_better_by")) {
            Exec("ALTER TABLE rumours ADD COLUMN known_better_by TEXT; "
                 "ALTER TABLE rumours ADD COLUMN retell_origin INTEGER NOT NULL DEFAULT 0;");
        }
        // Schema 8 didn't keep when a screened memory was formed.
        if (!HasColumn("screened", "memory_day")) Exec("ALTER TABLE screened ADD COLUMN memory_day REAL;");
        // Schema 9 (before stage 5) read nobody's bio.
        if (!HasColumn("gossip_scores", "level")) {
            Exec("ALTER TABLE gossip_scores ADD COLUMN level REAL; ALTER TABLE gossip_scores ADD COLUMN bio TEXT;");
        }
        // Schema 10 (before stage 6) promoted nothing.
        if (!HasColumn("rumours", "promoted_time")) {
            Exec("ALTER TABLE rumours ADD COLUMN promoted_time REAL; ALTER TABLE rumours ADD COLUMN promoted_text TEXT; "
                 "ALTER TABLE rumours ADD COLUMN lasting REAL;");
        }
        Exec(fmt::format("INSERT INTO meta(key, value) VALUES('schema_version', '{}') "
                         "ON CONFLICT(key) DO UPDATE SET value = excluded.value;", kSchemaVersion).c_str());
        logger::info("Database: opened {}", path.string());
        return true;
    }

    void Close() {
        std::lock_guard lk(g_lock);
        CloseLocked();
    }

    bool IsOpen() {
        std::lock_guard lk(g_lock);
        return g_db != nullptr;
    }

    int DeleteAfter(double gameDays) {
        std::lock_guard lk(g_lock);
        if (!g_db) return 0;
        int total = 0;
        Exec("BEGIN;");
        // What retired after then was still going round then. Its promotion is taken back by
        // TakeBackPromotions, first.
        {
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(g_db, "UPDATE rumours SET retired_time = NULL WHERE retired_time > ?;", -1, &st, nullptr) ==
                SQLITE_OK) {
                sqlite3_bind_double(st, 1, gameDays);
                if (sqlite3_step(st) == SQLITE_DONE) total += sqlite3_changes(g_db);
            }
            sqlite3_finalize(st);
        }
        for (const char* table : kTimedTables) {
            sqlite3_stmt* st = nullptr;
            const auto    sql = fmt::format("DELETE FROM {} WHERE game_time > ?;", table);
            if (sqlite3_prepare_v2(g_db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) continue;
            sqlite3_bind_double(st, 1, gameDays);
            if (sqlite3_step(st) == SQLITE_DONE) total += sqlite3_changes(g_db);
            sqlite3_finalize(st);
        }
        Exec("COMMIT;");
        return total;
    }

    std::vector<int> TakeBackPromotions(double gameDays) {
        std::lock_guard  lk(g_lock);
        std::vector<int> ids;
        if (!g_db) return ids;
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(g_db,
                               "SELECT promotion_id FROM rumours WHERE promotion_id IS NOT NULL AND "
                               "(promoted_time > ?1 OR game_time > ?1);",
                               -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_double(st, 1, gameDays);
            while (sqlite3_step(st) == SQLITE_ROW) ids.push_back(sqlite3_column_int(st, 0));
        }
        sqlite3_finalize(st);
        st = nullptr;
        if (sqlite3_prepare_v2(g_db,
                               "UPDATE rumours SET promoted_time = NULL, promoted_text = NULL, promotion_id = NULL, lasting = NULL "
                               "WHERE promoted_time > ?;",
                               -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_double(st, 1, gameDays);
            sqlite3_step(st);
        }
        sqlite3_finalize(st);
        return ids;
    }

    bool With(const std::function<void(sqlite3*)>& f) {
        std::lock_guard lk(g_lock);
        if (!g_db) return false;
        f(g_db);
        return true;
    }

    std::string Counts() {
        std::lock_guard lk(g_lock);
        if (!g_db) return "closed";
        std::string out;
        for (const char* table : kTimedTables) {
            sqlite3_stmt* st = nullptr;
            const auto    sql = fmt::format("SELECT COUNT(*) FROM {};", table);
            long long     n = -1;
            if (sqlite3_prepare_v2(g_db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) {
                n = sqlite3_column_int64(st, 0);
            }
            sqlite3_finalize(st);
            out += fmt::format("{}{} {}", out.empty() ? "" : ", ", table, n);
        }
        return out;
    }
}
