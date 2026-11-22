# Changelog

## 1.0.0

The first release.

- **Mills.** People are grouped into the groups that talk to each other daily
  (a town's commons, court, garrison and underworld; guilds; factions), built
  from the load order at each game start. `mills.ini` and `*_mills.ini`
  patches add faction mills, links, secrecy and mill descriptions.
- **Harvesting.** People's recent SkyrimNet memories are screened by the
  decision model: would they tell it, how much of it, and how interesting is
  it. What passes becomes a rumour in their mills, and the teller remembers
  telling it. A long fight split over several memories is one rumour.
  Dismissed followers' stories from the road are told where they settle.
- **Spreading.** Rumours hop between mills along roads, carriage routes,
  ships, shared members and informants, faster the more notable they are.
  Each mill's version is retold from the one before by a cheap model, when
  someone there first needs it.
- **Who has heard it.** Each person's gossip score comes from their job
  (`gossips.ini`), a decision-model reading of their bio, re-read when the
  bio changes, and overrides. A rumour reaches a mill's gossips first, and
  trivia never reaches the tight-lipped.
- **Telling.** The talk is in NPCs' bios as talk, not fact, and gossips bring
  it up, to each other and, by the settings, to the player and the party.
  Nobody gossips mid-scene in OStim or SexLab. Anyone overhearing a telling
  carries it home.
- **World knowledge.** When the biggest news has run its course, the
  decision model is asked whether people will still talk of it in a year. If
  so, what people came to know of it becomes a SkyrimNet world-knowledge
  entry, for those it had reached only.
- **Quest endings.** When one of the player's quests ends, the decision
  model reads its journal: will word of how it ended get out, and how far?
  If it would, it becomes a rumour where the player is, told as the people
  there would tell it: Dawnstar freed of its nightmares, the Butcher of
  Windhelm caught, the Emperor assassinated. `quests.ini` says always or
  never for a quest or one of its endings. Quests a save had finished before
  are never told.
- **The player's setting.** SkyrimNet's setting text goes to every prompt
  that judges or writes rumours, so what counts as scandal follows your
  Skyrim.
- **History.** Each playthrough has its own database, and nothing is stored
  in saves. When SkyrimNet clears its history after loading an earlier save,
  RumourEngine takes back the same stretch.
- **API v1** for other mods: C++ (`RumourEngineAPI.h`), Papyrus
  (`RumourEngine.psc`) and ModEvents. Add, update, patch and retire rumours,
  with per-hold or per-mill versions, delays, witnesses, secrecy and
  proclamations; ask what an NPC has heard; listen for events. Frozen: later
  versions only add to it.
