# ADR-001: Rumours spread between mills, not between people

**Status:** proposed, 2026-10-30. Revised the same day, after the first review
and again with the stage 0 results ([stage0-results.md](stage0-results.md)).
Only the stage 0 offline tools exist so far.
**Related:** FactionEngine ADR-002 (stage 9, gossip), NarrativeEngine Phase 13 (gossip propagation)

## Context

NarrativeEngine does two unrelated things: it sends NPCs to find the player and
ask them something, and it spreads rumours by writing them into NPCs' memories.
The first was tried and was more annoying than useful. The second is the idea
worth keeping, but its storage model scales badly. Every carrier gets a real
SkyrimNet memory with a vector embedding. NarrativeEngine's own measurements
come to about 32 memories per rumour and about 480 a month at its recommended
pace, and it notes that past 2 rumours a game day the memory store "becomes
noise".

FactionEngine spreads news differently. Each NPC's prompt carries a short list
of the few latest items people in their hold would know. Items retire when they
stop being news. Nothing is written to anyone's memory. It works in play.

RumourEngine combines the two approaches:

- **NarrativeEngine's source of rumours:** the memories of people who actually
  saw something.
- **FactionEngine's way of storing and showing them:** a rumour is held once
  per group, shown in prompts while it is fresh, and forgotten afterwards.

## Goals

- Things that happen around the player turn into talk, and that talk **travels
  ahead of the player**. You do something in Whiterun, and a week later Riften
  has a worn, garbled version of it waiting for you.
- Gossips raise the talk unprompted when the player is near them and someone is
  there to tell. Others who have heard it join in. Not everyone in a group has:
  gossips hear first, and people who keep to themselves may never hear the
  smaller talk.
- Storage grows with rumours × groups reached, not rumours × people.
- Other mods can add rumours through a C++ API, a Papyrus API and a ModEvent.
  FactionEngine is the first intended client, as a soft dependency.

## Non-goals

- NPCs travelling to find the player. Rumours only reach the player where the
  player already is.
- Writing a memory into each carrier. SkyrimNet still writes memories where the
  player witnesses a telling. That is where continuity matters, and it costs us
  nothing.
