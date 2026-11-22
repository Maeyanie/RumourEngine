#!/usr/bin/env python3
"""Quest panel: can a decision model tell quest endings people would hear about from private ones?

Reads the journal of about 60 vanilla quest endings straight from the masters (log entries from their
strings in Skyrim - Interface.bsa) and asks a System One endpoint two questions about each:

    heard   will word of how it ended get out beyond the people who took part?
    far     will people in other holds hear of it too?

The state is the quest's name and type and its journal: the first entry, which sets the scene, and the
ending's entry (only the ending when it already restates the first, as many do). Each ending carries the
label I'd give it: wide (news across holds), local (talked of where it happened), private, or ? for a
judgement call. The summary counts what each threshold on `heard` would let through, by label.

    python quest_panel.py --openrouter                      # typesafe/jev-1.14, as SkyrimNet sends it
    python quest_panel.py --model d3 --url http://host:11434/v1/systemone
    python quest_panel.py --list                            # the journals, no calls
    python quest_panel.py --openrouter --no-type            # without the quest type, to see what it adds

--openrouter uses OpenRouter's decisions endpoint with the key in OPENROUTER_API_KEY.

Nothing here is read by the DLL.
"""
import argparse
import concurrent.futures
import json
import os
import re
import struct
import urllib.request

import esm
import lstrings

QUESTIONS = {
    "heard": ("`journal` is the player's own record of `quest`: how it began and how it ended. Will word of how it "
              "ended get out beyond the people who took part, so that ordinary folk around there talk about it?",
              {"true": "Yes: it happened in the open, changed something people can see, or those involved will tell of it.",
               "false": "No: it stays with the people who took part, or is kept secret."}),
    "far": ("`journal` is the player's own record of `quest`: how it began and how it ended. Will people in other "
            "holds of Skyrim hear how it ended and talk about it too?",
            {"true": "Yes: it is news well beyond the hold where it happened.",
             "false": "No: if anyone talks of it, it is only near where it happened."}),
}

TYPES = {1: "main", 2: "College of Winterhold", 3: "Thieves Guild", 4: "Dark Brotherhood", 5: "Companions",
         7: "Daedric", 8: "side", 9: "Civil War", 10: "Dawnguard", 11: "Dragonborn"}

