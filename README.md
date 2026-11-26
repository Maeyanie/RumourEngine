# RumourEngine

A SkyrimNet plugin that gives Skyrim's people rumours to pass around.

When something worth telling happens to someone, it gets around: what they
saw, what was done to them, what the player got up to in front of them. It
reaches their neighbours first, then the next town, changing a little with
each retelling. A week later an innkeeper three holds away has heard a
garbled version, and passes it on to the player.

- **Mills.** People are grouped into *mills*, the groups that talk to each
  other daily: a town's commons, its court, its garrison, its underworld, a
  guild. Rumours live in mills, not in each person, so a whole town can know
  something without a memory being written into everyone.
- **Harvesting.** Every couple of game hours, RumourEngine looks at the
  SkyrimNet memories people have formed lately. SkyrimNet's decision model
  judges whether the owner would pass each one on, and how much they would
  tell, by who they are. What they would becomes a rumour in their mills, in
  their own words: they tell it themselves, and everyone else retells it as
  talk about what they said. Those who were there go by their own memories
  of it, so it never comes back to them as talk.
- **Quest endings.** When one of your quests ends, the decision model reads
  its journal and judges whether word of how it ended would get out, and how
  far: Dawnstar freed of its nightmares is talk in the Pale, the Emperor
  assassinated is news everywhere, a Thieves Guild job stays with the guild.
  What would get out becomes a rumour where you are. Quests you had finished
  before installing RumourEngine are never told.
- **Spreading.** Rumours hop between mills along roads, carriage routes,
  ships and shared members, faster the more notable they are. Each mill's
  version is retold from the one it came from, by a cheap model, when someone
  there first needs it: names blur, numbers grow, and each group adds its own
  slant.
- **Who has heard it.** Not everyone in a mill has heard everything. Gossips
  hear first, and trivia never reaches the tight-lipped. How much of a gossip
  someone is comes from their job and a reading of their SkyrimNet bio.
- **Telling.** NPCs have the talk going round their mills in their bio, as
  talk, not fact. Gossips bring it up, to each other and to the player (in
  taverns, by default), once the room has been quiet for a while. Nobody
  turns away from a conversation with you to gossip, or gossips in the middle
  of an OStim or SexLab scene.
- **Going stale.** Talk fades after a few days, longer the more notable it
  is. The biggest news, the kind people still bring up a year later, is kept
  as SkyrimNet world knowledge for those it reached: a war breaking out, a
  Jarl murdered, a dragon slain at a city's gates.
- **Followers and moves.** The party belongs to no mill while it travels. A
  dismissed follower joins the mills of wherever they settle, and their
  stories from the road get told there. Those who were with you when a quest
  ended never hear it back as talk, since they were there. NPCs that other
  mods move to a new home are noticed the same way.

Other mods can add and read rumours through a C++ API, a Papyrus API and a
ModEvent; FactionEngine uses it for faction news. See
[docs/API.md](docs/API.md).

## Requirements

- Skyrim with SKSE and the Address Library for SKSE Plugins:
  - AE 1.6.x (tested on 1.6.1170) or SE 1.5.97.
  - 1.7.99 and 1.7.104 are supported but haven't been played on yet, so
    treat them as experimental. A later 1.7 is refused until it's been
    checked.
  - Skyrim VR, with SKSE VR and the VR Address Library for SKSEVR, and Skyrim
    VR ESL Support as SkyrimNet asks. One player has run 1.0.0 on it without
    trouble, but it's still experimental.
- SkyrimNet 1.0 or later (or beta 26 or later), with an LLM set up.
- A decision provider in SkyrimNet, for screening memories, reading bios,
  judging quest endings and deciding what is kept as world knowledge. Without
  one, screening falls back to a chat prompt (the `rumour_screen` variant),
  gossip scores come from jobs alone, only the quests `quests.ini` lists are
  told, and only rumours of notability 0.8 or more are kept.

There is no ESP, and nothing is stored in your saves, so RumourEngine can be
added to or removed from a playthrough at any time.

## Installing

Install it with your mod manager. Then:

1. Load a save. RumourEngine builds the mills when the game starts, and opens
   the save's rumours once SkyrimNet's keep/clear prompt is answered.
