#pragma once

#include "Mills.h"
#include "Rumours.h"

// What NPCs know of the talk, in their prompts (ADR-001, "Delivery"):
//   get_rumourengine_talk(actorUUID)  the bio's "## Local talk" block
//   get_rumourengine_news(actorUUID)  the GameMaster's "Also keen to pass on" line, gossips only

namespace RumourEngine::Delivery {

    /** Register the decorators. kDataLoaded, after SkyrimNet is bound. */
    void RegisterDecorators();

    /** Following the player now: in no mill, told nothing from here. The teammate flag,
     *  CurrentFollowerFaction or a mills.ini [Party] faction says so; and someone seen following
     *  stays in the party while they're close to the player and not dismissed, as some follower
     *  mods clear the flag while the follower idles near a player who has stopped. Any thread. */
    bool InPlayersCompany(RE::Actor* actor);

    /** A save is loaded or closed: forget who was seen following. */
    void ResetParty();

    /** Main thread, every few seconds: ask about everyone loaded, so a follower is seen
     *  following while the game still marks them, before their mod clears the flag. */
    void WatchParty();

    /** In an OStim or SexLab scene: nobody starts a telling, or is told one, mid-scene. */
    bool InIntimateScene(RE::Actor* actor);

    /** 0 to 1: their job, their bio's reading and gossips.ini (Gossip.h). Asking about a loaded
     *  actor also has their bio read if it never was, or has changed. */
    double GossipScore(RE::Actor* actor);

    /** The same, for a mill member who needn't be loaded: the hop simulation weights shared members by it. */
    double GossipScoreOf(const Mills::Person& person);

    /** What this gossip most wants to pass on: their freshest item that isn't about them and
     *  that nobody else has just told. Empty for anyone below the teller score, or in an intimate
     *  scene. The bio and the
     *  GossipStarter both ask this, so the item a telling is started for is the one their bio
     *  puts first. */
    std::optional<Rumours::Heard> PickTelling(RE::Actor* actor, double now);

    /** The mill's place for prompts: "Whiterun", "the East Empire Company". */
    std::string PlaceOf(const std::string& mill);

    /** The bio block and the GameMaster line, as the decorators render them. */
    std::string BuildTalk(RE::Actor* actor);
    std::string BuildNews(RE::Actor* actor);
}
