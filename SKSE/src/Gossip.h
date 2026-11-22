#pragma once

#include "Mills.h"

// How much of a gossip each person is, 0 to 1 (ADR-001, "Gossip score"):
//   - their job, from gossips.ini [Jobs] and its *_gossips.ini patches;
//   - moved up or down by one Decisions reading of their SkyrimNet bio, kept in the
//     save's database and read again whenever the parts of the bio it reads change,
//     the authored file or a generated one;
//   - or an override from gossips.ini [People], which wins outright.
// The score decides who starts tellings, how low their harvest bars sit, how soon
// they hear what is going round their mills, and how much they link two mills.

namespace RumourEngine::Gossip {

    /** gossips.ini, and the save's bio readings. Once the mills are built and the database is open. */
    void Load();

    /** A different save is being loaded: forget the readings and anything under way. */
    void Reset();

    /** 0 to 1. Any thread; calls nothing in SkyrimNet. */
    double ScoreOf(const Mills::Person& person);

    /** A fixed number for a person's rolls, the same after a load-order change where it can be. */
    std::uint64_t RollKey(const Mills::Person& person);

    /** This person matters just now (their bio is rendering, they are being screened): their bio
     *  is read if it never was, or looked at again if a while has passed. Cheap; any thread. */
    void Want(RE::Actor* actor, const Mills::Person& person);

    /** Look at the bios asked for and send what needs reading. The worker thread. */
    void Pump();

    /** Counts changes to anyone's score, so what is built from them knows to rebuild. */
    std::uint64_t Version();
}
