# Stage 0 tools

Offline prototypes for [ADR-001](../docs/ADR-001-rumour-mills.md). Results and
conclusions are in [stage0-results.md](../docs/stage0-results.md). Python 3.12,
standard library only.

| Script | What it does |
| --- | --- |
| `extract.py` | Reads the vanilla masters into `cache/world.pkl` (about 3 s). Run it first. `--profile 5` reads MO2 profile 5's whole load order into `cache/world-p5.pkl` instead (about 7 s). |
| `mills.py` | Builds mills under each strategy, writes `cache/mills_<strategy>.json`, and prints coverage and sizes. `-v` lists every mill; `--floor N --name X` builds a variant; `--world p5` reads a profile's cache; `--kid 5` applies that profile's KID location keywords (see `kid.py`). |
| `compare_mills.py` | Compares the mills the DLL built in game (`RumourEngine_Mills.json` in the SKSE log folder) with this builder's, by mill and member editor ID. |
| `hopsim.py` | Simulates rumours hopping between mills around a wandering player. It reports spread by notability, sameness, who has heard it, and retell calls (lazy against eager). Every rate is a flag; `--no-weighting` turns per-person knowledge off. |
| `gossip_panel.py` | Reads how much of a gossip each of 46 vanilla NPCs is off their SkyrimNet bio, with the question and levels the DLL's `gossip_score` decision template uses, on a System One endpoint (`--url`, `--model`; a local `d3` by default). `--variants blocks` or `questions` compares what is shown or asked, `--previous` shows an earlier reading as well, and `--static` ignores generated bios. The neutral band in `Gossip.cpp` comes from its output. |
| `lasting_panel.py` | Asks a System One endpoint the question the DLL's `lasting` decision template asks (will people still bring it up a year from now?) of every rumour in a save's database, opened read-only, plus a yardstick of made-up events, and prints them by the answer with what `promote.lasting` (`--keep`) would keep. `--openrouter` asks `typesafe/jev-1.14` as SkyrimNet does. `promote.lasting` 0.4 comes from its output on Jev, and the setting's description gives the equivalents for other models. |
| `quest_panel.py` | Reads about 60 vanilla quest endings' journal entries from the masters and asks a System One endpoint whether word of each would get out (`heard`) and reach other holds (`far`), beside the label I'd give it (wide, local, private or ?), with what each threshold on `heard` lets through. `--openrouter` asks Jev as SkyrimNet does, `--no-type` leaves the quest type out, `--list` prints the journals without asking. |
| `panel/retell_panel.py` | Sends the seeds in `panel/seeds.json` down their mill paths through candidate models on OpenRouter. It scores drift and writes `panel/results/panel-<time>.md` for reading. `--dry-run` prints a prompt without calling anything. Reasoning is off; an endpoint that refuses gets `low` automatically, or pass `--reasoning low`. |
| `panel/seed_panel.py` | Rebuilds real `rumour_seed` inputs from the memories RumourEngine seeded on a save (both databases opened read-only), laid out as `Harvest.cpp` lays them out, and asks candidate models on OpenRouter to write the first tellings with the current prompt. It flags quotation marks and names found in no input, and writes `panel/results/seed-<time>.md` beside each memory's in-game telling. The results hold the save's memories, so `.gitignore` keeps them out. `--list` prints the inputs without calling anything. |

`esm.py` is the plugin reader the first two share, and `quest_panel.py` too. `lstrings.py` reads the masters' localized string tables, loose or from `Skyrim - Interface.bsa`.

```bash
python extract.py && python mills.py && python hopsim.py -v
```

To check the DLL's mills against this builder on the same load order:

```bash
python extract.py --profile 5 && python mills.py --world p5 --kid 5 --strategy merged --name p5kid
python compare_mills.py "C:/Users/image/Documents/My Games/Skyrim.INI/SKSE/RumourEngine_Mills.json" cache/mills_p5kid.json
```

```bash
python panel/retell_panel.py --models google/gemma-4-31b-it --seeds helgen -n 3
```

The panel's OpenRouter key comes from the debugging proxy's loader, at
`N:\build\openrouter-proxy\replay.py` (environment, then its `.env`). The key
is never printed.
