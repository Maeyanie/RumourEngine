#pragma once

// Promotion to world knowledge (ADR-001, "Lifecycle"). A rumour that retires stale having reached
// a fair share of people is a candidate (Rumours.cpp marks it). The decision model is asked
// whether people will still talk of it in a year; one that will is kept: the writer model sets
// down what people came to know of it, from how it was being told by then, and that becomes a
// SkyrimNet world-knowledge entry, retrieved when relevant, for those the rumour had reached:
//   condition  contains(get_rumourengine_heard(actorUUID), "|<id>|")

namespace RumourEngine::Promote {

    /** Another save: forget calls under way. */
    void Reset();

    /** Ask, write and make what is waiting; take entries out when promotion is off. The worker. */
    void Pump();

    /** SkyrimNet cleared its history after `gameDays`: remove the entries made since, before the
     *  rows are taken back. The session thread. */
    void TakeBack(double gameDays);
}
