# Stage 0 results: mills, hops and retelling

**Date:** 2026-10-30. **Design:** [ADR-001](ADR-001-rumour-mills.md), stage 0.
**Tools:** `tools/` (Python 3.12, standard library only). Raw outputs are in
`tools/results/` and `tools/panel/results/`.

Nothing here touches the game. The mill builder reads the five vanilla masters
directly, the hop simulator runs on its output, and the retell panel sends
hand-written seeds to OpenRouter. The seeds are first tellings taken from real
SkyrimNet memories in the current save.

## Summary

- **About 53 mills** cover 965 people (734 named, 231 generic guards and
  workers). That is the low end of the ADR's 40–120 guess.
- **The spread shape works,** with two formula changes: the hop rate goes with
  notability *squared*, and notability fades faster between towns than within
  one.
  - Under the merged strategy, 76% of minor talk stays in its hold.
  - About half of moderate talk reaches a third hold.
  - 9 in 10 major rumours cross into another hold, and 39% reach six holds or
    more.
  - The next hold is about 3 days away.
- **Lazy retelling needs about 4× fewer calls than eager:** 41 against 180
  per 60 game days, which is under one call a day.
- **Per-person weighting is the big win for variety.** Two people in the same
  mill share 57% of what they've heard, against 99% without weighting. Two
  people in the same town share 49%, against 90%.
- **Retell model:** GLM 5.3 Flash is the default. Gemma 4 31B, which
  FactionEngine already uses, is as good. Claude Haiku 5.5 writes
  the most natural speech, and DeepSeek V4 Flash is a good fourth. Mistral
  Small 3.2, Mistral Nemo, Llama 3.3 70B and Nemotron 3.5 Super lose the story
  or the English. Muse Spark is faithful but bland, drifts little, and can't turn reasoning off. Aion 3.5
  Mini is far too slow and costly. Every usable candidate except Muse costs
  $0.0002 a call or less, so price doesn't decide it.

## 1. Mill builder (`tools/extract.py`, `tools/mills.py`)

`extract.py` reads Skyrim, Update and the three DLC masters (2.5 s) into
`tools/cache/world.pkl`. Its record counts match NarrativeEngine's
measurements: 763 locations, 6362 NPCs and 911 unique-resident rows.
`mills.py` builds four strategies and the edges between their mills.

| Strategy | Mills | Size (median / p90 / max) | In 2+ mills |
| --- | --- | --- | --- |
| settlement: one per settlement | 57 | 6 / 39 / 122 | 0 |
| strata: cities split into court / garrison / commons / underworld | 70 | 7 / 35 / 67 | 0 |
| institutions: strata plus guilds | 72 | 7 / 33 / 67 | 11 |
| **merged:** institutions, small mills folded (floor 4) | **53** | 11 / 35 / 68 | 8 |
| merged, floor 10 | 31 | 22 / 68 / 95 | 0 |

The full mill list for `merged` is in `tools/results/mills-merged.txt`.

**Who is placed:**
- **Residence.** Named NPCs are placed from the locations' unique-NPC rows (808), or
  from where they are placed in the world (81). Generic NPCs count once per
  placement.
- **Excluded:**
  - hostile factions and hostile places, by location keyword;
  - player houses (*dropped in stage 1:* their residents belong to the town
    around them, and patches mark houses that real residents live in);
  - quest copies, corpses and battle extras, by name pattern (`*Dead`,
    `Corpse`, `^CW`, `^MQ`, …).

Things the data taught us:

- **Civil-war guard variants double the guard count.** Both the Imperial and
  Stormcloak guards sit enabled in the plugin, and their enable-parent markers
  decide which appear. The builder follows enable-parent chains and drops
  anything effectively disabled. The real mod reads the live enable state
  instead.
- **Companions carry guard factions.** Guild members are therefore placed with
  their town's commons before the garrison test runs.
- **Some communities live outside any settlement:** the Thalmor Embassy, High
  Hrothgar, Fort Dawnguard and Stonewater. A non-hostile place with 4 or more
  named residents becomes an *enclave* mill.
