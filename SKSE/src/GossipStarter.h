#pragma once

// Starts a telling when the player is within earshot of a gossip who has
// something fresh to pass on, standing near someone who may be told (ADR-001,
// "Delivery"). The shape is FactionEngine's, which works in play:
//   - SkyrimNet's GameMaster sees who is keen to talk, but with its default
//     model it carries on whatever scene is in front of it, so this doesn't
//     wait to be chosen;
//   - a worker thread polls, and only a quiet room (nobody has spoken for a
//     while, by SkyrimNet's event log) gets a look on the main thread;
//   - the look registers a direct_narration with the gossip as speaker. What is
//     said comes from the gossip's bio, which puts the same item first.
// Never into a conversation, a scene (vanilla, OStim or SexLab), combat, a menu, or anyone busy. Once per
// gossip per item, and a minimum gap between starts.
//
// Who may be told follows starter.tell_player and starter.tell_party: being
// picked as the listener is an interruption, and a follower told something on
// the road stops the party while they talk it over.

namespace RumourEngine::GossipStarter {

    void Start();
    void Stop();

    /** A save was loaded: forget who has told what, and let the world settle first. */
    void Reset();
}