# (editor ID, which completion entry, label, extras). Extras: "first" picks the opening entry, "name"
# replaces the quest's name, "alias" fills <Alias=...> tags as the game would.
PANEL = [
    ("MQ104", 0, "wide", {}),
    ("MQ305", 0, "wide", {}),
    ("DB11", 0, "wide", {}),
    ("DB05", 0, "wide", {}),
    ("CWObj", 0, "wide", {"first": 0, "name": "The Battle for Windhelm"}),
    ("CWObj", 1, "wide", {"first": 1, "name": "The Battle for Solitude"}),
    ("DLC1RH07", 1, "wide", {"alias": {"QuestGiver": "Isran", "Capital": "Markarth", "Jarl": "Jarl Igmund",
                                       "PronounPosObj=Jarl": "his"}}),
    ("C04", 0, "wide", {}),
    ("MQ105", 0, "?", {}),
    ("DA16", 0, "local", {}),
    ("DA16", 1, "?", {}),
    ("DA16", 2, "?", {}),
    ("MS11", 0, "local", {}),
    ("MS11", 1, "local", {}),
    ("MS02", 0, "local", {}),
    ("MS02", 1, "local", {}),
    ("MS01", 0, "local", {}),
    ("MS14", 0, "local", {}),
    ("MS05", 0, "local", {}),
    ("DB01", 0, "local", {}),
    ("MS06", 0, "local", {}),
    ("MS10", 0, "local", {}),
    ("MS09", 0, "local", {}),
    ("MS08", 0, "local", {}),
    ("MG08", 0, "local", {}),
    ("MG05", 0, "local", {}),
    ("DLC2RR01", 0, "local", {}),
    ("DLC2MQ03B", 0, "local", {}),
    ("C06", 0, "local", {}),
    ("DBDestroy", 0, "local", {}),
    ("CWFortSiegeFort", 0, "local", {"alias": {"Fort": "Fort Greymoor", "FieldCO": "Legate Rikke"}}),
    ("DA05", 0, "?", {}),
    ("MS08", 1, "?", {}),
    ("DLC2MQ06", 0, "?", {}),
    ("DLC2MH01", 0, "?", {}),
    ("DA14", 0, "?", {}),
    ("MS07", 0, "?", {}),
    ("DA06", 0, "?", {}),
    ("DB10", 0, "?", {}),
    ("DBSideContract01", 0, "?", {}),
    ("DA05", 1, "private", {}),
    ("TG01", 0, "private", {}),
    ("TG02", 0, "private", {}),
    ("TG08B", 0, "private", {}),
    ("TGLeadership", 0, "private", {}),
    ("TGRFO", 0, "private", {"alias": {"Business": "Belethor's General Goods", "City": "Whiterun"}}),
    ("DA01", 0, "private", {}),
    ("DA01", 1, "private", {}),
    ("DA03", 0, "private", {}),
    ("DA15", 0, "private", {}),
    ("DA10", 0, "private", {}),
    ("DA11", 0, "private", {}),
    ("MS13", 0, "private", {}),
    ("MGRitual04", 0, "private", {}),
    ("NN01", 0, "private", {}),
    ("dunHunterQST", 0, "private", {}),
    ("dunVolunruudQST", 0, "private", {}),
    ("MGRArniel04", 0, "private", {}),
    ("DLC2EbonyWarriorQuest", 0, "private", {"alias": {"StartCity": "Whiterun"}}),
    ("MGRAppJzargo01", 0, "private", {}),
]
LABELS = ["wide", "local", "?", "private"]

OPENROUTER = "https://openrouter.ai/api/alpha/decisions"


def read_quests(data_dir):
    """{editor ID: quest} for the typed quests of Skyrim, Dawnguard and Dragonborn."""
    out = {}
    for name in ("Skyrim.esm", "Dawnguard.esm", "Dragonborn.esm"):
        strings = lstrings.table(name, "strings", data_dir=data_dir)
        dl = lstrings.table(name, "dlstrings", data_dir=data_dir)
        plugin = esm.Plugin(os.path.join(data_dir, name), esm.VANILLA)
        for _, _, _, d, _ in plugin.records({b"QUST"}):
            q = {"edid": "", "name": "", "type": 0, "entries": []}
            stage, entry = None, None
            for st, sd in esm.subrecords(d):
                if st == b"EDID":
                    q["edid"] = esm.zstr(sd)
                elif st == b"FULL" and len(sd) == 4:
                    q["name"] = strings.get(esm.u32(sd), "")
                elif st == b"DNAM" and len(sd) > 8:
                    q["type"] = sd[8]
                elif st == b"INDX":
                    stage = struct.unpack_from("<H", sd)[0]
                elif st == b"QSDT":
                    entry = {"stage": stage, "flags": sd[0], "text": ""}
                    q["entries"].append(entry)
                elif st == b"CNAM" and entry is not None and len(sd) == 4:
                    entry["text"] = dl.get(esm.u32(sd), "").strip()
                elif st == b"QOBJ":
                    entry = None
            if q["edid"]:
                out[q["edid"]] = q
    return out


def fill(text, aliases):
    """Fill <Alias=Jarl> and <Alias.PronounPosObj=Jarl> tags, keyed "Jarl" and "PronounPosObj=Jarl"."""
    def sub(m):
        inner = m.group(1)
        k = inner[1:] if inner.startswith("=") else inner.lstrip(".")
        return aliases.get(k, m.group(0))
    return re.sub(r"<alias([^>]*)>", sub, text, flags=re.I)