- **Inn bards, carriage drivers and caravans are not mills.** As mills they
  became a superhighway: a rumour reaching one bard reached every inn in a day,
  and 44% of all hold crossings went that way. A mill is people who talk daily,
  so these are travel links, as the ADR's `links.ini` already says.
- **An institution that is really one place's mill is dropped.** The College,
  the Thieves Guild and the Dark Brotherhood each overlap one place mill by 80%
  or more, and so do the Companions, who all live in Whiterun's commons. With
  travelling trades and duplicates gone, only the East Empire Company remained
  (8 members). *Correction from stage 1:* the builder followed only one level
  of leveled-list templates, so NPCs whose template is a leveled list inside a
  leveled list had no race and didn't count as people. Fixed, the East Empire
  Company has 12 members, 10 of them in Solitude's commons, and is dropped
  too. Vanilla then has 52 mills, none of them institutions. Folding the
  Dawnstar sanctuary's two initiates, who have no map position, into Dawnstar
  leaves 51.
- **Merging (floor 4)** folds hamlets of one to three people into the nearest
  bigger mill in their hold. Old Hroldan joins Dushnikh Yal, and lone farms
  join their village. Small strata fold into their town's commons.

**Edges:**
- shared members;
- strata within a town;
- roads: each settlement to its 3 nearest;
- carriages between the five carriage cities;
- the Windhelm–Raven Rock ship;
- a weak edge to each hold's biggest settlement.

## 2. Hop simulator (`tools/hopsim.py`)

A simulated player wanders the travel graph, staying longer in cities.
Rumours are seeded where the player is, 1.5 per game day:

- 60% minor (notability 0.2–0.4);
- 30% moderate (0.4–0.7);
- 10% major (0.7–1.0).

Each run is 10 trials of 60 game days on 2-hour ticks.

### Formula changes found by tuning

With the ADR's formula as written (rate ∝ n, one decay), spread was far too
wide: 63% of rumours reached 4+ holds. These changes produced the intended
shape:

```text
P(hop g→h) = 1 − exp(−rate(edge) × n(r,g)² × interest × Δt)      n squared, not n
n(r,h)     = n(r,g) × 0.9   within a place (strata, guild members)
           = n(r,g) × 0.6   between places
```

- **Squaring n** keeps minor talk local, and still lets notable news travel.
- **The two decays** let a rumour cross a town's strata without using up its
  reach.
- **Notability is lost on any hop to another town,** even through a shared
  guild member, and that hop takes the travel time.

Rates per day, per unit of edge weight:

| shared | strata | road | carriage | ship | hold fallback |
| --- | --- | --- | --- | --- | --- |
| 1.0 | 3.0 | 0.6 | 0.1 | 0.3 | 0.3 |

Freshness is `2 + 6n` days and the notability floor is 0.15, both as in the
ADR. Every parameter is a command-line flag.

### Outcomes (`tools/results/hopsim-tuned.txt`)

Holds reached, by the rumour's starting notability:

| Strategy | Band | 1 hold | 2 | 3 | 4–5 | 6+ | Next hold (p50) |
| --- | --- | --- | --- | --- | --- | --- | --- |
| strata | minor | 88% | 11% | 1% | 0% | 0% | 2.8 days |
| | moderate | 48% | 31% | 17% | 3% | 1% | 3.7 days |
| | major | 5% | 20% | 27% | 34% | 14% | 2.7 days |
| **merged** | minor | 76% | 21% | 3% | 0% | 0% | 2.9 days |
| | moderate | 29% | 22% | 27% | 21% | 2% | 3.0 days |
| | major | 10% | 12% | 10% | 29% | 39% | 2.1 days |
| merged, floor 10 | minor | 74% | 23% | 3% | 0% | 0% | 2.6 days |
| | moderate | 14% | 34% | 30% | 16% | 6% | 3.1 days |
| | major | 0% | 5% | 14% | 40% | 40% | 1.8 days |

