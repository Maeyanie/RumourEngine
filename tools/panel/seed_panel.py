"""
Seed panel: rebuild real rumour_seed inputs from a save and ask candidate models
to write the first tellings, for choosing the rumour_seed variant's model.

  python seed_panel.py --list                  # print the rebuilt inputs, no calls
  python seed_panel.py                         # default panel, 18 memories in batches of 3
  python seed_panel.py --models ~z-ai/glm-flash-latest -n 30 --batch 2

Inputs come from the memories RumourEngine seeded on this save (its database's
`screened` rows with verdict `seeded`, and `own` rows for the owner's other
memories of the same happening), with the memory, the owner's name and the
people SkyrimNet linked to it from SkyrimNet's database, and the owner's mill
line from mills.ini, FactionEngine's *_mills.ini, or the default for its kind.
Both databases are opened read-only. The block is laid out as Harvest.cpp lays
it out. The system part is the current rumour_seed prompt, with the setting
submodule as SkyrimNet last rendered it (taken from its OpenRouter input log).

Checks per telling, for reading the report rather than as a score:
  quoted    quotation marks, "I heard" or "they say" (the prompt forbids them)
  new names capitalised words in no input of that memory (possible inventions)
  words     length; the prompt asks for one to three plain sentences

The memories are the player's own save, so results go to
results/seed-<time>.md and .json, which .gitignore keeps out of the repository.
The API key comes from the OpenRouter proxy's loader and is never printed.
"""

import argparse
import configparser
import glob
import json
import os
import re
import sqlite3
import statistics
import sys
import time
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
PROXY = r"N:\build\openrouter-proxy"
URL = "https://openrouter.ai/api/v1/chat/completions"
MO2 = r"G:\MO2Data\SkyrimSE"
RE_DATA = MO2 + r"\overwrite\SKSE\Plugins\RumourEngine\data"
SN_DATA = MO2 + r"\overwrite\SKSE\Plugins\SkyrimNet\data"
SN_LOGS = MO2 + r"\overwrite\SKSE\Plugins\SkyrimNet\logs"
MILLS_JSON = os.path.expanduser(r"~\Documents\My Games\Skyrim.INI\SKSE\RumourEngine_Mills.json")
PROMPT = os.path.join(REPO, r"SKSE\Plugins\SkyrimNet\external\maeyanie.rumourengine\prompts\rumour_seed.prompt")
PLAYER_FORM = 0x14
MAX_SUBJECTS = 8  # Harvest.cpp kMaxSubjects

# model -> reasoning effort (None leaves it out). Muse runs at medium as the user's config has it.
DEFAULT_MODELS = {
    "meta/muse-spark-1.3-contributor": "medium",
    "~z-ai/glm-flash-latest": "low",
    "~z-ai/glm-latest": "low",
    "~deepseek/deepseek-flash-latest": "low",
    "google/gemma-4-31b-it": None,
    "~anthropic/claude-haiku-latest": None,
    "qwen/qwen3-235b-a22b-2507": None,
    "~openai/gpt-luna-latest": "low",
}

EVERYTHING = "everything, juicy details and all"
GIST = "only the gist, without the intimate details, their feelings or confidences"


def one_line(s):
    return s.replace("\r", "").replace("\n", " / ")


# ---- Inputs ----

def latest(pattern):
    files = sorted(glob.glob(pattern), key=os.path.getmtime)
    return files[-1] if files else None


def ro(path):
    return sqlite3.connect("file:%s?mode=ro" % path.replace("\\", "/"), uri=True)


def mill_lines():
    lines = {}
    paths = [os.path.join(REPO, r"SKSE\Plugins\RumourEngine\mills.ini")]
    paths += glob.glob(MO2 + r"\mods\*\SKSE\Plugins\RumourEngine\*_mills.ini")
    for p in paths:
        cp = configparser.ConfigParser(interpolation=None, strict=False, comment_prefixes=(";", "#"))
        cp.optionxform = str.lower
        cp.read(p, encoding="utf-8")
        if cp.has_section("Lines"):
            for k, v in cp.items("Lines"):
                lines.setdefault(k, v.strip())
    return lines


