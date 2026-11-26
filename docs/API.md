# RumourEngine API

*Version 1, frozen 2026-11-12, after FactionEngine adopted it.
`GetApiVersion()` returns 1. Later versions stay backwards-compatible: they add
functions, JSON fields and listener events, and nothing here changes meaning or
goes away. Ignore JSON fields you don't know. The design and its reasons are in
[ADR-001](ADR-001-rumour-mills.md); this page describes what the DLL does
now.*

RumourEngine keeps rumours in **mills**, the groups of people who gossip
together: a town's commons, its court and its garrison, a farm, an enclave, and
guilds that span holds. A rumour starts in some mills and hops to their
neighbours over the days that follow. Each mill tells it in its own words, and
it goes stale after a while. RumourEngine puts what a person's mills are saying
into that person's SkyrimNet prompt itself, through the `rumourengine_news` and
`rumourengine_talk` bio sections. A client only adds rumours. It doesn't render
them.

**How to send one.** Describe the event as someone who was there would tell
it, at the place where they'd tell it, and let RumourEngine do the rest. Each
hop is a retelling, so the story drifts as it travels, the way talk does.
Don't write your own version for every hold. A version you give pins that
mill's wording, so nothing drifts there and nothing from elsewhere reaches it.
Keep `versions` for wording that mustn't drift, such as a proclamation or a
posted notice, and for a faction's own account (`as_fact`, below).

There are three ways in:

| Way | Link | Use it for |
| --- | --- | --- |
| C API | `GetProcAddress`, through `RumourEngineAPI.h` | SKSE plugins: everything, with return values and events |
| Papyrus | `RumourEngine.psc`, global natives | scripts that can take a compile-time dependency |
| ModEvents | none | scripts that can't, and quick tests |

## Conventions

- **Optional dependency.** `RumourEngineAPI::Bind()` returns false and leaves
  every pointer null when RumourEngine isn't installed. Call it at
  `kDataLoaded` or later.
- **Strings** go in as UTF-8. A query returns a `char*` that RumourEngine
  allocated. Release it with `RumourEngineAPI::Free` (or hold it in
  `RumourEngineAPI::Owned`), never your own `free`, because the two DLLs may
  use different C runtimes. A null return means not found or failed, and
  `GetLastError()` says why, on the same thread.
- **JSON** carries anything structured, so fields can be added without
  breaking the ABI. Unknown fields are ignored, both ways.
- **Threads.** Every function is safe from any thread. `AddRumour` returns at
  once, and any LLM work happens later on RumourEngine's own threads.
- **Readiness.** Mill queries work from `kDataLoaded`. Everything about
  rumours fails until a save's database is open and SkyrimNet has settled its
  timeline, which happens a second or two after a load, or after the player
  answers SkyrimNet's keep/clear prompt. Wait for the `ready` event or the
  `RumourEngine_Ready` ModEvent, or check `IsReady()`.
- **Per save.** Each save has its own database. Loading another save closes
  it, as does quitting to the main menu, and a load that SkyrimNet rolls back
  takes back RumourEngine's rows from after the save too. Nothing is stored in
  the save itself.
- **Forms** in a request are `"Plugin.esp:0xLOCALID"` (the id within that
  plugin, which also works for light plugins), a hex string of a runtime form
  id (`"0x14"` for the player), or a runtime form id as a number.
- **Actors** (`subjects`, `heard_by`, `origin.actor`) can also be given by
  plain name: `"Ulfric Stormcloak"`. A string with a `:` or a leading `0x` is
  a form, and anything else is a name. A name is matched in full, ignoring
  case, against the player and the named NPCs the mills know, placed or not.
  Where several have it, it means all of them: mostly that's a quest's
  stand-in for the same person (the burned Astrid, the Nazir of the Sanctuary
  attack) or a mod's copy, which never talks anyway. Two different people who
  share a name both count, so give a form where that matters. An unknown name
  is logged and skipped.
- **Actors in events** are keys. A key is the actor's SkyrimNet UUID as a
  decimal string, which survives load-order changes, or `"form:XXXXXXXX"` when
  SkyrimNet doesn't know them.
- **Time** is in game days, as `GetCurrentGameTime` counts them: day 115.5 is
  noon on day 115.

## Mills