2. Its settings are on SkyrimNet's dashboard, under the RumourEngine plugin.
   The defaults are meant to be played with. The ones most worth a look are
   who may tell the player and the party (taverns, by default), how readily
   memories become rumours, and the models for the `rumour_seed`,
   `rumour_retell`, `rumour_quest` and `rumour_settle` variants.
3. `SKSE/Plugins/RumourEngine/mills.ini` and `gossips.ini` describe the mills
   and the gossips, and `quests.ini` says which quests are always or never
   told. Other mods can patch them with `<Name>_mills.ini`,
   `<Name>_gossips.ini` and `<Name>_quests.ini` files beside them.
   Followers are recognised by the game's own follower markers. A follower
   mod that keeps its own faction instead, as Sofia does, needs a line in
   `mills.ini`'s `[Party]` section; Sofia's is there already.

## What it costs

- **The decision model:** one screening request per person with new
  memories at each harvest, one bio reading per named NPC (and again when
  their bio changes), one question per quest that ends, and one per rumour
  that might be kept as world knowledge.
- **`rumour_screen`:** if no Decision model is configured in SkyrimNet's settings,
  this model is used instead. If you have a Decision model (highly recommended)
  this never gets used.
- **`rumour_seed`:** one call per harvest that accepted anything, a few times
  a game day at most.
- **`rumour_quest`:** one call per quest ending that word of would get out, a
  few times a game week at most.
- **`rumour_retell`:** one call per rumour and town, when the player arrives
  somewhere the talk has got to. It is meant for a small, cheap model; GLM 5.3
  Flash is the tested default.
- **`rumour_settle`:** one call per rumour kept as world knowledge, which is
  rare: on a 57-day test save, only the declaration of war.

## Models we've tried

RumourEngine's calls go to whatever model each SkyrimNet variant is set to.
These are the ones tried so far, in November 2026 on OpenRouter. Names that
start with `~` are OpenRouter's aliases for a family's latest model.

**Keep reasoning low or off.** SkyrimNet gives each call 4,096 tokens by
default. At medium reasoning, Muse Spark spent nearly all of them thinking,
so seed calls carrying several memories came back empty or cut off. DeepSeek
Flash did the same once even at low. A memory left without a telling is
tried again at later harvests, and dropped after three tries.

### `rumour_seed`

This call writes a rumour's first telling: the account of the person who
remembers it, in their own words, which they tell themselves and every
retelling starts from. Accuracy matters more than style here: the telling
should say what happened, keep to the memory, and leave out what "only the
gist" holds back. NPCs put it in their own words when they bring it up anyway.

[`tools/panel/seed_panel.py`](tools/panel/seed_panel.py) gave each model 18
memories from a play save, three to a call, as a harvest sends them. The
panel ran before tellings were in the first person.

| Model | Reasoning | How it did | $/call | s/call |
| --- | --- | --- | --- | --- |
| Muse Spark 1.3 (`meta/muse-spark-1.3-contributor`) | low | The most faithful, and the best at holding back what "only the gist" hides. Plain, a little dry. At medium, given room to finish, it wrote no better and took twice as long. | 0.0005 | 11 |
| Claude Haiku 5.5 (`~anthropic/claude-haiku-latest`) | off | Nearly as faithful, and reads more smoothly. A few small slips: a song's composer turned into a singer, and song titles in quotes. | 0.0012 | 9 |
| GLM 5.3 (`~z-ai/glm-latest`) | low | The liveliest of those that stay mostly true, though it adds small details of its own. | 0.0024 | 2 |
| GLM 5.3 Flash (`~z-ai/glm-flash-latest`) | low | Vivid but loose. It kept feelings that "only the gist" leaves out, gave people descriptions nobody had given them, and folded two memories of one fight together. | 0.0003 | 4 |
| GPT Luna (`~openai/gpt-luna-latest`) | low | Accurate but thin: the juicy details of "everything" shrink to a phrase. | 0.0004 | 6 |
| DeepSeek Flash (`~deepseek/deepseek-flash-latest`) | low | Close to copying the memory out. Once it told a secret that "only the gist" should keep. | 0.0011 | 8 |
| Gemma 4 31B (`google/gemma-4-31b-it`) | off | Accurate but so terse that what made a memory worth repeating is gone. | 0.0002 | 1 |
| Qwen3 235B (`qwen/qwen3-235b-a22b-2507`) | off | Invents. It gave one woman a race her memory never mentions, a different one each run. | 0.0004 | 8 |

