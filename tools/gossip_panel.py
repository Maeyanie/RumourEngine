#!/usr/bin/env python3
"""Stage 5 panel: how readily someone gossips, read off their SkyrimNet bio by a decision model.

Reads bio files the way SkyrimNet layers them (a generated .dynamic.prompt over the authored
one), builds the same state and question the DLL's decision template does, and asks a System One
endpoint. Prints one row per person and variant.

    python gossip_panel.py --base <skyrimnet.base>/prompts/characters --save <saves/id>/prompts/characters
    python gossip_panel.py ... --variants blocks --names hulda,irileth
    python gossip_panel.py ... --previous        # the re-rating test: show an earlier score

Nothing here is read by the DLL.
"""
import argparse
import json
import re
import sys
import urllib.request
from pathlib import Path

PANEL = [
    # name prefix, what one would expect: H(igh) M(id) L(ow)
    ("hulda", "H"), ("belethor", "H"), ("mikael", "H"), ("brenuin", "H"), ("corpulus_vinius", "H"),
    ("carlotta_valentia", "H"), ("keerava", "H"), ("haelga", "M"), ("nazeem", "H"), ("sven", "H"),
    ("cicero", "H"), ("heimskr", "M"), ("ysolda", "M"), ("arcadia", "M"), ("adrianne_avenicci", "M"),
    ("danica_pure-spring", "M"), ("fralia_gray-mane", "M"), ("olfina_gray-mane", "M"), ("camilla_valerius", "M"),
    ("lucan_valerius", "M"), ("orgnar", "L"), ("faendal", "M"), ("ahlam", "M"), ("idgrod_the_younger", "M"),
    ("jenassa", "L"), ("balgruuf_the_greater", "L"), ("lydia", "L"), ("irileth", "L"), ("farengar_secret-fire", "L"),
    ("eorlund_gray-mane", "L"), ("vilkas", "L"), ("kodlak_whitemane", "L"), ("delphine", "L"), ("esbern", "L"),
    ("arngeir", "L"), ("maven_black-briar", "L"), ("astrid", "L"), ("elenwen", "L"), ("mercer_frey", "L"),
    ("brynjolf", "M"), ("sapphire", "L"), ("general_tullius", "L"), ("septimus_signus", "L"), ("angi", "L"),
    ("nurelion", "L"), ("froki_whetted-blade", "L"),
]

LEVELS = [
    "Tight-lipped: keeps what they know to themselves, and dislikes idle talk.",
    "Discreet: says little about other people's business unless there's a reason to.",
    "Ordinary: passes on news like anyone would, without going looking for it.",
    "Talkative: enjoys the local talk and readily passes on what they hear.",
    "Incorrigible gossip: collects every story going and tells it to anyone who will listen.",
]

QUESTIONS = {
    "plain": "How readily does {name} pass on news and gossip about other people to those around them? "
             "Judge by who they are (`person`): how fond of talk, how discreet, and what their work brings them to hear.",
    "habit": "Day to day, how much of a gossip is {name} (`person`)? This is about their habit of talking about "
             "other people's doings, not about whether they are friendly, loud or well informed. A secretive "
             "schemer, a soldier under orders and a recluse are at the low end even if they know a great deal.",
}

BLOCK_SETS = {
    "summary": ["summary"],
    "sum+pers": ["summary", "personality"],
    "sum+pers+speech": ["summary", "personality", "speech_style"],
    "all5": ["summary", "personality", "occupation", "skills", "speech_style"],
}

BLOCK = re.compile(r"\{%\s*block\s+(\w+)\s*%\}(.*?)\{%\s*endblock\s*%\}", re.S)


def blocks(text):
    return {m.group(1): " ".join(m.group(2).split()) for m in BLOCK.finditer(text)}