def default_line(mill, info):
    """Retell.cpp DefaultLine, with the mill id standing in for the place's name."""
    p = mill
    kind = (info or {}).get("kind", "")
    if kind == "commons":
        return "%s's townsfolk: the people who live and trade in %s. They care about their neighbours' business and whatever comes in from the road." % (p, p)
    if kind == "court":
        return "%s's court: the Jarl's household, steward and staff. They care about the hold, the war, and what anything means for the Jarl." % p
    if kind == "garrison":
        return "%s's guards: they care about crime, fights and who's armed, and tell everything with a sneer." % p
    if kind == "underworld":
        return "%s's underworld: thieves, fences and smugglers. They care about who's watching and who can be squeezed." % p
    if kind == "enclave":
        return "%s: a closed household of its own. They hear little from outside and judge it by their own concerns." % p
    if kind == "institution":
        return "%s: members spread across several towns, who carry talk between them." % p
    return "%s: the few families of a small place. They care about their own neighbours and whoever passes through." % p


def build_inputs():
    re_db = latest(RE_DATA + r"\RumourEngine-*.db")
    sn_db = os.path.join(SN_DATA, os.path.basename(re_db).replace("RumourEngine-", "SkyrimNet-"))
    rc, sc = ro(re_db), ro(sn_db)
    names = {str(u): n for u, n in sc.execute("select uuid, actor_name from uuid_mappings")}
    player = next((str(u) for (u,) in sc.execute("select uuid from uuid_mappings where form_id = ?", (PLAYER_FORM,))), None)
    lines = mill_lines()
    mills = json.load(open(MILLS_JSON, encoding="utf-8")).get("mills", {}) if os.path.exists(MILLS_JSON) else {}

    items = []
    for rid, origin, text, seeded_at in rc.execute(
            "select id, origin, text, game_time from rumours where source = 'RumourEngine' order by id"):
        seeded = rc.execute("select memory_id, actor, detail from screened where rumour_id = ? and verdict = 'seeded' "
                            "order by memory_id limit 1", (rid,)).fetchone()
        if not seeded:
            continue
        mid, actor, detail = seeded
        row = sc.execute("select content, related_actors from memories where id = ?", (mid,)).fetchone()
        if not row:
            continue
        content, related = row
        own = [r[0] for r in rc.execute("select memory_id from screened where rumour_id = ? and verdict = 'own' and actor = ? "
                                        "order by memory_id", (rid, actor))]
        also = []
        for oid in own:
            o = sc.execute("select content, game_time from memories where id = ?", (oid,)).fetchone()
            # Only what the writer had: not memories made since, such as the owner's memory of
            # telling it ("I'm the one who spread it..."), which holds the telling itself.
            if o and (o[1] is None or o[1] <= seeded_at) and not o[0].startswith("I'm the one who spread it"):
                also.append(one_line(o[0])[:240])
        mill = (json.loads(origin).get("mills") or ["?"])[0]
        line = lines.get(mill) or default_line(mill, mills.get(mill))
        who = names.get(str(actor), "?")
        people = [(who, " (who remembers it)")]
        if player and player != str(actor):
            people.append((names.get(player, "the player"), " (the player)"))
        for u in json.loads(related or "[]"):
            u = str(u)
            if u in (player, str(actor)) or len(people) >= MAX_SUBJECTS:
                continue
            n = names.get(u)
            if n and n not in [p[0] for p in people]:
                people.append((n, ""))
        everything = "tell everything" in (detail or "")
        body = "Who remembers it: %s, one of %s\nTheir memory: %s%s\nHow much they'd tell: %s\nPeople: %s" % (
            who, line, one_line(content), "".join("\nTheir other memory of it: " + t for t in also),
            EVERYTHING if everything else GIST,
            "; ".join("s%d = %s%s" % (k + 1, n, note) for k, (n, note) in enumerate(people)))
        items.append({"rumour": rid, "memory": mid, "owner": who, "mill": mill, "everything": everything,
                      "body": body, "people": [p[0] for p in people], "in_game": text,
                      "default_line": mill not in lines})
    return items, re_db