Sameness is the mean overlap of what is known, measured daily:

| Strategy | Mills, same town | Mills, same hold | People, same mill | People, same town |
| --- | --- | --- | --- | --- |
| settlement | — | 0.12 | 0.55 | 0.56 |
| strata | 0.44 | 0.19 | 0.59 | 0.53 |
| merged | 0.40 | 0.22 | 0.57 | 0.49 |
| merged, no weighting | 0.44 | 0.23 | **0.99** | **0.90** |
| merged, floor 10 | 0.40 | 0.25 | 0.59 | 0.52 |

Retell calls per 60 game days:

| Strategy | Eager | Lazy | Versions written lazily |
| --- | --- | --- | --- |
| settlement | 107 | 8 | 9 |
| strata | 166 | 39 | 49 |
| merged | 180 | 41 | 52 |
| merged, floor 10 | 144 | 37 | 46 |

What the numbers say:

- **Merged at floor 4 is the recommendation.**
  - Merged and strata differ mostly in the Companions and East Empire links.
    Those carry 6% of hold crossings and push moderate rumours a little
    further.
  - The floor of 10 cuts the mill count to 31. Moderate rumours then cross
    holds 86% of the time, and villages lose their own identity.
  - The settlement strategy loses a town's inner variety entirely. The court
    and the commons would know the same things.
- **Within a town,** different strata share about 40% of their live talk. The
  court and the commons hear different things, as intended.
- **Mill count barely moves sameness between people;** per-person weighting
  does almost all of it. So the mill count can be chosen for spreading speed
  and village identity alone.
- **Lazy retelling writes about 1.3 versions per call.** The cost grows with
  where the player goes. A player who stays put pays for almost nothing.

### Who has heard it

Tuned settings: `reach_base_rate` 0.5, `reach_gossip_rate` 2.0. The table is
the share of a mill that has heard a rumour, by how long it has been there:

| | <½ day | ½–1 day | 1–2 days | 2–4 days | 4+ days |
| --- | --- | --- | --- | --- | --- |
| gossip score < 0.35 | 15% | 36% | 52% | 64% | 87% |
| gossip score 0.35–0.65 | 23% | 52% | 69% | 80% | 96% |
| gossip score > 0.65 | 34% | 77% | 93% | 99% | 100% |

- **Minor talk** (n < 0.3) reaches 38% of the low-gossip people, 54% of the
  middling ones and 83% of the gossips.
- **Notable talk** (n > 0.6) reaches 86–92% of everyone.
- **The 4+ day column is mostly notable rumours,** because minor ones have
  gone stale by then.

The gossip scores here are placeholders taken from job factions (innkeepers
0.9, merchants 0.7, guards 0.3, …). The real scores come in stage 5.

## 3. Retell panel (`tools/panel/`)

Six seeds, each hand-written from a real high-importance memory in the current
save:

- Adelaisa on Haldyn's death;
- Stig on Jenassa's debt to the East Empire Company;
- Ralof on Helgen;
- Vesyra on the Sanguine ledger;
- Arcadia's scandal;
- the wounded soldier at the Temple of Kynareth.

Each seed was sent down a seven-mill path, which is six retellings. Each call
wrote two of them, as a lazy call writes the steps to one settlement. Eight
models ran two chains per seed at temperature 1.0, with reasoning turned off.
The whole run cost $0.023.

