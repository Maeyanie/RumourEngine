#pragma once

#include "Mills.h"

// Rumours hopping between mills (ADR-001, "The mill graph and hops"): a port
// of tools/hopsim.py. Every spread_tick game hours, each mill where a rumour
// is fresh passes it along each of its edges with
//
//   P(hop g->h) = 1 - exp(-rate(edge) x weight x n^2 x dt)
//
// and an arrival is recorded at the far end after the edge's travel time,
// with notability decayed by 0.9 within a place or 0.6 to another place.
// The rolls are fixed per (rumour, edge, tick), so a reload replays the same
// spread. Numbers only: versions are written later, when needed (Retell.cpp).

namespace RumourEngine::Spread {

    /** Forget the clock: a save was loaded. The next tick starts counting from then. */
    void Reset();

    /** Run the ticks game time has passed since the last call, record the hops, and announce
     *  arrivals. Worker thread, once a save is ready. */
    void Tick();

    /** A rumour added late with "happened" in the past: run its hops from `from` up to the ticks
     *  already simulated, with the same rolls those ticks would have made. Any thread. */
    void CatchUp(std::int32_t id, double from);

    /** Days for news to travel from one mill to another by road: 0.25 + distance / 40 cells,
     *  0.1 within one place, 2 when either has no position or they are in different worlds. */
    double TravelDays(const Mills::MillSet& set, const std::string& from, const std::string& to);
}