def system_prompt():
    """The current prompt's system part, with the setting as SkyrimNet last rendered it."""
    src = open(PROMPT, encoding="utf-8").read()
    src = re.sub(r"\{#.*?#\}", "", src, flags=re.S)
    part = src.split("[ system ]", 1)[1].split("[ end system ]", 1)[0].strip()
    logged = None
    for path in sorted(glob.glob(SN_LOGS + r"\openrouter_input*.log*"), key=os.path.getmtime):
        text = open(path, encoding="utf-8", errors="replace").read()
        for m in re.finditer(r"\] \[rumour_seed\] Generate request \[[^\]]+\]:\n(\{.*?\n\})", text, flags=re.S):
            body = json.loads(m.group(1), strict=False)
            logged = next(x["content"] for x in body["messages"] if x["role"] == "system")
    if not logged:
        sys.exit("no rumour_seed request in SkyrimNet's OpenRouter logs to take the setting from")
    head = "before anyone else has retold it."
    tail = "For each memory you are given"
    setting = logged.split(head, 1)[1].split(tail, 1)[0].strip()
    out = re.sub(r"\{\{\s*render_template\([^)]*0010_setting[^)]*\)\s*\}\}", lambda _: setting, part)
    return re.sub(r"\n{3,}", "\n\n", out)


def batches(items, size):
    return [items[i:i + size] for i in range(0, len(items), size)]


def user_part(batch):
    return "\n\n".join("m%d\n%s" % (k + 1, it["body"]) for k, it in enumerate(batch))


# ---- Calls ----

def call(key, model, messages, reasoning, temperature, max_tokens):
    body = {"model": model, "messages": messages, "temperature": temperature, "top_p": 0.95,
            "max_tokens": max_tokens, "usage": {"include": True}}
    if reasoning:
        body["reasoning"] = {"effort": reasoning}
    req = urllib.request.Request(URL, data=json.dumps(body).encode(), headers={
        "Authorization": "Bearer " + key, "Content-Type": "application/json",
        "X-Title": "RumourEngine seed panel"})
    last = None
    for attempt in range(3):
        t0 = time.time()
        try:
            with urllib.request.urlopen(req, timeout=240) as r:
                data = json.loads(r.read().decode("utf-8"))
            if "choices" not in data:
                raise RuntimeError(str(data.get("error", data))[:300])
            return data, time.time() - t0
        except (urllib.error.URLError, RuntimeError, TimeoutError) as e:
            last = e
            if isinstance(e, urllib.error.HTTPError):
                last = RuntimeError("HTTP %d: %s" % (e.code, e.read().decode("utf-8", "replace")[:300]))
            time.sleep(2 + 3 * attempt)
    raise last


def parse(text, want):
    """Harvest.cpp's reading, roughly: blocks headed m<k>, each telling/about/topics or skip."""
    out = {}
    cur = None
    for raw in text.replace("\r", "").split("\n"):
        line = raw.strip().strip("*").strip()
        m = re.fullmatch(r"m(\d+)[:.]?", line, flags=re.I)
        if m:
            cur = int(m.group(1))
            out[cur] = {}
            continue
        if cur is None or not line:
            continue
        if line.lower() == "skip":
            out[cur]["skip"] = True
            continue
        f = re.match(r"(telling|about|topics)\s*:\s*(.*)$", line, flags=re.I)
        if f:
            out[cur][f.group(1).lower()] = f.group(2).strip()
        elif "telling" in out[cur] and "about" not in out[cur]:
            out[cur]["telling"] += " " + line
    return [out.get(k + 1) for k in range(want)]


# ---- Checks ----