A mill id is built from its location's editor id, such as `whiterun.court`,
`riverwood`, `winterholdcollege`, or `darkbrotherhood` for an institution. Ids
stay the same across saves and load-order changes, and a mill exists only
while the mod adding its location is installed. Membership is rebuilt for every
save, because who is enabled differs between saves. The civil war, for one,
swaps whole garrisons.

Kinds: `settlement` (a village or farm with one mill), `commons`, `court`,
`garrison` and `underworld` (the strata of a city), `enclave` (a place apart:
the College, the Thalmor Embassy), and `institution` (a guild or faction
across holds). Someone belongs to one place mill and any number of
institutions.

**Faction mills** are institutions that `mills.ini` defines in its
`[Factions]` section (`id = Name: FactionEditorID, …`). Each one holds the
named members of those factions, wherever they live. A member keeps their
town's mills as well. Where both of their mills have a rumour, they hear the
faction's telling. A named member with no place of their own, such as a legate
on campaign, is in the faction mill alone. Members of a listed faction count
as people even when the faction is hostile to strangers, as Volkihar is.
These ship:

| Group | Mill |
| --- | --- |
| Imperial Legion | `legion` (`CWImperialFaction`) |
| Stormcloaks | `stormcloaks` (`CWSonsFaction`) |
| Thalmor | `thalmor` (`ThalmorFaction`); the embassy is also `thalmorembassy` |
| Companions | `companions` (`CompanionsFaction`) |
| Dark Brotherhood | `darkbrotherhood` (`DarkBrotherhoodFaction`) |
| Silver-Blood family | `silverblood` (`MarkarthTreasuryHouseFaction`), from FactionEngine's patch |
| Black-Briar family | `blackbriar` (`RiftenBlackBriarHouseFaction`, `RiftenBlackBriarMeaderyFaction`), from FactionEngine's patch |
| Volkihar | `volkihar` (`DLC1VampireFaction`); the castle is also `vampirecastle` |
| Thieves Guild | `riften.underworld`, a place mill whose 22 members are all the Guild |
| College of Winterhold | `winterholdcollege`, a place mill |
| Dawnguard | `hunterhq`, a place mill whose members are all Dawnguard |

**Patches.** A mod changes the mills without editing `mills.ini` by shipping
its own `SKSE/Plugins/RumourEngine/<Name>_mills.ini`, as SPID reads
`*_DISTR.ini`. RumourEngine reads `mills.ini`, then the patches in file-name
order, every time it builds the mills (each load). Any section can be patched:

- A line replaces an earlier file's line with the same key. In `[Links]`, the
  key is the two mills.
- A key with nothing after the `=` takes the earlier line away. In `[Links]`,
  `none` takes a link away instead.
- The log names each file read and each line that replaces another file's.
  Mistakes are logged with the file they're in.

`[Retell]` (`id = eager`) has a mill's versions written as soon as a rumour
arrives, wherever the player is, rather than when someone there first needs
one. Use it for a mill whose talk your mod reads out in prompts of its own:
lazily, the first query asks for the version and only a later one gets it.
`lazy` undoes another file's `eager`. Each eager mill costs a retell call for
most rumours that reach it, so list only the mills you read. `GetMills()`
shows `"eager": true` for them.

```ini
; SKSE/Plugins/RumourEngine/MyMod_mills.ini
[Factions]
myguild = the Guild of Example: MyGuildFaction
[Lines]
myguild = The Guild of Example: ... They care about ...
[Retell]
myguild = eager
```

**Gossip scores** can be patched the same way: `<Name>_gossips.ini` is read
after `gossips.ini`. `[Jobs]` is `FactionEditorID = score` (0 to 1, with
`default` for people in none), and `[People]` is `Name = score` or an NPC's
editor ID, which wins outright over their job and their bio.

**A person's mills can change during play.** A dismissed follower who stays
in another town for a game day moves into its mills there, and so does someone
the builder couldn't place who turns out to live somewhere. Going back home
clears it. So ask for an actor's mills when you need them, rather than caching
them for a whole session.

