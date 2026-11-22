#!/usr/bin/env python3
"""Stage 6 panel: which rumours a decision model would keep as world knowledge.

Asks a System One endpoint the question the DLL's `decisions/rumourengine/lasting` template asks
(will people across Skyrim still bring it up a year from now?) of every rumour in a save's
RumourEngine database, read-only, and prints them by the answer beside their notability, so
`promote.lasting` can be set for another model. A few made-up events of known weight are asked too,
as a yardstick.

    python lasting_panel.py --db <overwrite>/SKSE/Plugins/RumourEngine/data/RumourEngine-<save>.db
    python lasting_panel.py --db ... --setting <overlay>/prompts/submodules/system_head/0010_setting.prompt
    python lasting_panel.py --db ... --openrouter            # typesafe/jev-1.14, as SkyrimNet sends it
    python lasting_panel.py --db ... --model d3 --url http://host:11434/v1/systemone --keep 0.75
    python lasting_panel.py --db ... --model kev:4b-q8_0 --keep 0.6    # Ollama wants the full tag

--openrouter uses OpenRouter's decisions endpoint with the key in OPENROUTER_API_KEY.

Nothing here is read by the DLL.
"""
import argparse
import json
import os
import re
import sqlite3
import urllib.request

# Keep in step with prompts/decisions/rumourengine/lasting.prompt.
QUESTION = ("`rumour` went round Skyrim by word of mouth. A year from now, will ordinary people across Skyrim still "
            "remember it and bring it up, the way people remember a war breaking out, a Jarl killed or a dragon slain? "
            "Judge the matter itself, not how juicy the telling is.")
CRITERIA = {"true": "Yes: it will be remembered and talked of across Skyrim for a year or more.",
            "false": "No: within a season it will be old talk, remembered only by those it touched."}

YARDSTICK = [
    ("keep", "Someone murdered Jarl Siddgeir in his own longhouse in Falkreath, and the steward has taken his seat until the Legion names another."),
    ("keep", "A Breton named Rennard killed a dragon outside Whiterun's walls while the guards watched, and the dragon's soul went into him."),
    ("keep", "The Companions' Harbinger Kodlak Whitemane was killed by the Silver Hand in Jorrvaskr, and the Companions have sworn revenge."),
    ("drop", "Rennard and his companions cleared the bandits out of Halted Stream Camp near Whiterun."),
    ("drop", "Ysolda was seen kissing a Khajiit caravan trader behind the Bannered Mare."),
]


OPENROUTER = "https://openrouter.ai/api/alpha/decisions"


def ask(url, model, setting, text, key=None):
    state = {"rumour": text}
    if setting:
        state = {"setting": setting, "rumour": text}
    body = {"model": model, "state": state,
            "questions": {"lasting": {"type": "noul", "instructions": QUESTION, "criteria": CRITERIA}}}
    headers = {"Content-Type": "application/json"}
    if key:
        headers["Authorization"] = "Bearer " + key
    req = urllib.request.Request(url, json.dumps(body).encode(), headers)
    with urllib.request.urlopen(req, timeout=120) as r:
        return json.load(r)["answers"]["lasting"]["noul"]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--db", required=True, help="a save's RumourEngine database (opened read-only)")
    ap.add_argument("--setting", help="the player's 0010_setting.prompt, as the template includes it")
    ap.add_argument("--openrouter", action="store_true", help="ask typesafe/jev-1.14 on OpenRouter's decisions endpoint")
    ap.add_argument("--url", default="http://192.168.1.2:11434/v1/systemone")
    ap.add_argument("--model", help="default d3, or typesafe/jev-1.14 with --openrouter")
    ap.add_argument("--keep", type=float, default=0.4, help="promote.lasting, to count what would be kept")
    ap.add_argument("--min-notability", type=float, default=0.0, help="skip rumours below this")
    a = ap.parse_args()
    key = None
    if a.openrouter:
        key = os.environ.get("OPENROUTER_API_KEY")
        if not key:
            ap.error("--openrouter needs OPENROUTER_API_KEY")
        a.url = OPENROUTER
    a.model = a.model or ("typesafe/jev-1.14" if a.openrouter else "d3")

    setting = ""
    if a.setting:
        setting = re.sub(r"\{#.*?#\}", "", open(a.setting, encoding="utf-8").read(), flags=re.S).strip()
    c = sqlite3.connect("file:" + a.db + "?mode=ro", uri=True)
    rows = c.execute("SELECT id, source, notability, game_time, text FROM rumours WHERE notability >= ? ORDER BY id",
                     (a.min_notability,)).fetchall()
    out = []
    for rid, source, n, day, text in rows:
        p = ask(a.url, a.model, setting, text, key)
        out.append((p, f"#{rid}", source, n, text))
        print(f"{p:.2f}  #{rid:<4} {source[:12]:12} n {n:.2f}  {text[:100]}", flush=True)
    for want, text in YARDSTICK:
        p = ask(a.url, a.model, setting, text, key)
        out.append((p, f"({want})", "yardstick", -1.0, text))

    print(f"\nBy the answer ({a.model}):")
    for p, tag, source, n, text in sorted(out, reverse=True):
        mark = "*" if p >= a.keep else " "
        ns = f"n {n:.2f}" if n >= 0 else "      "
        print(f"{mark} {p:.2f}  {tag:7} {source[:12]:12} {ns}  {text[:110]}")
    real = [o for o in out if o[2] != "yardstick"]
    if real:
        days = [d for _, _, _, d, _ in rows]
        span = max(days) - min(days) if days else 0.0
        kept = sum(1 for o in real if o[0] >= a.keep)
        rate = f", about {kept / span * 30:.1f} a game month" if span > 1 else ""
        print(f"\n{kept} of {len(real)} rumours at {a.keep}{rate} (* marks them).")


if __name__ == "__main__":
    main()
