#pragma once

// The game session: the plugin scan at data load, then per save, rebuilding
// the mills, opening that playthrough's database, and settling its timeline
// with SkyrimNet's keep/clear prompt before anything is ready.

namespace RumourEngine::Session {

    /** kDataLoaded: start the plugin scan, and listen for SkyrimNet's ModEvents. */
    void OnDataLoaded();

    /** kPreLoadGame: whatever was open belongs to the save being left. */
    void OnPreLoad();

    /** kPostLoadGame or kNewGame. */
    void OnGameLoaded(bool newGame);

    /** A save's database is open and its timeline settled. */
    bool IsReady();
}