**The player's party** is in no mill while it travels: its members aren't told
the talk, don't tell it, and their memories wait until they're dismissed and
settle. Someone is in the party when the game marks them as the player's
teammate, or they're in `CurrentFollowerFaction` at rank 0 or more, or in a
follower mod's own faction that `mills.ini` lists under `[Party]`
(`mod = FactionEditorID, …`; Sofia's ships). Someone seen following also stays
in the party while they're within half a cell of the player and not in
`DismissedFollowerFaction`, since some follower mods clear the teammate flag
while the follower idles. A follower mod with a faction of its own can ship
`<Name>_mills.ini` with its `[Party]` line. When one of the player's quests
ends, everyone in the party then is in that rumour's `known_better_by`.

## C API

Copy `RumourEngineAPI.h` into your project. It's in the download at
`SKSE/Plugins/RumourEngine/RumourEngineAPI.h`, and in this repository at
[SKSE/include/RumourEngineAPI.h](../SKSE/include/RumourEngineAPI.h). It finds
RumourEngine at run time, so your mod builds without it and runs as before
where it isn't installed.

### Example

This is everything a mod needs to send its rumours to RumourEngine when it's
installed. Change `MyMod` to your mod's name. It uses nlohmann::json, but any
JSON library will do, since the request is just a string.

```cpp
// MyModRumours.h: RumourEngine as an optional dependency.
#pragma once

#include "RumourEngineAPI.h"

#include <nlohmann/json.hpp>

namespace MyModRumours {
    inline bool g_available = false;

    // Call once, on SKSE's kDataLoaded message.
    inline void Init() {
        g_available = RumourEngineAPI::Bind() && RumourEngineAPI::GetApiVersion() >= 1;
        if (g_available) SKSE::log::info("RumourEngine found: rumours will spread through its mills");
    }

    // Hands a rumour to RumourEngine. It starts with the people of `where`, spreads to other towns
    // over the next days (further the more notable it is), and is retold in each town's own words.
    // RumourEngine puts it in the bios of the people it has reached.
    //
    //   id          your own id for the rumour. Sending the same id again changes nothing.
    //   text        the rumour as people tell it, in UTF-8.
    //   where       where it's going round: any place in a town or farm, or elsewhere in a hold,
    //               which starts it in the hold's capital. Null means wherever the player is (read
    //               from the game, so call that from the main thread).
    //   notability  0 to 1. As a guide, 0.3 stays in the town, 0.5 reaches the next hold, and 0.8
    //               most of Skyrim within a week.
    //
    // Returns RumourEngine's id for the rumour, or 0 when RumourEngine isn't installed, no save is
    // loaded yet, or it refused the rumour (RumourEngine.log says why).
    inline std::int32_t Submit(const std::string& id, const std::string& text, RE::BGSLocation* where = nullptr,
                               double notability = 0.4) {
        if (!g_available || !RumourEngineAPI::IsReady()) return 0;
        if (!where) {
            if (auto* player = RE::PlayerCharacter::GetSingleton()) where = player->GetCurrentLocation();
        }
        if (!where) return 0;

        nlohmann::json request = {
            { "source", "MyMod" },
            { "key", id },
            { "text", text },
            { "origin", { { "location", where->GetFormID() } } },
            { "notability", notability },
            { "if_absent", true },  // leave this out to replace a rumour already sent with this id
        };
        // Optional: who it's about, as form IDs. They hear it as "People are saying this about you".
        // request["subjects"] = { actor->GetFormID() };

        // error_handler_t::replace: a byte that isn't UTF-8 becomes U+FFFD instead of an exception.
        const auto json = request.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
        return RumourEngineAPI::AddRumour(json.c_str());
    }
}
```

```cpp
// in SKSE's message listener
case SKSE::MessagingInterface::kDataLoaded:
    MyModRumours::Init();
    break;

// where a rumour is made
MyModRumours::Submit(rumourId, rumourText, location, 0.4);
```

A location form is the simplest origin. RumourEngine walks up from it to the
town or farm it's part of, and for a place in none, such as a cave or the
wilds, starts the rumour in the hold's capital. AddRumour, below, has the
other origins and every field. To hear what
becomes of your rumours, register a listener as well (see Events):

```cpp
RumourEngineAPI::RegisterListener("ready,retired", OnRumourEvent, nullptr);
```

### Functions

| Function | Returns |
| --- | --- |
| `int GetApiVersion()` | 1 (0 in pre-release builds) |
| `bool IsReady()` | whether rumour calls work yet |
| `int32_t AddRumour(const char* requestJson)` | the rumour's id (>0), or 0 when rejected |
| `bool RetireRumour(int32_t id)` | false if no such live rumour |
| `bool RetireRumourByKey(const char* source, const char* key)` | false if no such live rumour |
| `char* GetRumour(int32_t id)` | one live rumour, or null |
| `char* GetRumourByKey(const char* source, const char* key)` | the live rumour with that key, or null |
| `char* GetRumoursForActor(uint32_t formId, int maxCount)` | what the actor's mills are saying, what concerns them first |
| `char* GetMillsForActor(uint32_t formId)` | the actor's mill ids, the one they live in first |
| `char* GetMills()` | every mill |
| `bool HasHeard(int32_t id, uint32_t formId)` | whether the actor knows the rumour |
| `char* GetLastError()` | this thread's last failure, or null |
| `uint64_t RegisterListener(const char* typesCSV, cb, void* user)` | a listener id (>0) |
| `bool UnregisterListener(uint64_t id)` | false if no such listener |
| `void Free(void* p)` | releases a returned string |

### AddRumour

```jsonc
{
  "source": "FactionEngine",          // required: the calling mod; scopes "key" and appears in logs
  "key": "event-1234",                // optional: the same source and key update this rumour later
  "text": "…",                        // required: the first telling, as someone who was there would say it
  "origin": { "location": "Skyrim.esm:0x01A26F" },  // required: where it starts, one of
                                      //   { "actor": form }     their mills, or where they are now if in none
                                      //   { "location": form }  the mills of that place
                                      //   { "settlement": "Solitude" } every mill of that town
                                      //   { "mills": ["whiterun.court", …] }
                                      //   { "hold": "Whiterun" } every mill in the hold
  "notability": 0.6,                  // 0–1, default 0.5: how far it travels and how long it stays fresh
  "secrecy": 0.0,                     // 0–1, default 0: how readily it is kept from outsiders
  "scope": "spread",                  // spread (default): hops on from the origin
                                      // proclaimed: reaches every mill, later the further it is
                                      // local: stays in the origin mills
  "subjects": ["0x14"],               // who it is about
  "reach": ["legion"],                // optional: more mills that know it first-hand, from "happened" or now
  "versions": {                       // optional: your own tellings, used instead of retelling
    "mill:legion": { "text": "…", "as_fact": true },  // a version can be marked as what they know
    "mill:riften.court": "…",         //   for one mill (wins over its hold)
    "hold:The Rift": "…",             //   for every mill in a hold
    "*": "…"                          //   for every mill the others don't cover
  },
  "retell": true,                     // false: mills without a version get "text" word for word
  "retell_origin": false,             // true: the origin mills retell "text" too, as hop 0
  "fresh_days": 3.0,                  // optional: overrides the notability-based freshness
  "delay_hours": 6.0,                 // optional: game hours before the origin mills know
  "happened": 112.5,                  // optional: the game day it happened, if that was earlier
  "heard_by": ["Skyrim.esm:0x01A694"],// optional: people who certainly know it, whatever their mill
  "known_better_by": ["General Tullius"], // optional: people you give a fuller account; kept out of their talk
  "if_absent": false,                 // true: if this source and key exist, change nothing
  "patch": false,                     // true: change only the fields given (see Updating)
  "topics": ["politics"],             // optional: stored, for the interest table to come
  "promote": "auto"                   // auto / always / never: kept as world knowledge once it has run its course
}
```

- **Origin.** The origin mills tell `text` word for word, or their entry in
  `versions`. With `retell_origin: true` they retell it too, as the first hop,
  when someone there first needs it. Then `text` can be a plain account, or
  even data, that RumourEngine turns into talk.
- **Which origin.** A location gives the one mill whose talk it belongs to: a
  castle's court, a barracks' garrison, else the town's commons. A settlement
  gives every mill of that town at once (commons, court, garrison,
  underworld), for news the whole town knows the moment it happens, such as
  the Legion marching out of Solitude. It doesn't take in the rest of the
  hold, or the faction and institution mills, which hear it in the usual way
  or through `reach`. A settlement is matched by its in-game name, ignoring
  case (`"Solitude"`), or by its id in mill ids (`"solitude"`). A name more
  than one settlement has is refused, and then a location form in the town
  works instead. A small place folded into a bigger one's mill (see
  `GetMills`) gives that mill. A hold is matched by its in-game name, with or
  without " Hold" and ignoring case (`"Whiterun"`, `"The Rift"`), or by its
  id from `GetMills` (`"whiterunhold"`). Prefer any of these to mill ids,
  because then the mills come from the current build.