Muse Spark at low reasoning is the pick, with Claude Haiku 5.5 as the
alternative.

### `rumour_quest`

The models were compared on one quest ending, Waking Nightmare, from its
journal's opening and ending.

- **GLM 5.3 Flash** (`~z-ai/glm-flash-latest`) wrote the best tellings, in
  about 4 s, and is the pick. It sometimes fills gaps from what it knows of
  the game, naming Dawnstar or adding the temple's cultists. That isn't out
  of place in a rumour, but it may make things up for a modded quest it
  doesn't know.
- **Muse Spark** stayed true, but gave one bare sentence that left out what
  came of it.
- **Doubao Seed Character** (`bytedance/doubao-seed-character`, through
  NanoGPT) fell between the two.

### `rumour_retell` and `rumour_settle`

GLM 5.3 Flash came out of the stage 0 panel as the retell model
([stage0-results.md](docs/stage0-results.md)). Gemma 4 31B, Claude Haiku 5.5
and DeepSeek V4 Flash are the alternatives. GLM 5.3 Flash also did well
writing settled accounts.

### `rumour_screen`

Models for this haven't been extensively compared as it's only used in a
fallback role. A model which is good at analysis is recommended; whatever
you use for `meta` is likely a good pick.
This should be set to a regular LLM; don't use `typesafe/jev-1.14` or
another Decision model, as the calling conventions are different.
You only need to set up the SkyrimNet Decisions model, and RumourEngine 
will use whatever you have configured there.

### Decisions

What is kept as world knowledge (`promote.lasting`) is calibrated on Jev
(`typesafe/jev-1.14`), which most players will use. The setting's
description gives the equivalents for d3 and Kev.

## Removing it

1. On SkyrimNet's dashboard, turn off **Keep the biggest news as world
   knowledge**, and load your save. The log says how many world-knowledge
   entries were taken out. Left in, they would stay in SkyrimNet known to
   nobody, with a warning in SkyrimNet's log each time one is checked.
2. Remove the mod. Its per-save databases are in
   `SKSE/Plugins/RumourEngine/data/` (MO2's overwrite folder), and can be
   deleted.

## Logs

`RumourEngine.log` is beside SKSE's own logs, in
`Documents/My Games/Skyrim Special Edition/SKSE/` (`Skyrim Special Edition
GOG` for the GOG edition, `Skyrim VR` for VR). It records every rumour
added, every hop, retelling, telling and retirement, and what is kept as
world knowledge. With **Log what NPCs are shown** on, it also shows what each
NPC's bio is given.

## Building

- `SKSE/BuildDLL.ps1` builds the DLL with MSVC, Ninja and vcpkg
  (CommonLibSSE-NG).
- `BuildScripts.ps1` compiles the Papyrus scripts.
- `Deploy.ps1` copies the build into the MO2 mod folder, and `Package.ps1`
  builds the release zip from the same files, staged on their own.

## Documentation

- [ADR-001](docs/ADR-001-rumour-mills.md): the design and its decisions. That
  covers mills, hops, harvest and screening, retelling, delivery, promotion
  and persistence.
- [API.md](docs/API.md): the API for other mods. It is frozen at version 1:
  later versions only add to it.
- [stage0-results.md](docs/stage0-results.md) and [tools/](tools/README.md):
  the offline prototypes the design was tested with.

## Credits

- **[NarrativeEngine](https://github.com/pdusen/skyrimnet-plugin-narrative-engine)**
  by pdusen (Apache-2.0). Its gossip-propagation work came
  first, and we read its code to see how it did things. RumourEngine uses none
  of its code, but it borrows many of its ideas and findings:
  - location tiers from parent-location walks and keywords;
  - faction filtering and its size-band rules;
  - the screening questions (would they tell it, is it already going round,
    would anyone listen);
  - "drift, not replacement" for retelling;
  - an SIR model, applied here to mills;
  - what it learned about harvesting SkyrimNet memories.
- **FactionEngine**, for the prompt-side delivery: the bio block, the
  GameMaster line and the gossip starter.
- **SkyrimNet**, whose memories, decisions and prompts all of this runs on.

## Licence

Apache-2.0. See [LICENSE](LICENSE).
