#pragma once

// One SQLite database per playthrough, named by SkyrimNet's save id (ADR-001,
// "Persistence"). Nothing goes in the SKSE co-save. Every row carries the game
// time it was written at, in days, so history after a loaded save can be taken
// back when SkyrimNet clears its own.

struct sqlite3;

namespace RumourEngine::Database {

    /** Data/SKSE/Plugins/RumourEngine/data/RumourEngine-<saveId>.db */
    std::filesystem::path PathFor(const std::string& saveId);

    /** Open (creating if new) and bring the schema up to date. Closes any other first. */
    bool Open(const std::filesystem::path& path);
    void Close();
    bool IsOpen();

    /** Delete every row stamped after `gameDays`, and bring back rumours retired since. Returns how
     *  many rows went or changed. */
    int DeleteAfter(double gameDays);

    /** Forget promotions made after `gameDays`, and return the world-knowledge entries to remove:
     *  theirs, and those of rumours added since. Before DeleteAfter. */
    std::vector<int> TakeBackPromotions(double gameDays);

    /** Rows per table, for the log. */
    std::string Counts();

    /** Run f with the open connection, under the database lock. False if none is open. */
    bool With(const std::function<void(sqlite3*)>& f);
}