- Simulating each person. The simulation runs on groups.
- An ESP. See [Packaging](#packaging).

## Overview

```text
 SkyrimNet memories ──harvest──► screen (Decisions model) ──► seed (writer model)
         ▲                                                          │
         │                                                          ▼
   witnessed tellings                                   origin mill: first telling
         │                                                          │
         │                                          hop simulation (numbers only)
         │                                                          ▼
 GossipStarter ◄── bio block / GameMaster line ◄── retell (cheap model, when needed)
                                                                    │
                                                    stale ──► retired, or promoted
                                                              to world knowledge
 Other mods ──API──► seed directly (no screening)
```

## Decision

### Mills: the groups rumours live in

A **mill** is a group of people who talk to each other often enough that news
is shared within a day or so. A rumour is either in a mill or not, and every
member who has heard it knows that mill's version. Which members have heard it
is worked out per person without storing anything (see
[Who has heard it](#who-has-heard-it)).

Mills are finer than holds, because people mostly keep to their own town and
their own class. They are far coarser than individuals. On vanilla, the rules
below give **about 51 mills** (58 with the user's 685-plugin profile), built by the strategy
[stage 0](#stage-0--offline-prototypes) calls *merged at floor 4*. With fewer,
villages lose their own talk and news crosses holds too easily. Variety between
*people* comes from [who has heard it](#who-has-heard-it), not from the mill
count. Measured: in a 53-mill run, two people in one mill share 57% of what
they've heard, against 99% without the weighting.

**Membership is derived from game data**, so NPCs added by mods are included
without any patch. Most of it comes from the loaded forms: locations, NPCs,
templates, factions and live enable state. The rest isn't in memory at
`kDataLoaded`, because temporary references aren't loaded until their cell is.
On vanilla, 496 of 706 generic placements and 390 named NPCs' only placements
are temporary. So a background thread reads every active plugin once at
`kDataLoaded`, a second or so for 680 plugins, and keeps three things:
- placed actors (ACHR);
- cells' locations and grid positions;
- the editor IDs of NPCs, locations and factions, which the game discards.

Mills are first built when that finishes, then again after every save load,
when the save's enable state decides who exists.

1. **Place.** Walk the NPC's editor location (falling back to their persistent
   location, then their cell's) up `BGSLocation::parentLoc`. Classify each
   ancestor by keyword into household, settlement and hold, as NarrativeEngine's
   `GossipGraph` does. Inns, shops, guild halls, barracks, farms, mines and
   camps count as households. Settlement keywords are `LocTypeHabitation`,
   `LocTypeCity`, `LocTypeTown`, `LocTypeSettlement` and
   `LocTypeOrcStronghold`. Hold keywords are `LocTypeHold`, `LocTypeHoldMajor`
   and `LocTypeHoldMinor`.

   **A location's hold is its outermost ancestor with a hold keyword,** not
   the nearest one. Keyword distributors can copy a hold's keyword down to
   everything inside it. In the user's game, Object Categorization
   Framework's KID rule gives `LocTypeHoldMajor` to every location in
   Eastmarch and Haafingar. Taking the nearest made each house there its own
   hold. Holds don't nest, so the outermost is always right.

   Named NPCs are placed from the locations' unique-NPC rows, falling back to
   where they stand in the world. Where someone stands is the reference's own
   location, else its cell's, else the location of the exterior cell at its
   position, else its worldspace's: city worldspaces name their city, which
   is how Brynjolf, a persistent reference in Riften's world, is found. A generic NPC (a guard, a farmhand) counts
   once per placed reference. **Only what actually exists counts:** the vanilla
   masters hold both the Imperial and the Stormcloak guard sets enabled, and
   enable parents decide which appear. So references that are disabled, or
   whose enable parent is, are skipped, using the live enable state at load.
   Hostile places are skipped too, along with quest copies and corpses,
   which are told apart by editor ID patterns (`*Dead`, `Corpse`, `CW*`,
   `MQ*`, …). **Player homes are not skipped:** whoever lives in one belongs
   to the city or village around it, as a dismissed follower would. Stage 0
   skipped them. On vanilla that only dropped the five housecarls, but
   patches mark houses the player can later own: USSEP's Severin Manor, and
   OCF's KID marks the Arch-Mage's Quarters. So the rule was also removing the
   Severins, Savos Aren and Quaranir.

   **Enclaves.** A non-hostile place outside any settlement, with 4 or more
   named residents, is a mill of its own. The Thalmor Embassy, High Hrothgar,
   Fort Dawnguard and Stonewater are enclaves.
2. **Class.** Cities split by social stratum, using job and guard factions:
   - **court:** the Jarl, steward, housecarl, court wizard and household staff.
   - **garrison:** guards and soldiers stationed in the town.
   - **commons:** everyone else who lives in the town.
   - **underworld:** fences, the Ragged Flagon and similar, where they exist.

   Villages and small towns are one mill each. Farms, mills and mines outside a
   town join the nearest settlement's commons. Guild members who carry guard
   factions, such as the Companions, are commons, not garrison.
3. **Institutions.** Guilds and similar bodies can be mills of their own that
   span settlements. Members also keep their place-based mill. Two rules keep
   this honest:
   - **An institution that is really one place's mill is dropped.** If 80% or
     more of its members are in one place mill, it adds nothing but a
     duplicate. On vanilla this drops every candidate: the Companions all live
     in Whiterun's commons, the College in Winterhold College, and so on. The
     last to go was the East Empire Company, 10 of whose 12 are in Solitude's
     commons once leveled templates are followed (stage 1). The rule stays for
     guilds that mods spread across towns.
   - **Travelling trades are never mills.** Inn bards, carriage drivers,
     couriers and caravans are travel links (below). As a mill, the bards
     carried a rumour to every inn in Skyrim within a day.
4. **Merging.** A mill with fewer than 4 members folds into the nearest bigger
   mill in its hold. A lone farm joins its village, Old Hroldan joins Dushnikh
   Yal, and a city's thin strata join its commons. A place with no position on
   the map folds into its hold's biggest mill; on vanilla that is only the
   Dark Brotherhood's two initiates at Dawnstar. The floor is a setting
   (`mill_floor`).
5. **Overrides.** `SKSE/Plugins/RumourEngine/mills.ini` can add, remove or move
   NPCs, merge mills and name new ones. NarrativeEngine's measurements apply
   here: faction size does not tell a social group from a label.
   `JobInnkeeperFaction` has 29 members, none of whom have met. Never admit a
   faction as a mill automatically; mills come from the rules above and from
   the ini.
   *Built 2026-11-01:* `[Factions]` (`id = Name: FactionEditorID, …`) makes an
   institution of a faction's named members, never folded away. Named members
   with no place of their own are in it alone, and a listed faction's members
   count as people even if it is hostile (Volkihar). Within an actor's mills,
   an institution's telling comes before their place mill's. It ships mills
   for FactionEngine's factions that had none: the Legion, the Stormcloaks,
   the Thalmor, the Companions, the Dark Brotherhood, the Silver-Bloods, the
   Black-Briars and Volkihar. The Thieves Guild, the College and the Dawnguard
   already had one (`riften.underworld`, `winterholdcollege`, `hunterhq`).
   **Secrecy** (0–1, from AddRumour) is the simplified form FactionEngine
   asked for. A mill passes the rumour on only if its readiest gossip rates
   above the secrecy, and a gossip brings it up unprompted only if they do.
   Stage 5's per-person reach can refine it.
   *Patches, built 2026-11-07:* another mod changes the mills by shipping
   `SKSE/Plugins/RumourEngine/<Name>_mills.ini`, as SPID reads
   `*_DISTR.ini`. `mills.ini` is read first, then the patches in file-name
   order. A later line replaces an earlier one with the same key (`[Links]`:
   the same two mills), and an empty value takes it away. The Silver-Blood and
   Black-Briar family mills moved to FactionEngine's patch, since they exist
   only because FactionEngine has those families as factions. The armies, the
   Thalmor, the Companions, the Brotherhood and Volkihar stay in `mills.ini`:
   they shape the spread without FactionEngine too.
   *Also from FactionEngine's requests, built the same day:*
   - a `"*"` version for mills no `mill:` or `hold:` key covers;
   - actors by plain name, when exactly one person has it;
   - `patch` updates, which merge versions and leave other fields alone;
   - updates now keep retellings while `text` is unchanged;
   - `if_absent`;
   - `happened` backdating, which replays the missed hops with the regular
     rolls (`Spread::CatchUp`);
   - `GetRumourByKey`;
   - talk about the listener, then their faction's, ranks first under the cap.
   Reviving a retired key now also clears where it had spread.
   *Second round, built the same day:*
   - **The intended use (the user's):** a caller sends what someone who was
     there would say, at the place they'd say it, and every hop drifts it.
     Pinned `versions` are for wording that mustn't drift, and for a
     faction's own account.
   - **`as_fact` versions** are shown in their own bio section ("What X knows
     as one of the Legion"), not under "Local talk". They get out only as
     retellings, which are talk.
   - **`reach`** adds first-hand mills without changing the stored origin.
     Missed hops are replayed on an update only when it added mills.
   - **`known_better_by`** keeps a rumour out of those people's talk, fact
     section and gossip.
   - **`retell_origin`** (default off) retells `text` at hop 0 too, so `text`
     can be a plain account or data. The retell prompt gets `retellFirst`.
   - **Airings** no longer make a rumour stale. After three tellings in the
     player's hearing, gossips only stop raising it unprompted (the user's
     call).
   - **Faction mills are timed** by their nearest member's town, for
     proclaimed rumours and overheard carry-home.
   - Schema 7 adds `rumours.known_better_by` and `rumours.retell_origin`.

   *Third round, built the same day:* **`origin: { "settlement": … }`** gives
   every mill of one town at once, by name or a location form in it. It
   doesn't take in the hold or the faction mills. For news a whole town would
   know as it happens, such as the Legion marching out of Solitude (the
   user's example), where a location gives one stratum and a hold too much.
   **Hops out of a faction mill with no parent** (told by `reach` or as the
   origin) had no place, so each counted as a 2-day trip at travel decay,
   even into a member's own town. Now a shared member passes it on where they
   live: 0.1 days, local decay. `tools/spreadsim.py` ports `Spread::Simulate`
   for one event and replays this save's recorded hops exactly.

   *The spread curve, retuned the same day.* The user's goal: big news, such
   as the Legion marching a company on someone, should cross most of Skyrim
   and have a fair but unreliable chance of reaching the target before the
   troops (FactionEngine settles a raid two turn rounds later, about 5 to 7.5
   days). Stage 0's curve couldn't do it. The hop rate went with n², so even
   n = 1 only got the base rate, and a Solitude–Whiterun carriage hop averaged
   a week. Simulated from Solitude at n = 0.9, Falkreath heard by day 122.4 in
   8% of runs and the Brotherhood never did. Now hops to another place go with
   n⁴ at 8× the base rates (`spread.travel_exponent`, `spread.travel_rate_scale`),
   travel decay eases to 0.6 + 0.3n (`spread.decay_travel_per_notability`),
   and the mill cap is 80. The cap was a runaway guard from stage 0, and it
   bound once big news could travel. At n = 0.9 Falkreath now hears before
   the troops land in 55% of runs (85% by day 122.4), the Brotherhood in 16%
   (40%), and the news reaches about 53 of 66 mills. Ordinary talk from a city
   after 8 days, then and now:

   | n | before: mills, left its hold | after |
   | --- | --- | --- |
   | 0.3 | 3.8, 24% | 3.6, 18% |
   | 0.5 | 8.4, 73% | 11.8, 87% |
   | 0.7 | 13, 94% | 33, 100% |

   The Brotherhood's odds were limited by its links too: as built, only
   Gabriella (Falkreath) and two Dawnstar initiates tie its mill to the world.
   Placements now feed the spread graph ([Observed homes](#observed-homes)).
   Once Arnbjorn, Veezara and Nazir have settled in Falkreath, the Brotherhood
   hears the march before the troops land in 35% of runs at n = 0.9, and 14%
   at 0.8 (66% and 35% by day 122.4). Values will be tuned further in the
   simulator, with FactionEngine's notabilities.

An NPC can belong to several mills. **Those shared members are what links
mills.** Vilkas belongs to Whiterun's commons and to the Companions, so news
reaches the Companions through him with no authoring.

- **Mill ids** are stable strings, such as `whiterun.commons`,
  `whiterun.court`, `riverwood` and `companions`. They are not form IDs, so they
  survive load-order changes.
- **Membership is rebuilt every session.** Changing a definition takes effect
  mid-playthrough, and a mill that disappears drops its rumour state. The one
  exception is a stored placement, for a dismissed follower or someone a mod
  or quest moved, which is kept in the save's database.
- **Player's party:** current members belong to no mill while following,
  because they travel with the player. A dismissed follower does not simply go
  back to the mill derived at load time. Follower mods often send them to live
  somewhere else, so they join the mills of wherever they settle. See
  [Dismissed followers](#dismissed-followers).
- **Who doesn't take part:** hostile NPCs (bandits, Forsworn, necromancers) and
  NPCs with no resolvable home, until they are seen settled somewhere (see
  [Observed homes](#observed-homes)).

### The mill graph and hops

Rumours travel between mills by **simulation, not by overhearing**. As far as
we know, SkyrimNet records events, and forms memories, only for actors loaded
around the player ([open question 5](#open-questions)).
So overhearing only happens where the player is, and members of other mills are
rarely there. Left to that path alone, rumours would trail along behind the
player instead of travelling ahead.

The simulation runs on groups, as susceptible → spreading → done. While a
rumour is fresh in a mill, that mill passes it along each outgoing edge. When a
hop succeeds, an arrival is scheduled at the far end after the edge's travel
delay. Per simulation tick Δt:

```text
P(hop g→h) = 1 − exp(−rate(edge) × w(g,h) × n(r,g)² × interest(h,r) × Δt)
n(r,h)     = n(r,g) × 0.9      within a place (between a town's strata)
           = n(r,g) × 0.6      between places (any hop to another settlement)
fresh(r,h) = 2 + 6 × n(r,h)    days h keeps spreading it and talking about it
```

These are the values stage 0 tuned. A rumour whose notability falls below 0.15
stops spreading.

- **Squaring n** keeps minor talk local and still lets notable news travel.
  With a rate linear in n, 63% of all rumours reached four or more holds.
- **Two decays** let a rumour cross a town's court, garrison and commons
  without using up its reach.
- **A hop to another settlement always costs travel time and the travel
  decay,** even when it goes through a shared member such as a Companion.
  Shared members don't teleport news between towns.

The resulting shape, simulated over 60 game days on the 53 vanilla mills:
- 76% of minor talk stays in its hold.
- About half of moderate talk reaches a third hold.
- 9 in 10 major rumours reach another hold, and 39% reach six or more.
- The next hold is reached in about 2–3 days.

**Edges** come from three sources:

| Source | How the edge is made | Rate per day | Delay |
| ------ | -------------------- | ------------ | ----- |
| Shared members | Weight is the sum of the gossip scores of shared members who [have heard it](#who-has-heard-it) | 1.0 | same day within a place; travel time to another |
| Strata within one town | Commons↔garrison strong, commons↔court weaker, garrison↔court medium | 3.0 | hours |
| Roads | Each settlement to its 3 nearest | 0.6 | 0.25 + distance / 40 cells, in days |
| Travel links | Carriages between the towns drivers live in, the Solstheim ship, and `mills.ini` [Links] | carriage 0.1, ship 0.3 | by distance; ship 2 days |

Each mill also has a weak edge (0.3) to its hold's largest settlement, so no
mill is stranded. Roads are derived from where settlements sit on the map.
Hops to another place go faster for bigger news (see the spread curve, under
Overrides). `mills.ini` [Links] (`a = b: kind, weight`, built 2026-11-01)
adds a road, carriage, ship or hold link the map doesn't give, changes one it
gives, or removes one with `none`. It replaces whatever the builder made
between the two, and can't link mills already linked by shared members.
`tools/spreadsim.py --link a=b:kind` tries one without loading the game.
Caravans and travelling bards aren't authored yet.

**Ears and close mills** (built 2026-11-01, for the Dark Brotherhood: it
should hear what goes on around Skyrim while Falkreath stays insular).
A `[Links]` line of kind `ears` is one-way: `darkbrotherhood =
whiterun.commons: ears, 0.25` lets the Brotherhood hear what is said in
Whiterun, as an informant would, at the shared-member rate times the weight.
It takes the travel time from Whiterun to the Brotherhood's nearest member's
town, and nothing goes back along it. `[Secrecy]` (`darkbrotherhood = 0.8`)
makes a mill keep what it hears. It passes a rumour on only if someone in it
gossips more readily than the higher of that and the rumour's own secrecy,
and its members don't raise those rumours unprompted. In the simulator, with
a Whiterun ear at 0.25 and secrecy 0.8, the Brotherhood hears the Legion's
march before the troops land in 25% of runs at n = 0.9 (16% without it). Falkreath
and Dawnstar hear it exactly as often as without the ear, because nothing
leaks back out. A rumour starting in the Brotherhood stays there. The
simulator has `--link a=b:ears:w` and `--mill-secrecy id=v`, and the mills
dump records each mill's secrecy.

**Interest** is optional and has no LLM cost. The seed writer tags each rumour
with topics (violence, romance, money, politics, magic, the player, …), and
each kind of mill weights them. A court cares about politics, a garrison about
crime and fights, the commons about scandal. Stage 0 ran with it off, since it
can't be judged without real topic tags from the seed writer. Stage 4 decides
whether it earns its place.

**Overhearing still counts, as a bonus path, in two ways.**

- When the GossipStarter airs a rumour, RumourEngine knows who was in
  earshot. Any listener whose mill does not have the rumour carries it home: an
  arrival is scheduled directly, with no harvest and no LLM call.
- When NPCs talk about a rumour unprompted, SkyrimNet writes ordinary,
  untagged memories of it. The harvest sees those memories. The screening call
  matches them to the live rumour (below), and they count as that rumour
  reaching another mill, not as a new rumour. This also prevents gossip from
  feeding on its own output.

**Bounds:** at most `max_mills_per_rumour` mills per rumour, at most
`max_live_per_mill` live rumours in a mill (the stalest is dropped first), and
a global cap on live rumours.

### Who has heard it

Not everyone in a mill knows everything going round it. Who has heard a
rumour is **computed when needed, not tracked per person**. Each NPC gets a
fixed roll per rumour, and the share of the mill a rumour has reached grows
with time, faster and further for gossips:

```text
heard(a, r, g) = u(a, r) < reach(a, r, g)
u(a, r)        = hash(a's UUID, r's id) mapped to [0, 1)     the same roll every time it is asked
reach(a, r, g) = ceiling × (1 − exp(−rate × t))
ceiling        = min(1, n(r,g) + gossip(a))                 notable talk reaches everyone; trivia only the gossips
rate           = reach_base_rate + reach_gossip_rate × gossip(a)
t              = days since r arrived in g, frozen once r goes stale there
```

Stage 0 tuned `reach_base_rate` to 0.5 and `reach_gossip_rate` to 2.0.
- **Minor talk** reaches 38% of the people who rarely gossip, and 83% of the
  gossips.
- **Notable talk** reaches 86–92% of everyone.
- After half a day, about a third of the gossips have heard it, and 15% of the
  rest.

What the formula gives:

- **No storage.** The answer is recomputed each time and is the same in every
  prompt and after a reload.
- **It only grows.** Someone who has heard a rumour keeps having heard it.
- **Gossips hear first.** Notable rumours reach nearly everyone within a day or
  two. Trivia stays among the gossips. Someone who keeps to themselves hears
  only the notable talk, and late.
- **Some people have certainly heard it,** whatever their roll: the person
  whose memory seeded it, and members who were in earshot of an airing. These
  few are kept in a small `heard` table.
- **Several mills.** An NPC in several mills has heard a rumour if any of
  those mills' rolls says so, and knows the version of the mill where they'd
  have heard it first.

It applies everywhere a rumour meets a person:

- the bio block shows only what that NPC has heard;
- the GossipStarter picks a teller who has heard it, and prefers a listener
  who hasn't;
- shared-member edges between mills count only members who have heard it;
- screening's `same` choices are the rumours that NPC has heard or was in
  earshot of;
- the API's per-actor queries answer for that actor.

As built (stage 5, 2026-11-12):

- **The roll's key** is a named NPC's editor ID, which outlives a load-order
  change, and anyone else's form ID.
- **`heard` is checked by form,** kept beside the UUID keys, because a prompt
  decorator shouldn't call back into SkyrimNet.
- **Everyone has heard it at once** where the mill's version is `as_fact`
  (their own people's account), where the rumour is `proclaimed`, and in a
  mill marked `[Retell] eager`. The last is for FactionEngine: an eager mill
  is one whose talk another mod reads out, and a leader who learned of a march
  a day late would undo what eager retelling was for.
- **An actor who isn't one of the mills' people** is taken to know what their
  mills do, as before.
- **Not shown, not asked for.** A lazy mill's version is only requested for
  someone the rumour has reached, which saves the calls for those it hasn't.
- **Shared-member edges** sum the scores of the shared members it has reached
  in the mill it is leaving. Over 200 runs of the Legion's march
  (`tools/spreadsim.py`), the travel hops are unchanged and faction mills
  reached through members come a little later (the Companions 14% to 10%).
- **`reach.enabled`** off gives the old behaviour.

It also lowers sameness. Two people from the same mill no longer know the same
things, so fewer mills give the same variety, and the mill count can lean lower
still.

### Harvest and screening

Every `harvest_interval_hours` of game time:

1. **Find who has something new.** SkyrimNet has no global memory query, so
   one `PublicGetActorEngagement` call finds actors with important memories in
   the recent window. Because memories only form near the player, this set is
   small.
2. **Fetch only what is new.** `PublicQueryMemoriesForActor` runs with filters
   applied in SQL:
   - `minImportance` set to that NPC's threshold;
   - `gameTimeAfter` set to their per-actor watermark;
   - ordered by importance.

   Each memory is screened once. The watermark and a small screened-ids table
   guarantee it.
   - **Don't filter by memory type.** NarrativeEngine measured KNOWLEDGE as the
     largest and highest-importance type on a real save.
   - **Read a real row before writing the parser.** NarrativeEngine found
     several fields of the memory JSON named differently from what
     `PublicAPI.h` documents.
3. **Exclude some sources.** The player's own memories are never harvested. The
   player's deeds still reach the mill through witnesses' memories, and they
   are fair game: hearing something you did, retold half a dozen hops later, is
   a feature. The current party's memories wait until the follower has been
   dismissed and has settled somewhere ([below](#dismissed-followers)).
4. **Screen with one Decisions request per NPC.** Template:
   `prompts/decisions/rumourengine/screen_memories.prompt`. SkyrimNet decision
   templates allow option keys built in a loop and several questions per
   request. Its own `memory_recall_gate.prompt` does both. For each candidate
   memory `m<k>`:
   - `spread_m<k>` (noul): would this person pass it on? The state block
     carries their profile, their gossip score in words, and their mill.
   - `interest_m<k>` (score, 1–5): would a room go quiet for it?
   - `same_m<k>` (choice): which live rumour this is about, with keys `r1…rn`
     for the rumours they have heard or were in earshot of. It may
     also name an earlier memory in the same batch (`m<j>`), or `new`. Several
     witnesses of one event produce several memories, and this question folds
     them together.

   **Accept** when `p(spread) ≥ threshold(gossip score)`, interest is at or
   above `min_interest`, and the answer to `same` is `new`.
   - A **gossip's threshold is lower**, as is their importance floor in step 2.
   - **A match to a live rumour reinforces it** by refreshing its freshness in
     that mill. If the rumour was not yet in the NPC's mill, it counts as an
     arrival.
   - **No decision provider:** if SkyrimNet returns `no_decisions_route`,
     screening falls back to a chat prompt with the same questions.

#### Dismissed followers

A follower's memories are the best record of the player's adventures, and a
companion back from the road is exactly who would tell everyone about them
("Lydia's been telling half of Solitude about that barrow"). They are harvested
once the follower has been dismissed and has settled.

1. **Notice the dismissal.** The harvest tick checks party membership: the
   teammate flag, `CurrentFollowerFaction`, and the follower mods' own
   factions that `mills.ini` `[Party]` names. Someone seen following stays in
   the party while they're within half a cell of the player and not in
   `DismissedFollowerFaction`: Sofia clears her teammate flag while she idles
   near a player who has stopped, and keeps her own faction rather than
   `CurrentFollowerFaction`, so until 2026-11-26 she counted as a Whiterun
   local whenever the player stood still. Everyone loaded is looked at every
   4 s, so a follower is seen following while the game still marks them: in
   the first test, Melana's flag was cleared before anything had asked about
   her, and the grace had nothing to go on. Factions a quest alias gives count
   (Melana's PlayerFollowerFaction, from her follower alias, matched a test
   `[Party]` line). The time each follower joined and left is recorded.
2. **Give them time to get home.** Vanilla followers walk back to a fixed
   home, but follower mods commonly send them elsewhere. A follower counts as
   settled once they have been in the same settlement for `settle_hours` of
   game time. After `settle_max_days`, wherever they are counts. If they are
   recruited again first, nothing is harvested yet, and their memories wait
   for the next dismissal.
3. **Join the mills where they live now.** The place and class rules from
   [Mills](#mills-the-groups-rumours-live-in) are applied to where they
   settled. If Lydia is told to live in Solitude, Solitude hears her stories,
   not Whiterun. This placement is stored in the save's database and replaces
   the load-time one until they are recruited again. Institutions such as the
   Companions are unaffected.
4. **Harvest the time away.** Their memories from joining to leaving go
   through the usual screening, with the state block saying they have just
   come back from travelling with the player. Only the `follower_harvest_max`
   most important are screened, so a long stint doesn't flood the mill.
   Rumours seeded this way start in their new mills.

This depends on an unloaded NPC's location being updated as they travel
([open question 3](#open-questions)).

#### Observed homes

The builder places people where the game data says they live, and mods and
quests move them. Sanguine's Trade parks Grimr in its holding cell, which has
no location, then moves him into Breezehome. Follower and marriage mods send
people to live in player homes. In the user's 685-plugin load order, 165 named
NPCs have no location at all, most of them in a mod's holding cell. So the
settling rule for dismissed followers applies to everyone:

1. **Who is watched.** Named NPCs with a persistent reference who aren't in
   the player's party. A reference a quest or mod moves is almost always
   persistent, since filling a quest alias makes it so. Watching only those
   covers the moved, without touching the thousands of temporary references.
2. **Where they are.** At each harvest tick, their current location is put
   through the place and class rules. If the answer differs from their home
   (the built one, or a stored placement), the time is noted and saved
   (`pending_moves`). A session is often shorter than `settle_hours`, so a
   move under way has to outlast a reload. Until 2026-11-03 it didn't, and in
   play nobody ever settled. Its own table means a reload to before the move
   began drops only the move, never the placement it would replace.
3. **When it counts.** If they are still in that settlement `settle_hours`
   later, with no tick finding them elsewhere in between, the new placement is
   stored and replaces their home. Someone whose routine takes them to the
   next village each day never stays long enough to move. Going back to the
   built home clears the stored placement.

   **Out of sight** (2026-11-05). Outdoors and not loaded, a person has no
   location: the game parks them in the worldspace's persistent cell. Read as
   "on the road", that cancelled every outdoor move as soon as the player left
   the town. Celag Oranius (Riverwood), and Vesyra, Ungrien and Thorbur around
   Haafingar, lost their moves that way, and only Camilla, indoors at the
   Skeever, ever settled. Their worldspace and position are still kept, and
   travel packages (SeverActions' work and evening spots among them) update
   them. So the scan finds the exterior cell they stand in, using the plugin
   scan's grid, and takes its location, or the worldspace's own for a city,
   which is what `GetCurrentLocation()` would say if they were loaded.

   A first try held the move while they stood within 2048 units of where they
   were last seen in the town. It cancelled moves whenever an unloaded routine
   took someone from the inn to their stall, and it was dropped.

   **Known limitation: an interior with no location.** A mod interior without
   an XLCN is nowhere, loaded or not, so a move under way is called off each
   time the person's routine takes them inside. Reinhardt, from Helgen Reborn,
   lost his Falkreath move twice on 2026-11-05 to `aaaBalokReinhardtInterior`
   (Reinhardt's House, 20060154). The fix, if it becomes a real problem: give
   such a cell the location its load door leads out to. That needs the
   plugin scan to read doors' XTEL. Left for now.

4. **Nothing else changes.** Their memories are harvested as anyone's are.
   Only a dismissed follower gets the catch-up harvest of time away.
5. **The spread graph follows them** (added 2026-11-01). A placement moves
   them between place mills in the hop simulation too, so their guilds and
   factions link to the town they live in now. The graph is rebuilt whenever
   a placement changes. The Dark Brotherhood is the case that needed it:
   built, its mill reaches the world only through Gabriella in Falkreath, but
   the watcher finds Arnbjorn, Veezara and Nazir living there as well.

Grimr joins Whiterun's commons once he has spent `settle_hours` in Breezehome.
So does the user's Lydia in Solitude, by the same rule, whether or not
SeverActions says where she lives. The tick looks up a few hundred to a few
thousand persistent actors. If that is noticeable on the main thread, it is
spread across ticks.

### Quest endings

Some of the player's deeds are news nobody remembers in the first person:
Dawnstar freed of its nightmares, the Butcher of Windhelm caught, the Emperor
assassinated. SkyrimNet records no quest events, and the townsfolk who would
talk of these weren't there, so the harvest never sees them. `Quests.cpp`
makes them rumours. *Built 2026-11-21, the user's idea before 1.0.*

- **Seeing a quest end.** A `TESQuestStageEvent` sink watches each quest
  for 30 s after a stage is set, and the worker looks on the main thread
  whether it has completed. A script may complete it a little after the
  stage that leads there. Passed over are quests without a journal, and,
  unless `quests.ini` names them, miscellaneous quests, unnamed ones and
  those with neither a type nor objectives (QOBJ in the plugin). A missing
  type alone says nothing: The Man Who Cried Wolf, the three temples' quests,
  Forbidden Legend and half a dozen more of the game's own have none, and
  until 2026-11-22 they were all skipped. Its monitors and handlers have no
  type and no objectives. Each quest passed over, for whatever reason, is
  logged once a session, and the inbox's `quest_preview` says when a quest
  would be. Each quest's ending is handled once per save (`quest_endings`,
  schema 12). On a save's first load with this, every quest it had already
  finished is recorded as `before` and never told. A `before` row holds
  only while the loaded save has the quest finished: loading the latest
  save first, then an earlier one with SkyrimNet's history kept, once
  left The Man Who Cried Wolf on record as `before` and its ending untold
  (2026-11-22). An ending's own row replaces a `before` row whole, its day
  and stage too, so a reload's take-back goes by when it ended in this save.
- **The journal.** The game reads journal entries from the plugins only to
  show them, and CommonLib has no reader. The plugin scan keeps each quest's
  stages and entries from the last plugin to change it (a string id for a
  localized plugin, the text for one that isn't). The text comes from
  `Strings/<plugin>_<language>.DLSTRINGS` through the game's file system, so
  archives and loose files both work. Which ending the player got, when a stage
  has several (Waking Nightmare's three on stage 200, the civil war's two
  sides), is `BGSQuestInstanceText::journalStage/journalStageItem`, the entry
  the journal shows. Its `stringData` names each text alias, so
  `<Alias=Fort>` and `<Alias.PronounPosObj=Jarl>` are filled as the journal
  fills them. Entries mostly restate the earlier ones, so the state is the
  first entry of a stage done (one with a single text) and the ending, or
  the ending alone when it restates the first. "Done" is the game's own
  `GetStageDone` condition, built as a `TESConditionItem` and run on the
  player, since it holds after the quest has stopped; `executedStages` held
  two blank entries for a finished Waking Nightmare.
- **The decision** (`decisions/rumourengine/quest`): `heard`, will word of
  how it ended get out beyond the people who took part, and `far`, will other
  holds hear of it, both nouls, from the quest's name and type and its
  journal. `heard` >= `quests.heard` (0.5) tells it; `far` sets the
  notability, 0.5 + 0.4 × far (0.5 to 0.9). `tools/quest_panel.py` calibrated
  it on 60 vanilla endings: on Jev 1.14, 0.5 tells all 8 that are news across
  Skyrim and all 20 talked of where they happened, and 2 of 20 private ones;
  the type in the state keeps 5 more private ones out. d3 needs about 0.6;
  Kev-4B separates them poorly. The template's notes give the numbers.
- **The telling** (`rumour_quest` prompt and variant): what the people where
  it ended would first say, from the player's name, race and sex, the quest,
  the place and the journal. It's seeded through `Rumours::Add` as source
  `RumourEngine`, key `quest:<editorid>`, from the player's location (a
  dungeon's talk starts in its hold's capital), about the player, with
  `promote` left on auto so the lasting question decides world knowledge
  later. If the telling fails three times, the journal is seeded instead,
  with `retell_origin`.
- **The party** when it ended goes in `known_better_by`: they were there and
  have their own memories of it, so the talk never comes back to them as
  something they heard, then or after they're dismissed. *Added 2026-11-26:*
  a player's Sofia, travelling with them, told a caged prisoner at Fort
  Greymoor that "people in Whiterun are saying" the player killed Sinding,
  which they had done together.
- **Where it ended.** The player's location, else their cell's. In the
  wilderness, where cells carry none, it's the nearest exterior cell with
  one, up to 8 cells away. Failing that, it's the last location they were
  in this session. Either fallback is told as "near" that place. A mod interior with no location
  and nothing seen yet (a player home's cellar, just after loading) holds
  the ending until the player enters a location, and the talk starts there.
  Quitting before then loses it: the next load's sweep records it as old.
- **`quests.ini`** (and `*_quests.ini` patches): `EditorID` or
  `EditorID:stage` = `always`, `never` or a notability. Without a decision
  provider, only what it says to tell is told, at `quests.notability` (0.7).
  It ships the biggest vanilla endings as always, so they are told without a
  decision model, and `never` for the two endings Jev mistook for news.

### Writing: seeding and retelling

The two LLM jobs use two variants, so each can have its own model and
temperature.

**`rumour_seed`** (chat, through `PublicSendCustomPromptToLLM` with no length
cap) turns the accepted memories from a sweep into rumours, in one call. For
each memory it returns:

- `telling`: the first-hand version, in the third person, as the person who
  remembers it would tell it. Nothing they would keep to themselves.
- `subjects`: keys from a candidate list. The list holds the memory's owner,
  the player, the row's related actors if present, and the people the owner
  shares recent events with (`PublicGetRelatedActors`). Anyone else is recorded
  by name only.
- `names_subjects`: which subjects the telling still names. The code's own
  name check (under [who can be told](#who-can-be-told)) is what counts; this
  is a cross-check.
- `topics`: tags for the interest table.
- `topic_line`: a few words for logs and the dashboard.

**`rumour_retell`** (chat; a cheap model) writes the version a mill hears from
the version before it.

- **Drift, not replacement.** Names, numbers and reasons fade first.
  Exaggeration and muddle are welcome, and each mill's slant shows. A
  different event is not allowed, and neither is repeating the story word for
  word.
- **Mill descriptions.** A short line per mill (who they are, what they care
  about) lets the model slant the telling.
- **Model: `z-ai/glm-5.3-flash` by default,** chosen by the stage 0 panel.
  Twelve models retold six real seeds along seven-mill paths. GLM Flash and
  Gemma 4 31B kept the event while letting details drift, in each group's own
  voice. GLM Flash read more naturally.
  - **Alternatives:** Gemma 4 31B, DeepSeek V4 Flash, and Claude Haiku 5.5
    (the best speech, but the dearest).
  - **Copying is not a risk.** No model copied the step before.
  - **Cost doesn't decide it.** The usable candidates all cost $0.0002 a
    call or less.
- **Numbered lines, not JSON.** The answer is one telling per step,
  numbered to match the step. Cheap models broke JSON about half the time.
  When a model drops the numbers but writes exactly one paragraph per step,
  the paragraphs are taken in order.
- **Reasoning belongs to SkyrimNet's variant config,** not to RumourEngine.
  Recommend none or low for this variant. Some reasoning models spend the
  whole token budget thinking and return nothing, and some endpoints refuse to
  turn reasoning off.
- **Seeds state events plainly.** A euphemism in the seed ("showed me how
  devoted she was") came out as five different events across five models. The
  seed writer says what happened, and the drift is left to the retellings.

**Retelling is lazy.** The hop simulation needs only numbers, so a
mill's version is written when someone first needs it, not when the rumour
arrives. "Needs it" means the player enters a settlement with live rumours that
have no version there yet, or a rumour arrives in a mill the player is in.

- **One call per (rumour, settlement).** It starts from the nearest ancestor
  with a version and writes **every step along the path** to each mill there
  that needs one. Each step comes out as its own telling, so drift accumulates
  one hop at a time, and all of them are stored for other branches to reuse.
- **LLM calls scale with where the player goes, not with how far rumours
  travel.** The cost of having more mills becomes longer chains in one call,
  not more calls. That changes the trade-off on mill count: its remaining costs
  are spreading speed and sameness.
- **Waiting for a version.** A rumour whose version is still being written is
  left out of that NPC's prompt until it arrives. Writing starts when the
  player enters the location, so it is usually ready before anyone speaks.
- **Eager mode.** `retell_mode: eager` writes at arrival instead. It is for
  testing.
- **Eager mills** (2026-11-07). `mills.ini` `[Retell]` (`id = eager`) writes
  one mill's versions at arrival, wherever the player is. It is for a mill
  another mod reads out in prompts of its own: FactionEngine's leaders, whose
  bios carry the talk of their mills. Lazily, a leader's turn asked for a
  version and found it on the next turn. A sweep every few seconds asks for
  any version an eager mill is missing, so a hop, a `reach`, and the catch-up
  after a load all count.

**Prompt caching.** Both writers are called in bursts: one seed call per
sweep, and several retell calls when the player enters a settlement. Prompts
are therefore laid out from the most shared part to the least shared:

```text
rumour_retell:  [ instructions, style rules, output format ]  identical for every call
                [ the destination settlement's mill lines   ]  shared within a burst
                [ the rumour's path and versions so far     ]  per call
rumour_seed:    [ instructions, output format ] [ this sweep's memories ]
```

- **Keep the prefix byte-identical.** Nothing that varies goes in it: no game
  time, ids or weather, and no decorators whose output drifts. Ids go at the
  end.
- **It hardly matters at this size.** In stage 0, providers that cache
  automatically served 38–49% of input tokens from cache with no hints, and a
  call costs about $0.0001 either way. The layout is kept because it costs
  nothing.
- **Batching is built in.** A lazy call already writes every step to one
  settlement in one response, about 1.3 versions per call on average.

### Gossip score

Each NPC has a **gossip score** from 0 to 1, not a yes/no flag.

- **Job defaults** come from `gossips.ini`: innkeepers, bards, barmaids,
  merchants, carriage drivers, beggars and court servants score high; guards,
  court wizards and hermits score low.
- **One Decisions `score` call on the NPC's bio**, the first time they matter,
  with five levels from tight-lipped to incorrigible. It is cached in the
  save's database, and the dashboard and console can re-run it.
- **Overrides** in `gossips.ini` by name or form, such as Haelga or Carlotta,
  win outright.

As built (stage 5, 2026-11-12):

- **`gossips.ini`** has `[Jobs]` (`FactionEditorID = score`, the highest of
  several, `default` for none) and `[People]` (`Name` or NPC editor ID
  `= score`). `<Name>_gossips.ini` patches it as `*_mills.ini` patches the
  mills. Without the file the stage 2 table is used.
- **The bio reading** is `decisions/rumourengine/gossip_score`: the summary,
  personality, occupation, skills and speech blocks, and one `score` question.
  Named people only; everyone else shares a generic bio. It is asked the first
  time `GossipScore` is asked about a loaded actor (their bio rendering, the
  starter looking, the harvest screening them), at most two requests at once.
- **A bio that says nothing reads as about 1 of 4, not 2.** On
  `tools/gossip_panel.py`'s 46 vanilla bios (the local [d3](https://huggingface.co/vllm-sr/d3-mini), `d3:4b`),
  ordinary townsfolk read 0.8 to 1.3, the plainly close-mouthed 0.35 to 0.6
  (Eorlund, Angi, Irileth), and the plainly talkative 2.2 to 3.3 (Mikael,
  Hulda, Cicero, Haelga). So the reading doesn't replace the job score: inside
  0.8 to 1.3 it leaves it alone, and outside it moves the score towards 0 or
  1 by `gossip.bio_weight` (0.7) of the way. Carlotta's bio reads 0.80, so she
  stays a market vendor at 0.75; Irileth's 0.49 takes a housecarl's 0.25 to
  about 0.13; Mikael's 3.28 takes a bard to about 0.96. Another decision
  model may want the band moved (`Gossip.cpp`).
- **It is read again when the bio changes** (the user's, 2026-11-12). A
  fingerprint of those five blocks, from the generated bio and the authored
  one, is stored with the reading. Each time the person matters again, at
  most every ten real minutes, the files are looked at, and a different
  fingerprint asks again. A change to relationships or background alone
  doesn't. Ahlam's authored bio reads 1.61; the generated one that says she
  can keep her own secrets reads 1.12.
- **The earlier reading is not shown to the model.** Tried on the panel: the
  model already gives the same bio the same level to within 0.02, and showing
  an earlier level moved the answer 0.1 to 0.4 even when it was the model's
  own, and dragged it part of the way when it was a level off (Camilla 2.23
  became 1.33 or 2.62). It added drift and nothing else.
- **Not built:** re-running a reading from the dashboard or console. Deleting
  the person's `gossip_scores` row does it.

The combined score:

- lowers their harvest importance floor and their screening threshold;
- tells the screening call, in words, how much of a gossip they are;
- decides how soon they hear what is going round their mills, and how much of
  it ([Who has heard it](#who-has-heard-it));
- weights them as a link between mills;
- decides who the GossipStarter picks to start telling, and how often.

Below `min_teller_score` an NPC only joins in and never starts a telling.

### Delivery

Delivery reuses FactionEngine's proven shape, through decorators registered
with `PublicRegisterDecorator`:

- **`get_rumourengine_talk(actorUUID)`** goes in a `character_bio` submodule.
  It renders a "## Local talk" block: up to `max_rumours_per_prompt` (3) fresh
  rumours from the NPC's mills that the NPC has heard, each in their mill's
  version, framed as talk rather than certainty.
  - **Ranking:** fresher first, then more notable, then rumours about people
    from the NPC's own mills.
  - **A gossip's freshest item** is marked as most on their mind.
  - **A rumour about the NPC themselves,** once they've heard it, is shown as
    "people are saying this about you", but only if that version still names
    them (see [who can be told](#who-can-be-told)).
- **`get_rumourengine_news(actorUUID)`** goes in `interject_inline`. It is the
  short "Also keen to pass on …" line that the GameMaster and the speaker
  selector read, for gossips only, and cut short. The whole item is in the bio.

The **GossipStarter** follows FactionEngine's version:

- A worker thread polls, and only a quiet room gets a main-thread look.
- That look picks a gossip who has heard a fresh item, and someone nearby who
  may be told ([below](#who-can-be-told)), preferring one who hasn't heard it.
  It registers a `direct_narration` with the gossip as speaker.
- **Never** into a conversation, a scene, combat, a menu, or anyone busy.
  An OStim or SexLab scene counts too (`OStimExcitementFaction`,
  `SexLabAnimatingFaction`, as SkyrimNet's speaker selectors read them): no
  one in one starts a telling or is told one, the player included, and their
  bio loses the "most on their mind" mark while it lasts. The talk itself stays.
- **Cooldowns:** once per gossip per item, a minimum gap between starts, and a
  per-item cooldown once it has been aired.
- **Airings are logged** with everyone in earshot, who have all now heard it.
  That log drives the overheard-arrival path and retirement.
- **One starter only.** RumourEngine owns the starter for everyone. When
  FactionEngine uses RumourEngine, it stops running its own, so the two never
  compete for the same quiet room.

#### Who can be told

Being picked as the listener is an interruption, whatever is said. In
FactionEngine, an NPC walking up to a follower with a rumour stopped the party
on the road while the follower talked it over. So the settings decide whether
the player and the party can be **told anything at all**, not what the telling
says:

| Setting | Values | Default |
| ------- | ------ | ------- |
| `tell_player` | `never` / `taverns` / `towns` / `anywhere` | `taverns` |
| `tell_party` | `never` / `taverns` / `towns` / `anywhere` | `taverns` |

- **Taverns:** the player's current location, or one of its ancestors, has
  `LocTypeInn`.
- **Towns:** the player is inside a settlement (the settlement keywords under
  [Mills](#mills-the-groups-rumours-live-in)), indoors or out. That rules out
  roads, the wilds and dungeons.
- **When a target is not allowed,** the starter picks another listener or
  doesn't start. Party members who may not be told are also left out of the
  telling's audience, so SkyrimNet's speaker selection doesn't pick them to
  answer an overheard telling. That SkyrimNet honours this is still to be
  verified.

**"Is it true that…?"** is one form a telling can take, with no setting of its
own. When the listener is one of the rumour's subjects and **that mill's
version still names them**, the gossip asks them about it instead of telling
it. By then it probably isn't true. The same target rules apply, and NPC
subjects can always be asked.

**Most versions stop naming anyone after a hop or two.** In the stage 0
panel, "Rennard" became "some Breton sellsword", then "a foreigner", then "a
hired stranger". A version like that could be about anyone. The gossip has no
way to know, so they tell it like any other rumour, perhaps to the face of the
person it's about. So:

- **Names are checked per version, in code.** When a version is written, each
  subject is tested against its text: the full display name, and the first
  name on its own, as whole words, ignoring case. Names of five letters or
  more also match at one edit away, because retellings misspell them
  ("Rennart", "Renard"). Extra names for a subject, such as a nickname or a
  title the world uses, come from `aliases.ini` or the API. The result is
  stored with the version as `names_subjects`. The seed writer's own
  `names_subjects` is only a cross-check on this.
- **An unnamed version is told as ordinary talk.** The teller's prompt doesn't
  say that the listener is the subject, so they can't act on what they don't
  know. The subject still hears it. Their own memories are in their prompt,
  so they may recognise the story, or not.
- **The bio follows the same rule.** "People are saying this about you" is
  shown only for a version that names that NPC. An unnamed one is shown as
  ordinary talk.

### Lifecycle

- **Freshness:** a rumour is fresh in a mill for `fresh(r,h)` game days,
  longer the more notable it is. While fresh, it is shown in prompts and keeps
  spreading. Afterwards it is stale in that mill.
- **Retirement:** a rumour is retired when it is stale everywhere. It also goes
  stale early in a mill where it has been aired in the player's hearing
  `airings_until_stale` times: everyone there has heard it.
- **Updates through the API:** the same `source` and `key` updates a rumour in
  place, and the API can retire one.
- **Promotion to world knowledge.** A rumour that reaches high notability and
  enough mills or holds is written once with `PublicAddWorldKnowledge`, so it
  outlives its freshness. It becomes something people just know. The
  condition expression keys it to the mills it reached, using a decorator such
  as `rumourengine_in_mill(actorUUID, "whiterun.commons")` if decorators work
  inside condition expressions. [Open question 4](#open-questions) covers the
  fallback. It is inserted with `alwaysInject: false`, so it surfaces when
  relevant rather than costing prompt space everywhere. The text is the most
  widespread version, or a settled account written at promotion time.

  As built (stage 6, 2026-11-20):

  - **Notability couldn't pick them.** On the user's save (141 rumours over
    57 game days), notability 0.7 and three holds would have kept 16, about
    eight a game month. Holds barely filter: nearly everything notable crosses
    all ten. FactionEngine gives every march 0.9, so a forage-train raid
    scored the same as the declaration of war. And harvested rumours top out
    at 0.55 (0.05 + 0.1 x interest), so a standalone RumourEngine would never
    have kept anything. The user wants it useful on its own.
  - **So the decision model is asked.** A rumour that goes stale everywhere,
    having reached at least `promote.min_reach` (0.25) of the mills' people,
    is a candidate. `decisions/rumourengine/lasting` asks, of its first
    telling, whether ordinary people across Skyrim will still bring it up a
    year from now, as a noul. At `promote.lasting` (0.4) or more it is kept.
    It is tuned on Jev 1.14 (`typesafe/jev-1.14`), the decision model most
    players will have (the user, 2026-11-20). On the save: the declaration of
    war 0.78; Ulfric's proclamations 0.32 at most; the war's skirmishes and
    guild errands 0.35 or less; every harvested fling, fight or sale 0.04 to
    0.18. Made-up yardsticks: Alduin's return 0.81, a dragon slain at
    Whiterun 0.69, Ulfric killed 0.64, the Harbinger killed 0.47, a Jarl
    murdered 0.44. So 0.4 keeps events that change Skyrim, and on this save
    only the war. `d3:4b`, tried first, reads higher and orders less cleanly
    (war 0.88, a Jarl murdered 0.85, the Hjaalmarch necromancers 0.73), so
    about 0.75 there. Kev-4B (`kev:4b-q8_0`, SkyrimNet MultiProxy's default)
    reads higher still and takes letters between factions and proclamations
    for big news (a faction letter 0.67, proclamations up to 0.64, against a
    Jarl murdered 0.49), so about 0.6 there keeps the war, Alduin and a dragon
    slain, with a few letters besides. The setting's description gives each
    model's value. A five-level score was tried before the noul and bunched
    everything between 1.4 and 2.8.
  - **Without a decision provider,** notability decides: `promote.notability`
    (0.8), the user's choice, so some regular rumours are kept.
  - **`promote`** in a request: `always` keeps it without asking, `never`
    never keeps it. A rumour retired by request is kept only with `always`.
  - **The text is a settled account,** written once by the `rumour_settle`
    prompt and variant from the first telling and up to eight versions, one
    per hold or faction, the furthest travelled first. In testing (GLM 5.3
    Flash, about 2 s and $0.0002 a call) it kept what most tellings shared,
    dropped what one or two had (a bedroom story's detail among them), and
    said where people disagreed. After three failed calls, the
    furthest-travelled telling is kept instead. The DLL puts "Old news from
    Evening Star of 4E 201, talked about across Skyrim:" in front, dated from
    when the rumour was added.
  - **The condition is the decorator `get_rumourengine_heard`,** which
    returns the promoted rumours an actor has heard as `|57|132|`:
    `contains(get_rumourengine_heard(actorUUID), "|57|")`. Tested in game on
    2026-11-20 (open question 4): a registered decorator works in a
    condition. A condition calling a decorator that isn't registered is false
    and logs a warning each time it is checked, so after an uninstall the
    entries fall silent but are noisy. Who has heard it is the same as for a
    live rumour, with the reach as it was when the talk went stale.
    Importance is 0.8 x the higher of notability and the decision's answer,
    between 0.3 and 0.8.
  - **Taking it back.** SkyrimNet stamps world knowledge with game time 0, so
    its own history clearing never touches it. When it clears the history
    after a load, RumourEngine removes the entries it made since (and
    rumours that retired since are live again, which was a bug before). A
    rumour brought back by the API loses its entry; it is news again.
    Turning `promote.enabled` off removes every entry, keeping the texts, and
    turning it on puts them back: the clean-up before an uninstall.
  - **Once, at the first load,** rumours that had retired before this was
    built are candidates too.
- **Pruning:** retired rows stay in the database for dedupe and the dashboard.
  *Not built:* deleting them after `prune_after_days`. The user's save holds
  141 rumours and their versions in under 2 MB.

### Persistence

- **One SQLite database per save, as in FactionEngine:**
  `SKSE/Plugins/RumourEngine/data/RumourEngine-<SkyrimNet save id>.db`.
  Nothing goes in the SKSE co-save.
- **Actors are keyed by SkyrimNet UUID**, which is stable across load orders.
- **Tables:**
  - `rumours`: origin, source, key, topics, notability, scope, created and
    retired times, promotion id;
  - `rumour_subjects`;
  - `rumour_mills`: arrival, parent mill, hop count, notability, fresh-until
    time, airings;
  - `versions`: text, `names_subjects`, model and written time, per rumour
    and mill;
  - `airings`, with who was in earshot;
  - `heard`: the few people who have certainly heard a rumour, whatever their
    roll;
  - `placements`: where dismissed followers and moved NPCs settled (the mills,
    and since when), and followers' joining and dismissal times;
  - `pending_moves`: a move under way, and its first sighting;
  - `harvest_watermarks`;
  - `screened`;
  - `gossip_scores`.
- **Timeline.** Every row is stamped with game time.
  - **Pending:** while SkyrimNet's keep/clear check is pending
    (`PublicGetTimelineState() == 1`), harvesting, hops and starts pause.
  - **Cleared:** on `SkyrimNet_TimelineResolved` with state "cleared", rows
    newer than the current game time are deleted, matching what SkyrimNet did
    to its own history.
  - **Kept:** with "kept", they stay.

## Public API

API version 1 was frozen on 2026-11-12, once FactionEngine had adopted it and
run on it for some days of play. `docs/API.md` is the reference. Later
versions stay backwards-compatible: they add, and change nothing that's there.

### Conventions

- **Plain C ABI.** Functions are `extern "C"` exports from `RumourEngine.dll`,
  resolved with `GetProcAddress`. A shipped header, `RumourEngineAPI.h`, does
  the binding, as SkyrimNet's `PublicAPI.h` does. Unlike SkyrimNet's, nothing
  crosses the boundary as `std::string` or `std::function`. Those tie the
  caller to the same MSVC standard library build.
- **Strings.** Strings go in as UTF-8 `const char*`. Queries are synchronous
  and return a `char*` that RumourEngine allocated. The caller releases it with
  `RumourEngine_Free`, never its own `free`, because the two DLLs may use
  different C runtimes. A null return means not found or failed.
- **Listener events are the one asynchronous path,** so they stay callbacks.
  The string passed to a listener is only valid during the call, and is not
  freed by the caller.
- **Complex input and output is JSON**, which keeps the ABI stable as fields
  are added. Unknown fields are ignored.
- **Thread safety.** Every function is safe from any thread. `AddRumour`
  returns at once; any LLM work happens later.
- **Readiness.** Calls fail with 0 until a save is loaded and the database is
  open. Wait for the `ready` listener event, or the `RumourEngine_Ready`
  ModEvent.
- **Ids** are 32-bit, so Papyrus can hold them.

### C++

```cpp
int  RumourEngine_GetApiVersion();
bool RumourEngine_IsReady();

// Every char* returned below is UTF-8 JSON allocated by RumourEngine. Release it with
// RumourEngine_Free, not free(): the two DLLs may use different C runtimes.
void RumourEngine_Free(void* p);

// Returns the rumour id (>0), or 0 when rejected; the reason is logged and
// available from RumourEngine_GetLastError on the same thread.
int32_t RumourEngine_AddRumour(const char* requestJson);
bool    RumourEngine_RetireRumour(int32_t id);
bool    RumourEngine_RetireRumourByKey(const char* source, const char* key);

char* RumourEngine_GetRumour(int32_t id);                                // nullptr if unknown
char* RumourEngine_GetRumoursForActor(uint32_t formId, int maxCount);    // only what that actor has heard
char* RumourEngine_GetMillsForActor(uint32_t formId);
char* RumourEngine_GetMills();
bool  RumourEngine_HasHeard(int32_t id, uint32_t formId);
char* RumourEngine_GetLastError();                                       // this thread's last failure, or nullptr

// Events, as JSON on a RumourEngine worker thread; do not call RE:: from the callback.
// The string is only valid during the call. Types: ready, seeded, arrived, aired, retired, promoted.
typedef void (*RumourEngine_EventCallback)(const char* eventJson, void* user);
uint64_t RumourEngine_RegisterListener(const char* eventTypesCSV, RumourEngine_EventCallback cb, void* user);
bool     RumourEngine_UnregisterListener(uint64_t listenerId);
```

The `AddRumour` request:

```jsonc
{
  "source": "FactionEngine",        // required: the calling mod; scopes "key" and appears in logs
  "key": "event-1234",              // optional: same source+key updates the rumour instead of adding another
  "text": "…",                      // the first telling, third person, the claim itself with no "they say"
  "origin": { "actor": "Skyrim.esm:0x01A694" },   // or { "location": "Skyrim.esm:0x01A26F" },
                                                  // { "mills": ["whiterun.court"] }, { "hold": "Whiterun" };
                                                  // forms as "Plugin.esp:0xLOCALID" or a runtime form id number
  "notability": 0.6,                // 0–1: how far and how long it travels
  "subjects": ["Skyrim.esm:0x000014"],            // who it is about; the player is 0x14
  "scope": "spread",                // spread (default): hops from the origin
                                    // proclaimed: reaches every mill, delayed by distance (heralds, notices)
                                    // local: stays in the origin mills
  "retell": true,                   // false: every mill gets "text" (or "versions") verbatim
  "versions": {                     // optional pre-written tellings, used instead of retelling where given
    "hold:Riften": "…",
    "mill:riften.court": "…"
  },
  "fresh_days": 3.0,                // optional override of the notability-based freshness
  "delay_hours": 6.0,               // optional: game hours before the origin mills know (default 0);
                                    // add the rumour when the event happens, and the town still
                                    // hears it a while later. Hops, proclaimed arrivals and
                                    // freshness all start from then.
  "heard_by": ["Skyrim.esm:0x01A694"], // optional: people who certainly know it, whatever their
                                    // mill (witnesses; the one whose memory it came from)
  "promote": "auto",                // auto / never / always
  "topics": ["politics"]            // optional; feeds the interest table
}
```

Rumours added through the API skip screening; the caller has already decided.
They join the same hop simulation, retelling, delivery and lifecycle as
harvested ones. A caller that writes its own tellings per hold, as FactionEngine
does, passes them in `versions`. Anything not covered is retold from the
nearest version.

The `seeded` event carries `arrives`, the game day the origin mills know it.
With `delay_hours`, each origin mill also fires `arrived` when that time comes,
as a hop's destination does. Until then nobody there tells it, hears it or
passes it on.

### Papyrus

`RumourEngine.psc` holds only global native functions. No script is attached to
anything, so nothing is stored in saves.

```papyrus
int      Function GetApiVersion() global native
bool     Function IsReady() global native
int      Function AddRumour(string asSource, string asText, ObjectReference akOrigin, \
                            float afNotability = 0.5, Actor[] akSubjects = None, string asKey = "") global native
int      Function AddRumourInMill(string asSource, string asText, string asMill,                                   float afNotability = 0.5, string asKey = "") global native
int      Function AddRumourJson(string asRequestJson) global native
bool     Function RetireRumour(int aiId) global native
bool     Function RetireRumourByKey(string asSource, string asKey) global native
string[] Function GetRumoursFor(Actor akActor, int aiMax = 3) global native
string[] Function GetMillsFor(Actor akActor) global native
bool     Function HasHeard(int aiId, Actor akActor) global native
```

- **`akOrigin`** may be an actor, or any reference whose location is used.
- **By hand, for testing:** `AddRumourInMill` takes a mill id and nothing a
  console can't type, for console mods that add `cgf` (the vanilla console
  has no way to call a Papyrus function). Without one, the **debug inbox**
  takes requests as files. Any `*.json` in `RumourEngine_Inbox` next to the
  log is read every few seconds, then renamed `.done` or `.failed`. A file
  holds one AddRumour request, or an array of them, and `{"retire": id}`
  retires one. The folder is never created by the mod, so the inbox is off
  until someone makes it.

### ModEvents (soft dependency, no compile-time link)

- **In:** `SendModEvent("RumourEngine_AddRumour", requestJson)` takes the same
  JSON as `AddRumourJson`. The rumour id is not returned, so use `key` to refer
  to it later. Retiring works the same way through `RumourEngine_RetireRumour`,
  with `{"source":…, "key":…}`.
- **Out:** `RumourEngine_Ready` is sent once a save's database is open.

## Settings

Settings are SkyrimNet plugin settings (`settings/RumourEngine.yaml` in the
layer, with the player's copy in `config/plugins/RumourEngine/`), as
FactionEngine does it. They are editable on SkyrimNet's dashboard. The main
ones:

- `enabled`, `harvest.enabled`, `starter_enabled`
- `harvest.interval_hours` (2), `harvest.window_days` (2),
  `harvest.importance_floor` (0.5), `harvest.gossip_floor_bonus` (0.2),
  `harvest.screen_threshold` (0.6), `harvest.gossip_threshold_bonus` (0.25),
  `harvest.min_interest` (3), `harvest.max_actors` (10),
  `harvest.memories_per_actor` (6), `harvest.max_seeds_per_sweep` (6), and
  the `rumour_seed` and `rumour_screen` variants for their models
- `mill_floor` (4)
- `spread.tick_hours` (2), `spread.decay_local` (0.9),
  `spread.decay_travel` (0.6), `spread.decay_travel_per_notability` (0.3),
  `spread.travel_rate_scale` (8), `spread.travel_exponent` (4),
  `spread.notability_floor` (0.15), `spread.base_fresh_days` (2),
  `spread.fresh_days_per_notability` (6), `spread.max_mills_per_rumour` (80)
- `max_live_per_mill`, `max_live_total`, `max_rumours_per_prompt`
- `reach_base_rate` (0.5), `reach_gossip_rate` (2.0)
- `retell.mode` (`lazy` / `eager`), and the `rumour_retell` variant for its
  model
- `tell_player`, `tell_party`
- `settle.hours` (24), `settle.max_days` (3), `settle.follower_harvest_max` (6)
- `airings_until_stale`
- `promote.enabled`, `promote.min_reach` (0.25), `promote.lasting` (0.4),
  `promote.notability` (0.8, without a decision provider), and the
  `rumour_settle` variant for its model
- `quests.enabled`, `quests.heard` (0.5), `quests.notability` (0.7, without
  a decision provider), and the `rumour_quest` variant for its model
- `starter_cooldown_seconds`, `item_cooldown_minutes`

A dedicated trace log, `RumourEngine_Trace.log`, records every seed, hop,
version, airing and retirement, separate from the main log. NarrativeEngine
found this indispensable for tuning.

## Packaging

The mod ships:

- `SKSE/Plugins/RumourEngine.dll`;
- `Scripts/RumourEngine.pex`, natives only;
- the SkyrimNet layer `SKSE/Plugins/SkyrimNet/external/maeyanie.rumourengine/`:
  the bio submodules (the talk block and the gossip's news line), the seed,
  retell, settle, quest and screening prompts, the decision templates under
  `prompts/decisions/rumourengine/`, and settings;
- data files `mills.ini` (with its [Links]), `gossips.ini` and `quests.ini` in
  `SKSE/Plugins/RumourEngine/`. Other mods' `*_mills.ini`, `*_gossips.ini`
  and `*_quests.ini` patches go beside them.

**No ESP,** unless something later turns out to need one. Nothing is baked
into saves. The mod can be added to or removed from a playthrough, and every
update ships cleanly.

**Licence:** Apache-2.0, including the API header. Anything borrowed from
NarrativeEngine (its location classifier or ini lists, say) is credited in a
NOTICE file.

**Requirements:** SkyrimNet public API v12 or later (beta 26+) for custom
decisions, and CommonLibSSE-NG.

**Toolchain:** the same as FactionEngine. That means vcpkg, CMake and Ninja
with VS 18, the triplet `x64-windows-static-md`, and the CK's Papyrus compiler
with the SKSE-extended headers first in the import order.

## Staging

Each stage should be testable on its own.

### Stage 0 — offline prototypes

**Done, 2026-10-30.** The results are in [stage0-results.md](stage0-results.md)
and have been folded into this record:
- the merged mill strategy at floor 4, enclaves, and dropping duplicate
  institutions;
- travelling trades as links;
- placement after enable parents;
- the n² hop rate with two decays;
- the reach rates;
- the retell model and output format.

These are Python tools under `tools/` that run against the game data before any
C++ exists.

1. **Mill builder.**
   - Parse Skyrim.esm, Update and the DLC masters: placed NPCs, their
     locations, `LCTN` parents and keywords, and factions.
     NarrativeEngine's `build-social-graph.py` (Apache-2.0) and the
     TradeRoutesKnowledge esm parser are both starting points.
   - Build mills under several strategies: settlement only; settlement ×
     stratum; that plus institutions; and that with small mills merged below a
     size floor.
   - Report coverage (the share of NPCs placed in a mill), the mill-size
     distribution and the mill count.
2. **Hop simulator.** Monte Carlo over each strategy's graph, seeded where a
   player actually goes. It reports:
   - time to reach the next hold and the far side of the province (p50/p90);
   - mills reached per rumour;
   - **who has heard it:** the share of each mill that has heard a rumour over
     time, by gossip score;
   - **sameness:** the overlap of what is known between mills in one town and
     in one hold, and between two people in one mill. It shows "everyone knows
     the same thing" as a number;
   - **retell calls under lazy mode,** given a simulated player itinerary,
     against eager mode.

   The target shape: most rumours stay in their hold, some cross to the next,
   and a rare few go province-wide, with recognisable drift by the next town.
3. **Retell panel.** Take real seeded rumours and run six-hop chains through
   candidate cheap models. Score each step's overlap with its parent: about 1
   is copying, about 0 is replacement, and we want the middle. Also measure
   cost, latency and cached tokens, and have the user read the chains. Reuse
   the proxy's replay tooling.

### Later stages

1. **Skeleton.** *Done 2026-10-30.* In the user's game the plugin scan takes
   0.5 s and a build about 45 ms. The mills match the stage 0 builder run on
   the same load order, except where the save's enable state differs. One
   example: the Thieves Guild quest posts a soldier at the East Empire
   warehouse, which takes the Company just under the 80% duplicate threshold,
   so it becomes an institution. Corrections the in-game check forced, in
   both builders:
   - holds are the outermost hold-keyworded ancestor;
   - unique-NPC rows are read from the files;
   - player homes count as part of their town;
   - small mills with no position fold into the hold's biggest;
   - nested leveled templates are followed;
   - a reference's location falls back to its worldspace's.
   - The DLL loads and binds SkyrimNet's API.
   - Mills are built after the plugin scan at `kDataLoaded`, and again at each
     save load, and dumped to the log and `RumourEngine_Mills.json`.
     `tools/compare_mills.py` checks them against the stage 0 builder run on
     the same load order.
   - The per-save database and timeline handling work.
   - The Papyrus natives exist as stubs.
2. **Manual rumours and delivery.** *Done 2026-10-30.* In the user's game,
   Hulda passed a test rumour on to the player in the Bannered Mare.
   - `AddRumour` (C++, Papyrus, ModEvent).
   - The bio block, the GameMaster line and the GossipStarter, with
     `tell_player` and `tell_party` from the start.
   - Everyone in a mill has heard everything until stage 5.
   - Test with `cgf`. No LLM is needed yet beyond the dialogue itself.

   Stand-ins until later stages:
   - **Placement.** A `spread` rumour stays in its origin mills until hops
     exist (stage 3). `proclaimed` reaches every mill at once rather than by
     distance.
   - **Gossip scores** come from job factions alone, the table stage 0's
     simulator used, plus a fixed per-person jitter.
   - **The name check** runs on every version, so "about them" and "is it
     true" already work for rumours given subjects.
   - **Settings are cached**, refreshed every few seconds, because the prompt
     decorators run inside SkyrimNet's rendering and shouldn't call back into
     it.
3. **Hops and retelling.** The simulation, the `rumour_retell` prompt, lazy
   materialisation with prefetch on location change, and the proclaimed and
   local scopes. *Done 2026-10-31.* In the user's game a test rumour hopped
   from Whiterun's commons to its garrison, Riverwood, Heljarchen Hall and
   Windhelm, on the rolls `hopsim.py` predicted. All three retelling triggers
   fired: an arrival where the player was (the garrison), the player entering
   a place (Heljarchen), and a prompt meeting an unwritten mill (Ambarys
   Rendar's, through MCP `render_template`, for Windhelm). A rumour added in
   Riverwood with `delay_hours: 3` stayed out of Orgnar's bio until three
   game hours later, then led it. The proclaimed and local scopes are not yet
   run in game.
   - **Hops** (`Spread.cpp`) port `tools/hopsim.py`: the same rates, delays
     and two decays, ticking every `spread.tick_hours` of game time from the
     GossipStarter's worker. A long sleep catches up at most 60 days of ticks.
     The roll for each (rumour, edge, tick) is a fixed hash, so a reload
     replays the same spread.
   - **A hop is stored when it is decided,** with its arrival time:
     `rumour_mills.game_time` is the decision and `arrived` the arrival
     (schema 3). Rolling the timeline back then drops only hops decided
     after the load point, and keeps news already on the road.
   - **Proclaimed** rumours reach every other mill after the travel time
     from the nearest origin mill, at full notability. **Local** ones never
     hop.
   - **`delay_hours`** sets the origin mills' arrival that far ahead, so a
     caller can add a rumour as the event happens. Everything else already
     waits on arrival.
   - **Versions the caller gave** (`versions`, by mill or hold) are kept on
     the rumour and used when it reaches those mills later, as is the first
     telling for `"retell": false`. Updating a rumour by key drops the
     retellings, which were of the old story.
   - **Retelling** (`Retell.cpp`) starts when the player enters a settlement
     (`TESActorLocationChangeEvent`), when a rumour arrives in a mill of the
     settlement the player is in, or when a prompt would show a rumour whose
     version there is unwritten. A rumour still unwritten in an NPC's mill is
     left out of their prompt, and the next of their mills that has a version
     is used instead.
   - **One call writes one path:** from the nearest mill with a version,
     through every mill with none, up to 8 steps. Where two mills of a place
     need versions, the longest path goes first; it usually covers the
     other. One call per rumour runs at a time, and at most four at once.
   - **Failures** are retried twice, 30 and 60 seconds later. After the third,
     the version before is passed on unchanged, so nobody goes without the
     talk for want of a model.
   - **Mill lines** come from `SKSE/Plugins/RumourEngine/mills.ini`,
     `[Lines]`, written for the vanilla mills from the stage 0 panel's, and
     from patches. Mills none of them list get a line made from their kind
     and place.
   - **`retell.mode: eager`** writes every arrival at once, for testing.
     `[Retell]` makes single mills eager (Eager mills, above).
4. **Harvest, screening and seeding.** Built in two halves, each tested on
   its own.

   **4a, harvest and overhearing.** *Built 2026-11-01.* In the user's game the
   harvest, screening, discretion and seeding work: Arcadia, Lucan and Taarie's
   memories became rumours in their own mills, each as much as their profile
   says they'd tell. Not yet seen in play: a second witness matched by event
   id, and overheard carry-home.
   - **The sweep** (`Harvest.cpp`) runs every `harvest.interval_hours` on the
     GossipStarter's worker. `PublicGetActorEngagement` names who has had
     important memories within `harvest.window_days`, those with the most
     first. The player and the current party are left out, and so is anyone
     in no mill. Each person's memories are fetched with the importance
     floor (lower for gossips) and the window applied in SQL, newest first.
   - **Each memory is screened once,** by SkyrimNet memory id in `screened`.
     That is all a watermark would do, so there is no per-actor watermark: the
     window bounds the query and the screened set dedupes it. Diary entries
     (`Diary Entry:` content, typed EXPERIENCE) are skipped, as
     NarrativeEngine found.
   - **Other witnesses cost nothing.** SkyrimNet gives every witness of one
     happening the same `related_event_ids`, and `screened` keeps them for
     seeded memories. A later memory sharing one reinforces that rumour with
     no call: fresh again where it was, and first-hand (the first telling) in
     the witness's mills where it wasn't.
   - **Screening** is `decisions/rumourengine/screen_memories`, one request
     per person with up to `harvest.memories_per_actor` memories: `spread_m<k>`
     (noul), `tell_m<k>` (choice), `interest_m<k>` (score 1–5) and `same_m<k>`
     (choice), as above. SkyrimNet reports a score as the expected level
     counted from 0, so 1 is added back. It is compared unrounded: in play,
     rounding let "mostly a 2" (2.75) through a bar of 3, and with it a
     mammoth heart sold for ninety septims.
   - **Discretion is per character** (the user's call, 2026-11-01). `tell`
     asks how much this person would tell: `everything`, juicy details and
     all, or the `gist` without the intimate details, their feelings or
     confidences. Both `spread` and `tell` are judged from their profile, so
     some people share every detail, some only that something happened, and
     some nothing. The seed writer follows the answer; it never sees the
     profile itself. The first harvest in play turned Arcadia's nights with
     the player into "Rennard slept with Arcadia…" because the writer was told
     to name every deed plainly.
     `same` offers the live rumours the person knows (their mills', and what
     they overheard) and their earlier memories in the batch, and is left out
     when there is nothing to compare with. With no decision provider, the
     `rumour_screen` chat prompt asks the same, one line per memory.
   - **Seeding** is one `rumour_seed` call per sweep for up to
     `harvest.max_seeds_per_sweep` accepted memories, the most interesting
     first; the rest wait. Accepted memories sharing event ids become one
     rumour, the others' owners its witnesses. The answer is a block per
     memory (`telling`, `about` by per-memory subject key, `topics`) or
     `skip`, not JSON. Each becomes an `AddRumour` from source `RumourEngine`,
     key `mem:<memory id>`, starting in the owner's mills, with
     `heard_by` the owner and witnesses.
   - **One stretch, one rumour** (added 2026-11-04). SkyrimNet splits a long
     fight into several memories about an hour apart, with no event ids in
     common. Camilla Valerius's catch-up made four rumours of one fight at
     Mzulft: the screen saw them in two requests, and the decision model
     called each one new even beside the others. Now any of one person's
     accepted memories that are each within 2 game hours of the next are
     one happening. The most important one is written, with the others
     beside it as "their other memory of it", and the interest is the
     highest of them. A later memory within 2 hours of one that became a
     rumour joins that rumour without being screened, as another witness's
     does by event id. Schema 9 keeps `screened.memory_day` for that.
     `harvest.memories_per_actor` went from 4 to 6, so a follower's
     catch-up (up to 6 memories) is screened in one request. The catch-up
     also picks its 6 from 6 different stretches of time: going down the
     trip's memories by importance, one within 2 hours of a memory already
     taken goes along with that one instead of using a slot. It runs only
     once, so Camilla's would otherwise have spent four of its six slots
     on Mzulft and never seen the rest of her trip.
   - **Remembering the telling** (added 2026-11-10). The person a harvested
     rumour starts with gets a SkyrimNet memory of passing it on: "I'm
     the one who spread it. I couldn't keep my mouth shut, and gossiped to
     Whiterun's townsfolk about what I knew: …", with the seed's telling, at
     importance 0.5, tagged `rumourengine`. Leading with the telling matters:
     tested on Ahlam's memory, "did you tell anyone about what happened in
     the kitchen?" went from 4th (similarity 0.40) to 1st (0.58). Without it, every suspect sincerely believed they
     weren't the one. Ahlam seeded both the Ysolda rumour and the one about
     the player's telling-off, and had no memory of either. Only the
     seeder gets one: hops are offstage, and a telling on screen is
     SkyrimNet's to remember. The harvest skips memories with the tag, and
     records each one as `own` in `screened`. `harvest.remember_telling`
     turns it off.
   - **Notability** comes from interest: 0.05 + 0.1 × interest, so 3 gives
     0.35 and 5 gives 0.55. It was 0.15 × interest until 2026-11-03. Under
     the retuned spread curve, Aia Arria's night with the player (interest
     4.54, so 0.75) would have crossed most of Skyrim. At 0.5, in the sim, it
     gets round Haafingar, reaches Dragon Bridge 70% of the time and Whiterun
     21%. That's the user's choice. The topic tags are stored; the interest
     table is still off.
   - **Overhearing:** after a telling, everyone in earshot who isn't in the
     party and lives in another mill carries it to their home mill, after the
     travel time, with the usual decay (`Rumours::CarryHome`).

   **4b, dismissed followers and observed homes.** *Built 2026-11-01, not yet
   run in game.*
   - **Who is watched:** named NPCs with a persistent reference, placed or
     not, not in a hostile faction and not a travelling trade (a faction
     with "Caravan" in its editor ID, carriage drivers, the courier): the
     first look in play had the Khajiit caravans about to settle in Dawnstar
     and Solitude. 851 people in the user's game, looked up in 0.7 ms. The
     builder keeps the list
     (`MillSet::watch`) with each one's stratum from their factions, so
     settling follows the builder's own rules: factions first, then a
     castle's court or a barracks' garrison for someone no faction places.
   - **Every game hour** the main thread looks up where each is
     (`GetCurrentLocation`) and whether they're in the party, and the worker
     acts on it (`Placements.cpp`). The first look of a session logs how many
     are following and who is in a town other than their home, which shows a
     rule mismatch before it turns into moves.
   - **A move** is someone in another town's mills for `settle.hours` (24)
     with no look finding them elsewhere; the road, the wilds and dungeons
     reset it. Another stratum of their own town isn't a move: the first look
     found Blue Palace servants in the streets and Markarth's commons in
     Understone Keep.
     The mill they settled in replaces their place mill for every lookup
     (`Mills::SetPlacement`, honoured by `MillSet::MillsFor`); institutions
     stay. Going back to their built home clears it. Placements are stored
     in `placements` (schema 5) by SkyrimNet UUID and applied at load.
   - **Followers:** joining clears any placement. Dismissal starts the wait;
     their memories wait too, and the regular sweep skips them. Once they
     settle, or after `settle.max_days` (3) wherever they are, the
     `settle.follower_harvest_max` (6) most important memories from joining to
     leaving are screened, with the screening told they're just back from
     travelling with the player. Someone already following when first seen
     has no known join time, so all their history counts.
   - **Not done:** SeverActions' own follower state isn't read; the teammate
     flag and `CurrentFollowerFaction` cover its followers. Follower mods with
     a faction of their own are listed in `mills.ini` `[Party]` (Sofia's, from
     2026-11-26). Shared-member
     edges between mills still use the built membership.

   [`docs/API.md`](API.md), drafted 2026-11-01, describes the API as built,
   for FactionEngine. API v1 was frozen on 2026-11-12, after FactionEngine had
   adopted it (the user's call).
5. **Gossip scores and who has heard it.** *Built 2026-11-12; its log lines
   looked right in play the same day.* Job defaults and overrides in `gossips.ini`, the Decisions
   reading of each bio and its re-reading when the bio changes, and the
   per-person roll ("Gossip score" and "Who has heard it" say how each was
   built). "Is it true" tellings were already in the starter.
6. **Promotion and polish.** World-knowledge promotion, retirement tuning, and
   a dashboard tab if the trace log is not enough. *Promotion built
   2026-11-20* ("Promotion to world knowledge" under Lifecycle says how).
   Retirement is left as built, and there is no dashboard tab: neither came
   up in play. *Quest endings built 2026-11-21* ("Quest endings" says how).

**FactionEngine as a client** is FactionEngine's work, not this repository's.
Its plan is a soft dependency: use RumourEngine when present, otherwise fall
back to its own simple model.

## Open questions

1. **Defaults for who can be told.** The table proposes `taverns` for both the
   player and the party.
2. ~~**Mill count.**~~ Settled by stage 0: about 51 on vanilla (merged at
   floor 4), after the stage 1 corrections.
3. ~~**Does an unloaded NPC's location update as they travel?**~~ Mostly, by
   play on 2026-11-05. 622 of 652 watched people had a location while the
   player was elsewhere, and SeverActions' offscreen work moves showed up.
   The game sometimes keeps someone outdoors in no location, as with Vesyra
   in Solitude. For those, the scan takes the location of the exterior cell
   they stand in (see Placements, "Out of sight").
4. ~~**Can decorators be used in world-knowledge condition expressions?**~~
   Yes, tested in game on 2026-11-20: an entry whose condition called
   `get_rumourengine_talk` reached Hulda and not Ysolda. One calling a
   decorator that doesn't exist reached nobody, with a warning in SkyrimNet's
   log each time.
5. **Verify the assumption** that SkyrimNet forms memories only for actors
   loaded around the player. The case for simulating hops rests on it.
6. **What people actually say about harvested memories.** The seed prompt
   writes the owner's own telling, as Arcadia would tell Carlotta, which may be
   more than most of the marketplace would pass on. Once there's more play,
   review the harvest's prompt and model, unless the dialogue model already
   covers this (the user, 2026-11-01).
7. **How much exaggeration retelling may add** before it counts as a different
   event. Taste, tuned from the retell panel. The panel suggests the line is
   loose. A wild version, such as Jenassa's debt becoming an Imperial officer
   paying a Nord a thousand gold to go into the Reach, still sounds like
   rumour. What breaks it is a version that swaps who did the deed.

## Prior art

- **NarrativeEngine,** Phase 13, pdusen, Apache-2.0. Source at
  `D:\Devel\build\skyrimnet-plugin-narrative-engine`. RumourEngine borrows its
  ideas, not its model:
  - location tiers from parent walks and keywords;
  - faction filtering, size-band failures and the denylist;
  - the screening questions (would they tell / is it already going round /
    would anyone listen);
  - "drift, not replacement";
  - SIR, applied here to mills;
  - its memory-harvest findings.
- **FactionEngine** (ADR-002 stage 9). The prompt-side delivery: the bio block,
  the GameMaster line and the GossipStarter, with their cooldowns.