def build(quests, no_type):
    rows = []
    for edid, which, label, extra in PANEL:
        q = quests[edid]
        alias = extra.get("alias", {})
        told = [e for e in q["entries"] if e["text"]]
        opening = [e for e in told if not e["flags"] & 3]
        endings = [e for e in told if e["flags"] & 1]
        first = fill(opening[extra.get("first", 0)]["text"], alias) if opening else ""
        end = fill(endings[which]["text"], alias)
        journal = end if not first or end.startswith(first[:40]) else first + "\n...\n" + end
        name = extra.get("name") or fill(q["name"], alias)
        quest = name if no_type else "%s (a %s quest)" % (name, TYPES.get(q["type"], "side"))
        rows.append({"tag": "%s/%d" % (edid, which), "label": label, "quest": quest, "journal": journal})
    return rows


def ask(url, model, setting, row, key=None):
    state = {"quest": row["quest"], "journal": row["journal"]}
    if setting:
        state = {"setting": setting, **state}
    body = {"model": model, "state": state,
            "questions": {k: {"type": "noul", "instructions": q, "criteria": c} for k, (q, c) in QUESTIONS.items()}}
    headers = {"Content-Type": "application/json"}
    if key:
        headers["Authorization"] = "Bearer " + key
    req = urllib.request.Request(url, json.dumps(body).encode(), headers)
    with urllib.request.urlopen(req, timeout=180) as r:
        answers = json.load(r)["answers"]
    return {k: answers[k]["noul"] for k in QUESTIONS}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data", default=esm.SKYRIM_DATA, help="the game's Data folder")
    ap.add_argument("--setting", help="the player's 0010_setting.prompt, as a template would include it")
    ap.add_argument("--openrouter", action="store_true", help="ask typesafe/jev-1.14 on OpenRouter's decisions endpoint")
    ap.add_argument("--url", default="http://192.168.1.2:11434/v1/systemone")
    ap.add_argument("--model", help="default d3, or typesafe/jev-1.14 with --openrouter")
    ap.add_argument("--no-type", action="store_true", help="leave the quest type out of the state")
    ap.add_argument("--list", action="store_true", help="print the panel's journals and stop")
    ap.add_argument("--jobs", type=int, default=4, help="requests at once")
    a = ap.parse_args()

    rows = build(read_quests(a.data), a.no_type)
    if a.list:
        for r in rows:
            print("== %s [%s] %s\n%s\n" % (r["tag"], r["label"], r["quest"], r["journal"]))
        return
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

    with concurrent.futures.ThreadPoolExecutor(a.jobs) as pool:
        answers = list(pool.map(lambda r: ask(a.url, a.model, setting, r, key), rows))
    for r, ans in zip(rows, answers):
        r.update(ans)

    print("By heard (%s%s):" % (a.model, ", no type" if a.no_type else ""))
    print("heard  far   label    quest")
    for r in sorted(rows, key=lambda r: -r["heard"]):
        end = r["journal"].split("\n...\n")[-1]
        print("%.2f  %.2f  %-7s  %-28.28s %-18.18s %.70s" % (r["heard"], r["far"], r["label"], r["quest"], r["tag"], end))
    print("\nWhat a threshold on heard lets through, by label:")
    totals = {l: sum(1 for r in rows if r["label"] == l) for l in LABELS}
    print("  heard >=  " + "  ".join("%-9s" % ("%s/%d" % (l, totals[l])) for l in LABELS))
    for t in (0.3, 0.4, 0.5, 0.6, 0.7, 0.8):
        kept = {l: sum(1 for r in rows if r["label"] == l and r["heard"] >= t) for l in LABELS}
        print("  %.1f       " % t + "  ".join("%-9d" % kept[l] for l in LABELS))
    print("\nfar, mean by label: " + ", ".join(
        "%s %.2f" % (l, sum(r["far"] for r in rows if r["label"] == l) / max(1, totals[l])) for l in LABELS))


if __name__ == "__main__":
    main()