WORD = re.compile(r"[A-Za-z][A-Za-z'-]*")
COMMON = set("""a an the and or but of to in on at by for with from that this it its is was were be been he she
they them his her their him i me my you we us our there here as so if not no one some just all what who which when
then than had has have do did does out up off over into about like would could should will can may might after before
while during once later until since because though although yet still also even only never ever every each both
another other others such own same few many much more most less least very too again soon now how why where whose
whom whatever whoever her hers himself herself themselves itself""".split())


def capitalised(s):
    out = set()
    for sent in re.split(r"(?<=[.!?;:])\s+|\s+[—–-]\s+", s):
        toks = WORD.findall(sent)
        for t in toks[1:]:
            if t[0].isupper() and t.lower() not in COMMON:
                out.add(t.strip("'").lower().removesuffix("'s"))
    return out


def checks(item, telling):
    flags = []
    if re.search(r"[\"“”]|\bI heard\b|\bthey say\b|\bword is\b|\brumou?r has it\b", telling, flags=re.I):
        flags.append("quoted")
    known = set(w.lower().removesuffix("'s") for w in WORD.findall(item["body"]))
    new = sorted(n for n in capitalised(telling) if n not in known)
    if new:
        flags.append("new names: " + ", ".join(new))
    return flags


# ---- Run ----

def run_batch(key, model, reasoning, system, batch, bi, a):
    msgs = [{"role": "system", "content": system}, {"role": "user", "content": user_part(batch)}]
    try:
        data, secs = call(key, model, msgs, reasoning, a.temperature, a.max_tokens)
    except Exception as e:
        return {"model": model, "batch": bi, "error": str(e)}
    out = data["choices"][0]["message"].get("content") or ""
    u = data.get("usage", {}) or {}
    answers = parse(out, len(batch))
    res = []
    for it, ans in zip(batch, answers):
        r = {"rumour": it["rumour"], "answer": ans}
        if ans and ans.get("telling"):
            r["words"] = len(WORD.findall(ans["telling"]))
            r["flags"] = checks(it, ans["telling"])
        res.append(r)
    return {"model": model, "batch": bi, "secs": round(secs, 2), "cost": u.get("cost"),
            "prompt": u.get("prompt_tokens"), "completion": u.get("completion_tokens"),
            "reasoning": (u.get("completion_tokens_details") or {}).get("reasoning_tokens"),
            "raw": out, "results": res}


def mean(xs):
    xs = [x for x in xs if x is not None]
    return statistics.mean(xs) if xs else float("nan")


