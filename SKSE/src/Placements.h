#pragma once

// Where people really live (ADR-001, "Dismissed followers" and "Observed homes").
// The builder places people where the game data says they live; follower mods,
// marriage mods and quests move them. Every game hour, the main thread looks up
// where each watched person (a named NPC with a persistent reference) is. Someone
// who stays in one town for settle.hours counts as living there: the mill they'd
// belong to there replaces their built home (Mills::SetPlacement), and going back
// home clears it. Party members belong to no mill while they follow; when one is
// dismissed, their memories wait until they settle, and then their time away is
// harvested once (Harvest.cpp asks for it with TakeCatchups).

namespace RumourEngine::Placements {

    /** A dismissed follower who has settled: their memories from joining to leaving are to be harvested. */
    struct Catchup {
        RE::FormID            ref = 0;
        std::optional<double> joined;  // game days; empty if they were already following when first seen
        double                dismissed = 0.0;
    };

    /** The save's database is open and settled: read the stored placements and apply them. */
    void Load();

    /** A new save: forget everything, and drop a lookup still on its way. */
    void Reset();

    /** Worker thread, once a save is ready: settle what the last look found, and ask for the next. */
    void Pump();

    /** Dismissed, and not settled anywhere yet: their memories wait. `base` is their base form. Any thread. */
    bool Unsettled(RE::FormID base);

    /** Following the player when last looked at (each game hour, and as stored for the save).
     *  `base` is their base form. Any thread. */
    bool Following(RE::FormID base);

    /** Settled followers whose time away is due a harvest. Each is handed out once. */
    std::vector<Catchup> TakeCatchups();
}
