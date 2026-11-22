#pragma once

// Rumours from what people remember (ADR-001, "Harvest and screening" and
// "Writing: seeding and retelling"). Every harvest.interval_hours of game time:
//   1. SkyrimNet's actor engagement says who has had important memories lately;
//   2. their memories nobody has screened are fetched, filtered in SQL;
//   3. one decision request per person asks, for each memory, whether they'd
//      pass it on, how much a room would care, and whether it is talk already
//      going round (then it reinforces that rumour instead);
//   4. one rumour_seed call writes the accepted ones as rumours, which start
//      in the owner's mills and spread from there.
// Another witness's memory of a happening that already became a rumour is
// matched by SkyrimNet's event ids, with no call at all.

namespace RumourEngine::Harvest {

    /** The save's database is open and settled: read what was screened before. */
    void Load();

    /** A new save: forget everything, and drop answers still on their way. */
    void Reset();

    /** Worker thread, once a save is ready: handle answers, seed, and sweep when one is due. */
    void Pump();
}