def report(items, runs, models, a, path, system):
    by = {(r["model"], x["rumour"]): x for r in runs if "results" in r for x in r["results"]}
    errs = {r["model"]: r["error"] for r in runs if "error" in r}
    L = ["# Seed panel, %s" % time.strftime("%Y-%m-%d %H:%M"), "",
         "%d memories from this save, %d per call, temperature %.2f, top_p 0.95." % (len(items), a.batch, a.temperature), "",
         "| model | reasoning | answered | skipped | missing | words | quoted | new names | $/call | s/call |",
         "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |"]
    for m in models:
        xs = [by.get((m, it["rumour"])) for it in items]
        told = [x for x in xs if x and x.get("answer") and x["answer"].get("telling")]
        skipped = sum(1 for x in xs if x and x.get("answer") and x["answer"].get("skip"))
        missing = len(items) - len(told) - skipped
        calls = [r for r in runs if r["model"] == m and "results" in r]
        L.append("| %s | %s | %d | %d | %d | %.0f | %d | %d | %.5f | %.1f |" % (
            m, models[m] or "-", len(told), skipped, missing, mean(x["words"] for x in told),
            sum(1 for x in told if "quoted" in x["flags"]),
            sum(1 for x in told if any(f.startswith("new names") for f in x["flags"])),
            mean(r["cost"] for r in calls), mean(r["secs"] for r in calls)))
    if errs:
        L += [""] + ["- **%s** failed a call: %s" % (m, e[:200]) for m, e in errs.items()]
    L += ["", "*New names* are capitalised words found in none of that memory's inputs: often an invention, sometimes "
          "just a place or title the model knows.", ""]
    for it in items:
        L += ["## Rumour %d: %s, %s (%s)" % (it["rumour"], it["owner"], it["mill"], "everything" if it["everything"] else "gist"), "",
              "```", it["body"], "```", "", "- **In game:** %s" % it["in_game"]]
        for m in models:
            x = by.get((m, it["rumour"]))
            if not x or not x.get("answer"):
                L.append("- **%s:** *(no answer)*" % m)
            elif x["answer"].get("skip"):
                L.append("- **%s:** *skip*" % m)
            else:
                ans = x["answer"]
                extra = " *[%s]*" % "; ".join(x["flags"]) if x["flags"] else ""
                L.append("- **%s:** %s%s  \n  about: %s; topics: %s" % (
                    m, ans.get("telling", ""), extra, ans.get("about", "?"), ans.get("topics", "?")))
        L.append("")
    L += ["## The system prompt", "", "```", system, "```"]
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(L))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--models", nargs="*", help="model ids, each optionally @low, @medium or @none for its reasoning "
                                                    "(else as in the default list, or low)")
    ap.add_argument("-n", type=int, default=18, help="memories, spread evenly over the save's seeded ones")
    ap.add_argument("--batch", type=int, default=3, help="memories per call (a sweep sends up to 6)")
    ap.add_argument("--temperature", type=float, default=1.0)
    ap.add_argument("--max-tokens", type=int, default=4096)
    ap.add_argument("--workers", type=int, default=8)
    ap.add_argument("--list", action="store_true", help="print the inputs and the system prompt, no calls")
    a = ap.parse_args()

    items, db = build_inputs()
    if a.n < len(items):
        step = len(items) / a.n
        items = [items[int(i * step)] for i in range(a.n)]
    system = system_prompt()
    models = {}
    for m in a.models or DEFAULT_MODELS:
        mid, _, effort = m.partition("@")  # model@effort picks the reasoning; @none leaves it out
        models[m] = (None if effort == "none" else effort) if effort else DEFAULT_MODELS.get(mid, "low")
    if a.list:
        print("--- system ---\n%s\n" % system)
        for bi, b in enumerate(batches(items, a.batch)):
            print("--- call %d ---\n%s\n" % (bi + 1, user_part(b)))
        print("%d memories from %s; default mill lines for: %s" % (
            len(items), os.path.basename(db), ", ".join(sorted({i["mill"] for i in items if i["default_line"]})) or "none"))
        return

    sys.path.insert(0, PROXY)
    import replay  # noqa: E402  (key loading only)
    key = replay.load_key(None, "OPENROUTER_API_KEY")
    if not key:
        sys.exit("no OpenRouter key found")

    bs = batches(items, a.batch)
    jobs = [(m, r, b, bi) for m, r in models.items() for bi, b in enumerate(bs)]
    with ThreadPoolExecutor(max_workers=a.workers) as ex:
        runs = list(ex.map(lambda j: run_batch(key, j[0].partition("@")[0], j[1], system, j[2], j[3], a), jobs))
    for r, j in zip(runs, jobs):
        r["model"] = j[0]  # keep the @effort label in the report
    os.makedirs(os.path.join(HERE, "results"), exist_ok=True)
    base = os.path.join(HERE, "results", "seed-" + time.strftime("%Y%m%d-%H%M%S"))
    with open(base + ".json", "w", encoding="utf-8") as f:
        json.dump({"args": vars(a), "models": models, "items": items, "runs": runs}, f, indent=1, ensure_ascii=False)
    report(items, runs, models, a, base + ".md", system)
    total = sum(r.get("cost") or 0 for r in runs)
    for m in models:
        rs = [r for r in runs if r["model"] == m]
        ok = sum(1 for r in rs if "results" in r)
        print("%-36s %d/%d calls  %.1fs/call" % (m, ok, len(rs), mean(r.get("secs") for r in rs if "results" in r)))
    print("total cost $%.4f; report %s.md" % (total, base))


if __name__ == "__main__":
    main()