def find_bio(prefix, base, save, static_only=False):
    """(path, is_dynamic) of the bio SkyrimNet would use: the save's generated one first."""
    pat = re.compile(re.escape(prefix) + r"_[0-9A-Fa-f]{3}\.")
    roots = []
    if save and not static_only:
        roots.append((save / "dynamic", ".dynamic.prompt", True))
    if save:
        roots.append((save, ".prompt", False))
    roots.append((base, ".prompt", False))
    for root, suffix, dyn in roots:
        if not root.is_dir():
            continue
        for f in sorted(root.iterdir()):
            if f.name.endswith(suffix) and pat.match(f.name) and (dyn or not f.name.endswith(".dynamic.prompt")):
                return f, dyn
    return None, False


def ask(url, model, state, question, timeout=120):
    body = {"model": model, "state": state, "questions": {"g": {"type": "score", "instructions": question, "criteria": LEVELS}}}
    req = urllib.request.Request(url, json.dumps(body).encode(), {"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        a = json.load(r)["answers"]["g"]
    return a["score"], a.get("confidence", 0.0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", required=True, type=Path)
    ap.add_argument("--save", type=Path)
    ap.add_argument("--url", default="http://192.168.1.2:11434/v1/systemone")
    ap.add_argument("--model", default="d3")
    ap.add_argument("--names", help="comma-separated prefixes; default the whole panel")
    ap.add_argument("--variants", choices=["blocks", "questions"], help="compare block sets or question wordings")
    ap.add_argument("--blocks", default="sum+pers+speech", choices=list(BLOCK_SETS))
    ap.add_argument("--question", default="habit", choices=list(QUESTIONS))
    ap.add_argument("--previous", action="store_true", help="re-rate with an earlier score shown: its own, one level up, one down")
    ap.add_argument("--static", action="store_true", help="ignore generated bios")
    a = ap.parse_args()

    panel = PANEL
    if a.names:
        want = a.names.split(",")
        panel = [(n, dict(PANEL).get(n, "?")) for n in want]

    if a.variants == "blocks":
        cols = [(k, k, a.question) for k in BLOCK_SETS]
    elif a.variants == "questions":
        cols = [(k, a.blocks, k) for k in QUESTIONS]
    else:
        cols = [("score", a.blocks, a.question)]

    head = f"{'who':24} exp dyn " + " ".join(f"{c[0]:>15}" for c in cols)
    if a.previous:
        head += f" {'prev=same':>10} {'prev=+1':>8} {'prev=-1':>8}"
    print(head)
    by_exp = {}
    for prefix, exp in panel:
        path, dyn = find_bio(prefix, a.base, a.save, a.static)
        if not path:
            print(f"{prefix:24} (no bio)")
            continue
        b = blocks(path.read_text(encoding="utf-8"))
        name = prefix.split("_")[0].capitalize()
        row = f"{prefix:24} {exp:>3} {'y' if dyn else ' ':>3} "
        first = None
        for _, bset, q in cols:
            person = "\n".join(b[k] for k in BLOCK_SETS[bset] if b.get(k))
            s, conf = ask(a.url, a.model, {"person": f"{name}. {person}"}, QUESTIONS[q].format(name=name))
            first = s if first is None else first
            row += f" {s:6.2f} ({conf:4.2f}) "
            by_exp.setdefault((exp, _), []).append(s)
        if a.previous:
            _, bset, q = cols[0]
            person = "\n".join(b[k] for k in BLOCK_SETS[bset] if b.get(k))
            for shift in (0, 1, -1):
                lvl = min(4, max(0, round(first) + shift))
                state = {"person": f"{name}. {person}",
                         "earlier_rating": f"Rated before, from an earlier description of {name}: {LEVELS[lvl]}"}
                q2 = QUESTIONS[q].format(name=name) + " `earlier_rating` is what was thought before; keep it unless the description now says otherwise."
                s, _c = ask(a.url, a.model, state, q2)
                row += f" {s:8.2f}"
        print(row, flush=True)
    print()
    for (exp, col), v in sorted(by_exp.items()):
        print(f"expected {exp} {col:>16}: mean {sum(v) / len(v):.2f}  min {min(v):.2f}  max {max(v):.2f}  n {len(v)}")


if __name__ == "__main__":
    sys.exit(main())