| Model | Step similarity | Names kept to the end | Words per telling | $ per call | s per call | Verdict |
| --- | --- | --- | --- | --- | --- | --- |
| google/gemma-4-31b-it | 0.17 | 3% | 31 | 0.00010 | 5.0 | **alternative**: punchy, slanted, keeps the event |
| anthropic/claude-haiku-5.5 | 0.26 | 36% | 47 | 0.00017 | 2.2 | the most natural speech; occasionally swaps who did it late in a chain |
| deepseek/deepseek-v4-flash | 0.20 | 29% | 58 | 0.00008 | 4.9 | rich and coherent, but long |
| google/gemma-4-26b-a4b-it | 0.18 | 0% | 31 | 0.00004 | 2.2 | cheapest usable; drifts hard (a scandal became a madman praying) |
| qwen/qwen3.7-flash | 0.17 | 4% | 37 | 0.00003 | 2.3 | usable, but sometimes copies the step before almost word for word |
| meta-llama/llama-3.3-70b-instruct | 0.19 | 15% | 51 | 0.00015 | 7.4 | run-on sentences; the event shifts to who's plotting it |
| mistralai/mistral-small-3.2-24b-instruct | 0.13 | 12% | 39 | 0.00007 | 5.4 | loses the event ("a prisoner revolt"), garbled words |
| mistralai/mistral-nemo | 0.12 | 11% | 28 | 0.00001 | 8.7 | broken English |

No model copied the previous step outright. Step similarity sat between 0.12
and 0.26 for all of them, so the "repeating word for word" worry didn't show up
with this prompt. Drift is the real difference. A cheap model drifts until the
story is gone, and a good one keeps the core while the details change.

An example, the Haldyn seed through Gemma 4 31B (Windhelm → … → the Blue Palace):

> **Windhelm's guards:** Word is some Breton mercenary took out the pirate
> boss, Haldyn, and his fog-trick at Japhet's Folly. The East Empire Company
> moved in to clean up the mess, so keep an eye out for any Imperial spies
> sliding into the city on the tide.
>
> **Morthal:** I heard a stranger came from the coast and slaughtered a whole
> nest of brigands in some cave. They say the Empire's moving in to take over,
> though I reckon there's some dark business afoot if a lone wanderer is doing
> their killing.
>
> **The Blue Palace:** Reports indicate a mercenary has neutralized a
> significant brigand outpost in the marshes. General Tullius is coordinating a
> deployment to ensure the region remains under Imperial jurisdiction.

All the chains are in `tools/panel/results/panel-20261030-053754.md`. They're
worth a read; the numbers don't capture taste.

Lessons for the real prompt:

- **Numbered lines, not JSON.** Three of the cheap models wrote invalid JSON
  about half the time. One numbered line per step parsed every time.
- **Turn reasoning off.** DeepSeek V4 Flash and Qwen 3.7 Flash returned empty
  answers until reasoning was disabled. They had spent the whole token budget
  thinking.
- **Prompt caching is moot at this size.**
  - Gemma and DeepSeek already got 38–49% of input tokens from cache, with no
    hints.
  - Haiku got none. The system prompt is far below Anthropic's minimum
    cacheable length.
  - At $0.0001 a call there is nothing worth saving. Keep the cache-friendly
    layout anyway, since it costs nothing.
- **Euphemism is fragile.** "Showed me how devoted [to Dibella]" became
  nakedness, prayer, necromancy or eloping, depending on the model. The seed
  writer should state the event plainly, and leave the drift to the
  retellings.

### Second round: four more models

The same six seeds, two chains each, same settings. Reports:
`panel-20261030-065258.md` (GLM), `panel-20261030-065528.md` (Nemotron) and
`panel-20261030-070259.md` (Muse Spark, Aion).

| Model | Step similarity | Names kept to the end | Words per telling | $ per call | s per call | Verdict |
| --- | --- | --- | --- | --- | --- | --- |
| ~z-ai/glm-flash-latest (= z-ai/glm-5.3-flash) | 0.18 | 14% | about 50 | 0.00008 | 3.0 | **the new default**: natural speech, strong slant per group; once swapped attacker and victim |
| meta/muse-spark-1.2-contributor | 0.26 | 32% | 56 | 0.00032 | 8.6 | the most faithful, but reasoning can't be turned off; long run-on sentences |
| nvidia/nemotron-3.5-super-vl:free | 0.27 | 20% | 50 | free | 8.4 | invents events; some copying |
| aion-labs/aion-3.5-mini | 0.22 | 42% | 62 | 0.00339 | 37.7 | rejected: reasons 2,000–4,000 tokens a call, 2 of 12 chains finished |