- **Spreading.** Each hop takes travel time and costs notability: ×0.9 to
  another mill of the same town, and to another place ×(0.6 + 0.3 ×
  notability), so big news keeps its size. How readily it is carried to
  another place rises steeply with notability (to the 4th power), so minor
  talk stays home while news at 0.9 reaches most of Skyrim within a week. It
  stops below 0.15, or at 80 mills. These are settings in SkyrimNet's plugin
  config, and `tools/spreadsim.py` shows what a request would do under them.
  As a guide, from a city: 0.3 stays in its town or nearby, 0.5 reaches a
  dozen mills and usually the next hold, 0.6 the region, 0.8 most holds
  within a week, 0.9 nearly everywhere. A proclaimed rumour skips the hops and reaches every mill
  after the travel time from the nearest origin mill. A mill that has no
  version of its own gets a retelling, written from its parent's version when
  someone there first needs it, or as soon as it arrives near the player.
  Someone who overhears a telling and carries it home takes what they heard,
  retold for their own people, unless you gave their mill a version. A faction mill has no place on the map, so
  news reaches it through its nearest member's town.
- **Freshness.** A mill knows it for `2 + 6 × notability` days from arriving
  (by default, or `fresh_days`). When it is stale everywhere it retires. Once
  it has been told 3 times there in the player's hearing, gossips stop
  bringing it up unprompted. It stays in their bios until it goes stale.
