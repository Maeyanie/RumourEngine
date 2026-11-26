# Changelog

## Unreleased

- **Followers with a faction of their own.** Sofia, travelling with the
  player, told a caged prisoner that "people in Whiterun are saying" the
  player had killed Sinding, which she had done with them. Her mod keeps her
  in its own faction rather than `CurrentFollowerFaction`, and clears her
  teammate flag while she idles near a player who has stopped. So whenever the
  player stood still she counted as a Whiterun local: given Whiterun's talk,
  free to gossip, and her memories open to the harvest. `mills.ini` has a new
  `[Party]` section for follower mods' own factions, with Sofia's in it, and
  patches can add more. Someone seen following also stays in the party while
  they're close to the player and not dismissed, whatever their mod does with
  the flag.
- **The party at a quest's ending.** Whoever was with the player when a quest
  ended is left out of that rumour's talk, then and once they're dismissed.
  They were there, and have their own memories of it.
- **Your own story, told by you.** When someone's memory became a rumour, it
  went back into their own bio as the talk going round, in its third-person
  telling, and they could be picked to pass it on, so they could tell their
  own story back as something they'd heard. A dismissed follower's adventures
  came back to them the same way. Now the first telling is theirs, in their
  own words, and they're the one who tells it, to the people around them,
  gossip or not. Everyone else, their own town included, has it retold as
  talk about what they told. Witnesses, and those who were with the player
  when a quest ended, tell it as people who were there, not as hearsay.
  Rumours already going round in a save are put right when it's loaded.
- **No turning away mid-conversation.** A bard talking with the player turned
  to someone else to pass on some gossip. The gossip starter waited only for
  20 seconds of quiet, which a player typing a reply can leave. Now it waits
  for 30 seconds of quiet, and a minute after the player last spoke or was
  spoken to. Whoever the player has talked with in the last three minutes is
  left out, neither telling nor told. The "most on their mind" line in bios
  now says to wait for a natural pause, not to cut into another subject, and
  the line the GameMaster reads says to wait until nobody's mid-conversation.
- **API: `origin.teller`.** Names whose own first-person account a rumour's
  `text` is: they tell it themselves, and every mill has it retold. Rumours
  with one show `teller` in `GetRumour`.

## 1.1.0

- **Skyrim 1.7.99 and 1.7.104.** RumourEngine now loads on them: it reads
  their Address Library (format 5) itself and declares that it can, as SKSE
  for 1.7 requires. Experimental until someone has played on them. Any other
  1.7 version is refused, with the reason in the log, until it's been
  checked.
- **Skyrim VR.** Checked against the VR Address Library, and one player has
  run 1.0.0 on it without trouble; still experimental. Light plugins (with
  Skyrim VR ESL Support) are now numbered correctly there, both for forms named by plugin
  in API requests and when the plugins are read for where NPCs live. The
  CommonLib RumourEngine is built on numbered them as full plugins on VR.
- **Names with accents.** Text from the game is in its code page, not
  UTF-8, so a name like "Jónsi" in a plugin couldn't go into JSON: the mills
  dump wasn't written ("invalid UTF-8 byte" in the log), and when someone
  with such a name had memories to screen, the error went uncaught and would
  most likely have crashed the game. Names, place names, editor IDs, ini
  files and the strings other mods pass in are now made UTF-8 as they're
  read, in Windows-1252, or Windows-1250 and 1251 for Polish, Czech and
  Russian games. Quest journals already were. An error RumourEngine's own
  thread doesn't expect is now logged and skipped rather than ending the
  game.
- **Log folder.** On 1.6.1130 and later, `RumourEngine.log`, the debug inbox
  and the mills dump went to `My Games/Skyrim.INI/SKSE/`, because of a
  CommonLib lookup that no longer matches the game. They are now beside
  SKSE's own logs in `My Games/Skyrim Special Edition/SKSE/`.

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