- **GLM Flash** reads like talk overheard in a tavern, and each group gives it
  its own slant. The Cidhna Mine prisoners blame the Silver-Bloods for Helgen.
  Rorikstead won't believe the dragon until it flies over the farm. Its faults:
  - In one Temple chain the victims became the attackers by Windhelm, which
    breaks the "who did the central deed" rule.
  - Its Sanguine chains thin out to "a book-and-coin racket".

  It kept "by her own door" in an Arcadia chain, which Haiku lost. The alias
  resolves to `z-ai/glm-5.3-flash`, which is now the default. The provider
  the alias was routed to allowed reasoning off. Called by its own name, the
  model goes to a provider that requires reasoning. With `effort: low` it used
  1–220 reasoning tokens, cost $0.00007 a call and took 3 s, and three
  chains read the same as before (`panel-20261030-075253.md`).
- **Muse Spark** keeps the facts best. It read Sanguine's ledger correctly, and
  the Temple and Arcadia chains stayed on the event. Its numbers inflate
  pleasantly along a chain: 300 gold becomes 400, then 600, then "near a
  thousand". Its faults:
  - Every telling is one long sentence of comma-joined clauses.
  - The openers alternate "I heard" and "Word is" mechanically.
  - It mistook the Company for the Companions once, and changed the Breton's
    sex once.

  The endpoint refuses `reasoning: {enabled: false}`, so each call spends
  1,000–1,700 reasoning tokens. That makes it twice Haiku's price and four
  times Gemma's, and about 9 s a call.
- **Nemotron** makes up new events. A temple healing became a rescue from a
  burning temple, and Arcadia's scandal became a drunk fouling himself. It
  wrote "the East India Company", and once the teller became the shopkeeper
  ("my wares"). It is also a free endpoint with rate limits.
- **Aion** ignored `effort: low` and ran out of tokens while still reasoning.
  It is out on cost and speed before its quality matters.

Two more lessons for the engine:

- **Reasoning is SkyrimNet's setting, not ours.** Some endpoints reject
  `enabled: false`, so the variant's reasoning should be none or low.
  SkyrimNet's variant config already holds that. Offline, the panel turns
  reasoning off, and drops to low when an endpoint refuses.
- **Accept unnumbered output.** Nemotron writes one paragraph per step without
  numbers. The parser now falls back to paragraphs when there are exactly as
  many paragraphs as steps.

## Changes made to ADR-001

All six changes below were accepted and folded into ADR-001 on 2026-10-30, with
GLM 5.3 Flash as the default retell model. A seventh came out of reading the
chains: "is it true" tellings need a code check that the version still names
the listener.

1. **The hop rate goes with n²,** with separate decays within a place (0.9) and
   between places (0.6). A hop to another town takes travel time even through
   a guild member.
2. **Travelling trades** (inn bards, carriage drivers, caravans) are always
   travel links, never mills. An institution overlapping one place mill by 80%
   or more is dropped.
3. **Enclave mills:** a non-hostile place outside any settlement with 4+ named
   residents is a mill.
4. **The mill strategy is merged at floor 4,** about 53 mills on vanilla.
5. **Generic placements count, after enable parents.** Generic NPCs count once
   per placement, and enable-parent state decides what exists.
6. **Retelling prompt:** numbered-line output, with a paragraph fallback.
   Reasoning stays in SkyrimNet's config, set to none or low.
   `z-ai/glm-5.3-flash` is the default retell model. Gemma 4 31B, DeepSeek V4
   Flash and Claude Haiku 5.5 are the alternatives.

## Not covered

- **Interest by topic** is off (`--interest-spread 0`). It needs real topic
  tags from the seed writer before it can be judged.
- **Overheard arrivals** (listeners carrying an airing home) are not
  simulated. They only add spread near the player.
- **The seed writer and the screening template** weren't tested. They belong
  to stage 4, and need real harvest candidates.
- **Mod-added NPCs and locations** weren't included. The builder only reads
  the vanilla masters.