- **`secrecy`** keeps a rumour among the people who have it. A mill passes
  it on only if its readiest gossip rates above the secrecy, and a gossip
  only brings it up unprompted if they rate above it. Ratings come from jobs,
  give or take 0.1. Most people are 0.45, guards 0.3, court wizards 0.2,
  merchants 0.7, bards and inn servers 0.85, and innkeepers 0.9. So nearly
  every mill passes on anything below about 0.5. From 0.6 only mills with
  merchants, bards or an inn do, which leaves out faction mills of officers
  and family. Above 0.9 only some inns do, and at 1 nothing leaves the mills
  it starts in. Those in a mill whom it has reached still know it, and can
  talk about it if asked. A named person's rating also moves with what their
  bio shows of them (see Who has heard it, below).
- **`versions`.** A mill takes its `mill:` version, else its hold's, else
  `*`, else a retelling. With `*` given, no mill is ever retold, which suits
  news whose wording matters more than its drift. A version given as
  `{ "text": "…", "as_fact": true }` is what that mill knows rather than
  talk. Its members see it in its own bio section, "What Tullius knows as one
  of the Imperial Legion", not under "Local talk". It doesn't get out by being
  copied: a hop or an overheard telling carries a retelling of it, which is
  ordinary talk, and `secrecy` says how readily that happens.
- **`reach`** adds mills that know it first-hand without being where it
  started, typically a faction that took part or was told. Each one knows it
  from `happened`, or from now, with no `delay_hours`. The stored origin is
  left alone. A reach mill with no version of its own tells `text`, as an
  origin mill would. A faction mill told this way (or as the origin) knows it
  everywhere at once, so its members pass it on where they live, as their own
  town's talk.
- **`delay_hours`** is for adding a rumour when its event happens. The town
  still hears it some hours later. Hops, proclaimed arrivals and freshness all
  start from then. Until that time nobody there tells it, hears of it or
  passes it on.
- **`happened`** is for news sent late. A new rumour starts at that day (at
  most 30 days back), plus any `delay_hours`, and the hops it would have made
  since are made at once, with the same rolls the regular ticks would have
  made. Freshness counts from then too, so very old news arrives already
  stale and retires. On an update it applies only to the mills the update
  adds, through `reach` or a new origin, and the missed hops are replayed
  from them. An update that adds no mill replays nothing, so sending the same
  request again changes nothing.
