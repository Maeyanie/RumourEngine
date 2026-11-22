#pragma once

// Lazy retelling (ADR-001, "Writing: seeding and retelling"). A mill's version
// of a rumour is written when someone first needs it: when the player enters
// a settlement where it has arrived, when it arrives where the player is, or
// when an NPC's prompt would show it. One `rumour_retell` call writes every
// step along the path from the nearest mill that has a version, each step
// retold from the one before, so drift builds up a hop at a time.

namespace RumourEngine::Retell {

    /** Listen for the player changing location. kDataLoaded. */
    void Install();

    /** A new save: forget what was asked, and drop answers still on their way. */
    void Reset();

    /** Where the player is now; retelling starts for the mills there. Main thread. */
    void PlayerMoved(RE::BGSLocation* location);

    /** The mill is one of the player's current settlement's. Any thread. */
    bool InPlayersPlace(const std::string& mill);

    /** Ask for this rumour's version in this mill. Cheap, any thread, including inside a decorator. */
    void Request(std::int32_t id, const std::string& mill);

    /** Start the calls asked for, a few at a time. Worker thread, once a save is ready. */
    void Pump();

    /** The line describing a mill to the retell prompt: mills.ini's, else one made from its kind. */
    std::string LineOf(const std::string& mill);
}