- **`subjects`** are matched by name in each version. A subject a version
  names hears it as "People are saying this about you", and `about_them` is
  true for them in `GetRumoursForActor`.
- **`heard_by`** makes `HasHeard` true for those people, so a gossip with
  the rumour picks someone else to tell it to. Use it for witnesses. It doesn't
  put the rumour in their prompt if their own mills don't have it.
- **`known_better_by`** lists people you tell about it yourself, more fully,
  such as a faction's leaders once their faction knows it. It is left out of
  their talk and fact sections and their own gossip, so they don't hear it
  twice. `HasHeard` is true for them. A patch replaces the list, so send the
  whole list each time.
- **Updating.** A second call with the same `source` and `key` while the
  rumour is live replaces its text, origin, versions and the rest. It keeps
  the id and the mills it has reached, and freshness only grows. Retellings
  are kept as long as `text` is unchanged; a new `text` drops them, to be
  written again from it. Your own versions are always taken from the latest
  request, so one left out goes.
- **`patch: true`** changes only what the request gives. Every other field
  keeps its stored value, and `versions` merges into the stored ones by key,
  with `null` removing one. Changing one faction's or hold's version touches
  only the mills it covers. A patch with no live rumour behind it is an
  ordinary add, so it needs `text` and `origin`.
- **`if_absent: true`** returns the id of the rumour with that `source` and
  `key`, live or retired, without changing it. Only a new key is added. It
  makes resending everything recent at each `ready` safe.
- **Retired keys.** Without `if_absent`, a key whose rumour has retired
  brings it back under its old id, starting again from the origin. If it had
  been kept as world knowledge, that entry is taken out: it is news again.
- **`promote`.** When a rumour goes stale everywhere, `auto` makes it a
  candidate for world knowledge if it reached at least `promote.min_reach`
  (a quarter) of the mills' people. SkyrimNet's decision model is then asked
  whether people will still talk of it in a year, and it is kept if the answer
  is at least `promote.lasting` (0.4, tuned on Jev 1.14). Without a decision provider, a
  notability of `promote.notability` (0.8) or more decides instead. `always`
  keeps it without asking, `never` never keeps it. A rumour you retire
  yourself is kept only with `always`. What is kept is a world-knowledge entry
  for the people it had reached: "Old news from Evening Star of 4E 201, talked
  about across Skyrim:" and an account of what people came to know of it,
  written from how it was being told by then. Its condition is
  `contains(get_rumourengine_heard(actorUUID), "|<id>|")`. FactionEngine's
  marches all carry notability 0.9, so notability alone can't tell a war's
  outbreak from one more skirmish; the question can. Send `always` for the
  events you know are history, and `never` for those you keep track of
  yourself.
- **Not screened.** Rumours from the API skip RumourEngine's own screening.
  The caller has already decided they're worth telling.
- **Rejected** (0, with the reason in `GetLastError`): no `source` or `text`,
  an unknown `scope`, an origin that names no mill, no save open yet ("no
  save's database is open yet"), or a field of the wrong type ("bad request:
  …"). An unknown subject, `heard_by`, `known_better_by` or `reach` entry,
  or version key, is only logged.

### Queries

`GetRumour(id)`:

```jsonc
{
  "id": 12, "source": "FactionEngine", "key": "event-1234",
  "text": "…", "notability": 0.6, "secrecy": 0.0, "scope": "spread", "created": 115.32,
  "retell_origin": false, "known_better_by": [{ "key": "…", "form": 20, "name": "General Tullius" }],
  "topics": ["politics"],
  "subjects": [{ "key": "4627100", "name": "Rennard", "form": 20 }],
  "mills": [
    { "id": "whiterun.court", "arrived": 115.57, "fresh_until": 121.17, "notability": 0.6,
      "hops": 0, "airings": 1, "version": "…", "fact": false, "names_subjects": ["4627100"] }
  ]
}
```

A mill whose retelling isn't written yet shows the first telling as its
`version`. `arrived` can be in the future, for a delay or a hop on its way.

`GetRumoursForActor(formId, max)` returns what the actor has heard of the talk
going round their mills now: arrived, not stale, written, and got as far as
them (see Who has heard it). Talk about them comes first, then their guild's
or faction's, then the freshest. Someone in the player's party gets
`[]`, because the party isn't in any mill while it travels. A version that
isn't written yet is left out and requested, so it can appear on a later call.
A rumour whose `known_better_by` lists them is left out.

```jsonc
[{ "id": 12, "mill": "whiterun.court", "text": "…", "arrived": 115.57,
   "notability": 0.6, "about_them": false, "fact": false }]
```

`GetMillsForActor(formId)` takes a reference or a base form and returns
`["whiterun.commons", "companions"]`, with the mill they live in first.
`GetMills()` returns one entry per mill:

```jsonc
[{ "id": "whiterun.court", "kind": "court", "settlement": "whiterun", "hold": "whiterunhold",
   "size": 14, "pos": [1234.0, -5678.0], "eager": false }]
```

`settlement` and `hold` are ids made from location editor ids, as mill ids
are. Institutions have no `settlement`, and `pos` is null when the position
isn't known. `size` counts the mill's members. `eager` is true for a mill
whose versions are written as soon as a rumour arrives (`[Retell]`).

`HasHeard(id, formId)` is true once the rumour has reached the actor in any of
their mills, even if it has gone stale there since, or when they were in
`heard_by`, were in earshot of a telling, or remembered it themselves. Pass the
reference rather than the base form.

**Who has heard it.** A rumour that arrives in a mill gets round its people
over a day or two, gossips first, and trivia never reaches those who don't
gossip. Each person has a fixed roll per rumour, so the answer is the same
every time and only ever turns from no to yes. Until it has reached someone,
`HasHeard` is false for them, `GetRumoursForActor` leaves it out, and their
bio doesn't show it. Some people have always heard it at once:

- anyone in `heard_by`, in earshot of a telling, or who saw it themselves;
- everyone in a mill marked `[Retell] eager`, so a mill your mod reads out
  knows what reaches it as soon as it does;
- everyone in a mill whose version is `as_fact`;
- everyone, for a `proclaimed` rumour;
- anyone who isn't one of the mills' people (an actor the builder never
  placed), who is taken to know what their mills do.

If your mod needs someone outside an eager mill to know a rumour the moment it
is added, list them in `heard_by`.

### Events

`RegisterListener("seeded,retired", cb, user)` takes a comma-separated list of
types, or an empty string for all of them. Events arrive in order on one
RumourEngine worker thread, never on the thread that caused them. Don't call
`RE::` from the callback; hand the work to the main thread through SKSE's task
interface. The string is valid only during the call, and you don't free it.

| Type | Payload | When |
| --- | --- | --- |
| `ready` | `save`, `day` | a save's database is open |
| `seeded` | `id`, `source`, `key`, `updated`, `mills`, `arrives` | a rumour was added or updated: by any client, and by RumourEngine itself (`source` "RumourEngine"): its harvest (`key` "mem:<memory id>") and quest endings (`key` "quest:<quest editor ID, lowercased>") |
| `arrived` | `id`, `mill` | it reached a mill: a hop, a proclaimed arrival, a delayed origin when its time comes, a witness's mill, or someone who overheard it carrying it home |
| `aired` | `id`, `mill`, `teller`, `listener`, `earshot` | someone told it; `listener` is "" when it was told to no one in particular |
| `retired` | `id`, `source`, `key`, `why`, `promote` | `why` is "by request" or "stale everywhere"; `promote` is true when it is a candidate for world knowledge |
| `promoted` | `id`, `source`, `key`, `knowledge_id`, `text` | its world-knowledge entry was made: SkyrimNet's id for the entry, and its text |

Every payload also has `"type"`. Filter on `source` to see only your own
rumours. `arrives` in `seeded` is the day the origin mills know it, which is
later than now when `delay_hours` was given.

## Papyrus

`RumourEngine.psc` holds global natives only, so nothing is attached to
anything and nothing goes into saves.

```papyrus
int      Function GetApiVersion() global native
bool     Function IsReady() global native
int      Function AddRumour(string asSource, string asText, ObjectReference akOrigin, \
                            float afNotability = 0.5, Actor[] akSubjects = None, string asKey = "") global native
int      Function AddRumourInMill(string asSource, string asText, string asMill, \
                                  float afNotability = 0.5, string asKey = "") global native
int      Function AddRumourJson(string asRequestJson) global native
bool     Function RetireRumour(int aiId) global native
bool     Function RetireRumourByKey(string asSource, string asKey) global native
string[] Function GetRumoursFor(Actor akActor, int aiMax = 3) global native
string[] Function GetMillsFor(Actor akActor) global native
bool     Function HasHeard(int aiId, Actor akActor) global native
```

`AddRumour` starts in `akOrigin`'s mills: an actor's own, or those of any other
reference's location. `AddRumourJson` takes the full request above.
`GetRumoursFor` returns only the texts. Failures are logged rather than
returned.

## ModEvents

No compile-time link is needed:

- **In:** `SendModEvent("RumourEngine_AddRumour", requestJson)` takes the same
  JSON as `AddRumour`. No id comes back, so give a `key` to refer to the
  rumour later. `SendModEvent("RumourEngine_RetireRumour",
  "{\"source\":\"…\",\"key\":\"…\"}")` retires one by key.
- **Out:** `RumourEngine_Ready` is sent when a save's database is open, as
  the `ready` event is.

## Testing by hand

- **Debug inbox.** RumourEngine reads any `*.json` in a `RumourEngine_Inbox`
  folder next to `RumourEngine.log`, every few seconds, then renames it
  `.done` or `.failed`. A file holds one AddRumour request or an array of
  them. `{"retire": 12}` or `{"retire": {"source": "…", "key": "…"}}` retires
  one. RumourEngine never creates the folder, so the inbox is off until you
  make it.
- **Console.** With a console mod that adds `cgf`:
  `cgf "RumourEngine.AddRumourInMill" "Console" "Heimskr has been preaching all night" "whiterun.commons" 0.6 ""`.
- **The log.** `RumourEngine.log` records each rumour added, each hop, each
  retelling ("Rumour #12 as riverwood tell it: …") and each airing. The last
  five sessions are kept as `RumourEngine.1.log` to `.5.log`.

## For FactionEngine

The plan from ADR-001, as a soft dependency:

1. `Bind()` at `kDataLoaded`. If it fails, keep FactionEngine's own simple
   model.
2. Register a listener for `ready` (and `retired`, if FactionEngine wants to
   know when its news stops going round). Hold its events until `ready` comes.
3. For each event worth talking about, call `AddRumour` with `source`
   "FactionEngine" and `key` set to the event's id. Write `text` as someone
   who saw it would tell it, and give the place it happened as the `origin`:
   a location for something one corner of town saw, a settlement for
   something the whole town would know at once, like an army marching out.
   Don't send per-hold or `*` versions for ordinary events: the retellings on
   the way are the point. Use `delay_hours` when the town shouldn't know
   immediately, `subjects` for the people involved, and `heard_by` for those
   who were there.
4. **When a faction learns of it**, add its mill with `reach` and give the
   faction's own account as that mill's version, marked `as_fact` when the
   faction knows it for certain: `"mill:legion": { "text": "…", "as_fact":
   true }`. Leave it a plain string when the faction has only heard it as
   rumour. For news that starts inside a faction, make its mill the origin,
   with `secrecy` for how readily it gets out. Several factions can share a
   secret by listing several mills. The table under [Mills](#mills) maps
   FactionEngine's factions to mill ids.
5. **Leaders** that FactionEngine briefs itself go in `known_better_by`, by
   name, so the talk version doesn't repeat what they already have.
6. Calling again with the same key updates a rumour as the event develops.
   Send `patch: true` with just what changed: a `reach` mill and its version
   when another faction learns of it, or the new `known_better_by` list. Send
   `if_absent: true` with `happened` when resending recent events at `ready`,
   so anything lost to a reload comes back where it would have got to.
   `GetRumourByKey` shows how far an event has spread.
   `RetireRumourByKey` takes it back when it's no longer true.
7. Don't put the rumours in prompts yourself. RumourEngine does that for
   everyone whose mill has them.
8. **Ship `SKSE/Plugins/RumourEngine/FactionEngine_mills.ini`.** It holds
   the Silver-Blood and Black-Briar family mills, which exist only for
   FactionEngine, and a `[Retell]` list making every mill a leader is in
   eager. Leaders' bios carry their mills' talk, and lazily a turn would find
   a rumour's telling only on the turn after. A faction a player adds with its
   own `rumour_mill:` needs its own `[Retell]` line.
