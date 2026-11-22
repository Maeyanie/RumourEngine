"""
Retell panel (ADR-001, stage 0): run seeded rumours along six-hop mill paths
through candidate cheap models, the way lazy retelling would, and score how
much each step drifts from the one before.

  python retell_panel.py --dry-run                 # print one rendered prompt, no calls
  python retell_panel.py                           # default model panel, every seed
  python retell_panel.py --models google/gemma-4-31b-it --seeds helgen -n 3

Each call writes --steps-per-call tellings along the path, from the last one
written, as one lazy-retelling call writes every step to a settlement.

Scores, per step against the telling it came from:
  word-sequence similarity  about 1 is copying, about 0 is a different story
  content-word overlap      shared nouns and verbs, order ignored
  names kept                share of the seed's proper names still there at the end

The API key comes from the OpenRouter proxy's loader (environment, then its
.env) and is never printed. Results go to results/panel-<time>.md and .json.
"""

import argparse
import difflib
import json
import os
import re
import statistics
import sys
import time
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
PROXY = r"N:\build\openrouter-proxy"
URL = "https://openrouter.ai/api/v1/chat/completions"

# The shortlist after stage 0; the default retell model first. The rejected
# candidates are in docs/stage0-results.md.
DEFAULT_MODELS = [
    "z-ai/glm-5.3-flash",
    "google/gemma-4-31b-it",                    # FactionEngine's rumour model today
    "deepseek/deepseek-v4-flash",
    "anthropic/claude-haiku-5.5",
]

WORD = re.compile(r"[A-Za-z][A-Za-z']*")
STOP = set("""a an the and or but of to in on at by for with from that this it its is was were be been
being he she they them his her their i me my you we us our said says say saying heard hear word
there here as so if not no one some just all what who whom which when then than had has have do
did does out up off over into about like would could should will can may might im it's that's
he's she's they're i'm you're well aye""".split())


def words(s):
    return [w.lower() for w in WORD.findall(s)]


def seq_sim(a, b):
    return difflib.SequenceMatcher(None, words(a), words(b), autojunk=False).ratio()


def content_overlap(a, b):
    x, y = set(words(a)) - STOP, set(words(b)) - STOP
    return len(x & y) / len(x | y) if x | y else 0.0


def proper_names(s):
    """Capitalised words that don't start a sentence: a rough list of names."""
    out = set()
    for sent in re.split(r"(?<=[.!?])\s+", s):
        toks = WORD.findall(sent)
        for t in toks[1:]:
            if t[0].isupper() and t.lower() not in STOP:
                out.add(t.lower().rstrip("'s"))
    return out


def render(system, lines, prev_mill, prev_text, steps):
    """System prompt is identical for every call; the user part goes from shared to specific."""
    groups = []
    for m in [prev_mill] + steps:
        if m not in [g[0] for g in groups]:
            groups.append((m, lines[m]))
    user = ["Groups along the way:"]
    user += ["- %s" % line for _, line in groups]
    user += ["", "The talk so far, as %s tell it:" % first_clause(lines[prev_mill]), prev_text, "",
             "Pass it on through these groups, in order:"]
    user += ["%d. %s" % (i + 1, first_clause(lines[m])) for i, m in enumerate(steps)]
    return [{"role": "system", "content": system}, {"role": "user", "content": "\n".join(user)}]


def first_clause(line):
    """The group's name: the mill line up to its first colon or full stop."""
    return re.split(r"[:.]", line, maxsplit=1)[0].strip()


def call(key, model, messages, temperature, max_tokens, reasoning="off"):
    body = {"model": model, "messages": messages, "temperature": temperature,
            "max_tokens": max_tokens, "usage": {"include": True},
            "reasoning": {"enabled": False} if reasoning == "off" else {"effort": reasoning}}
    req = urllib.request.Request(URL, data=json.dumps(body).encode(), headers={
        "Authorization": "Bearer " + key, "Content-Type": "application/json",
        "X-Title": "RumourEngine retell panel"})
    last = None
    for attempt in range(3):
        t0 = time.time()
        try:
            with urllib.request.urlopen(req, timeout=180) as r:
                data = json.loads(r.read().decode("utf-8"))
            if "choices" not in data:
                raise RuntimeError(str(data.get("error", data))[:300])
            return data, time.time() - t0
        except (urllib.error.URLError, RuntimeError, TimeoutError) as e:
            last = e
            if isinstance(e, urllib.error.HTTPError):
                text = e.read().decode("utf-8", "replace")
                if reasoning == "off" and "Reasoning is mandatory" in text:
                    # Some providers refuse to turn it off; low is the next best.
                    return call(key, model, messages, temperature, max_tokens, "low")
                last = RuntimeError("HTTP %d: %s" % (e.code, text[:300]))
            time.sleep(2 + 3 * attempt)
    raise last


NUMBERED = re.compile(r"^\s*\**(\d+)[.):]\**\s*(.+?)\s*$")


def parse_tellings(text, want):
    """Numbered lines, one per step; stray quotes and markdown are stripped."""
    got = {}
    for line in text.splitlines():
        m = NUMBERED.match(line)
        if m and 1 <= int(m.group(1)) <= want and int(m.group(1)) not in got:
            got[int(m.group(1))] = m.group(2).strip().strip('"“”*').strip()
    if len(got) < want or not all(got.values()):
        # Some models drop the numbers but keep one paragraph per step.
        paras = [p.strip().strip('"“”*').strip() for p in re.split(r"\n\s*\n", text.strip()) if p.strip()]
        return paras if len(paras) == want else None
    return [got[i] for i in range(1, want + 1)]


def run_chain(key, model, seed, lines, system, a, sample):
    path = seed["path"]
    chain = [{"mill": path[0], "text": seed["telling"]}]
    calls = []
    i = 1
    while i < len(path):
        steps = path[i:i + a.steps_per_call]
        msgs = render(system, lines, chain[-1]["mill"], chain[-1]["text"], steps)
        try:
            data, secs = call(key, model, msgs, a.temperature, a.max_tokens, a.reasoning)
        except Exception as e:
            calls.append({"error": str(e)})
            break
        out = data["choices"][0]["message"].get("content") or ""
        u = data.get("usage", {}) or {}
        calls.append({"secs": round(secs, 2), "prompt": u.get("prompt_tokens"),
                      "completion": u.get("completion_tokens"), "cost": u.get("cost"),
                      "cached": (u.get("prompt_tokens_details") or {}).get("cached_tokens"),
                      "reasoning": (u.get("completion_tokens_details") or {}).get("reasoning_tokens"),
                      "raw": out if a.keep_raw else None})
        tellings = parse_tellings(out, len(steps))
        if tellings is None:
            calls[-1]["error"] = "unparseable: " + out[:2000]
            break
        for m, t in zip(steps, tellings):
            chain.append({"mill": m, "text": t})
        i += len(steps)
    return {"model": model, "seed": seed["id"], "sample": sample, "chain": chain, "calls": calls}


def score(run):
    ch = run["chain"]
    steps = []
    for prev, cur in zip(ch, ch[1:]):
        steps.append({"seq": seq_sim(prev["text"], cur["text"]),
                      "content": content_overlap(prev["text"], cur["text"]),
                      "words": len(words(cur["text"]))})
    seed_names = proper_names(ch[0]["text"])
    last = set(words(ch[-1]["text"]))
    kept = len([n for n in seed_names if n in last]) / len(seed_names) if seed_names else None
    run["steps"] = steps
    run["names_kept"] = kept
    run["origin_sim"] = content_overlap(ch[0]["text"], ch[-1]["text"]) if len(ch) > 1 else None
    return run


def mean(xs):
    xs = [x for x in xs if x is not None]
    return statistics.mean(xs) if xs else float("nan")


def summarise(runs, models, a):
    rows = []
    for m in models:
        rs = [r for r in runs if r["model"] == m]
        st = [s for r in rs for s in r["steps"]]
        calls = [c for r in rs for c in r["calls"] if "error" not in c]
        errs = sum(1 for r in rs for c in r["calls"] if "error" in c)
        full = sum(1 for r in rs if len(r["chain"]) == len(next(s for s in SEEDS if s["id"] == r["seed"])["path"]))
        costs = [c["cost"] for c in calls if c.get("cost") is not None]
        cached = sum(c["cached"] or 0 for c in calls)
        prompt = sum(c["prompt"] or 0 for c in calls)
        rows.append({
            "model": m, "chains": "%d/%d" % (full, len(rs)), "errors": errs,
            "seq": mean(s["seq"] for s in st),
            "copy": sum(s["seq"] >= 0.5 for s in st) / len(st) if st else float("nan"),
            "replace": sum(s["content"] <= 0.05 for s in st) / len(st) if st else float("nan"),
            "content": mean(s["content"] for s in st),
            "origin": mean(r["origin_sim"] for r in rs),
            "names": mean(r["names_kept"] for r in rs),
            "words": mean(s["words"] for s in st),
            "cost_call": mean(costs),
            "secs": mean(c["secs"] for c in calls),
            "cached": cached / prompt if prompt else 0.0,
            "reasoning": sum(c.get("reasoning") or 0 for c in calls),
        })
    return rows


def write_report(runs, rows, a, path):
    L = ["# Retell panel, %s" % time.strftime("%Y-%m-%d %H:%M"), "",
         "Temperature %.2f, %d steps per call, %d sample(s) per seed." % (a.temperature, a.steps_per_call, a.n), "",
         "| model | full chains | step similarity | copying | replaced | content overlap | vs seed at end | names kept | words | $/call | s/call | cached |",
         "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |"]
    for r in rows:
        L.append("| %s | %s | %.2f | %.0f%% | %.0f%% | %.2f | %.2f | %.0f%% | %.0f | %.5f | %.1f | %.0f%% |" % (
            r["model"], r["chains"], r["seq"], 100 * r["copy"], 100 * r["replace"], r["content"],
            r["origin"], 100 * r["names"], r["words"], r["cost_call"], r["secs"], 100 * r["cached"]))
    L += ["", "*Step similarity* is word-sequence similarity to the telling it came from (1 = copied).",
          "*Copying* is the share of steps at 0.5 or more; *replaced* the share sharing almost no content words.",
          "*Vs seed at end* is content overlap between the seed and the last telling.", ""]
    by_seed = {}
    for r in runs:
        by_seed.setdefault(r["seed"], []).append(r)
    lines = json.load(open(os.path.join(HERE, "mill_lines.json"), encoding="utf-8"))
    for sid, rs in by_seed.items():
        seed = next(s for s in SEEDS if s["id"] == sid)
        L += ["## %s" % sid, "", "**Seed** (%s, %s): %s" % (seed["owner"], first_clause(lines[seed["path"][0]]),
                                                           seed["telling"]), ""]
        for r in rs:
            L.append("### %s%s" % (r["model"], " #%d" % (r["sample"] + 1) if a.n > 1 else ""))
            L.append("")
            for step, s in zip(r["chain"][1:], r["steps"]):
                L.append("- **%s** (%.2f): %s" % (first_clause(lines[step["mill"]]), s["seq"], step["text"]))
            errs = [c["error"] for c in r["calls"] if "error" in c]
            if errs:
                L.append("- *stopped: %s*" % errs[0][:200].replace("\n", " "))
            L.append("")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(L))


SEEDS = []


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--models", nargs="*", default=DEFAULT_MODELS)
    ap.add_argument("--seeds", nargs="*")
    ap.add_argument("-n", type=int, default=1, help="chains per seed per model")
    ap.add_argument("--steps-per-call", type=int, default=2)
    ap.add_argument("--temperature", type=float, default=1.0)
    ap.add_argument("--max-tokens", type=int, default=900)
    ap.add_argument("--workers", type=int, default=8)
    ap.add_argument("--keep-raw", action="store_true")
    ap.add_argument("--reasoning", default="off", choices=["off", "minimal", "low", "medium"],
                    help="off for most models; some endpoints refuse to turn it off")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()

    global SEEDS
    SEEDS = json.load(open(os.path.join(HERE, "seeds.json"), encoding="utf-8"))["seeds"]
    if a.seeds:
        SEEDS = [s for s in SEEDS if s["id"] in a.seeds]
    lines = json.load(open(os.path.join(HERE, "mill_lines.json"), encoding="utf-8"))
    system = open(os.path.join(HERE, "rumour_retell.txt"), encoding="utf-8").read().strip()

    if a.dry_run:
        s = SEEDS[0]
        for msg in render(system, lines, s["path"][0], s["telling"], s["path"][1:1 + a.steps_per_call]):
            print("--- %s ---\n%s\n" % (msg["role"], msg["content"]))
        return

    sys.path.insert(0, PROXY)
    import replay  # noqa: E402  (key loading only)
    key = replay.load_key(None, "OPENROUTER_API_KEY")
    if not key:
        sys.exit("no OpenRouter key found")

    jobs = [(m, s, k) for m in a.models for s in SEEDS for k in range(a.n)]
    with ThreadPoolExecutor(max_workers=a.workers) as ex:
        runs = list(ex.map(lambda j: score(run_chain(key, j[0], j[1], lines, system, a, j[2])), jobs))
    os.makedirs(os.path.join(HERE, "results"), exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    base = os.path.join(HERE, "results", "panel-" + stamp)
    with open(base + ".json", "w", encoding="utf-8") as f:
        json.dump({"args": vars(a), "runs": runs}, f, indent=1)
    rows = summarise(runs, a.models, a)
    write_report(runs, rows, a, base + ".md")
    total = sum(c.get("cost") or 0 for r in runs for c in r["calls"])
    for r in rows:
        print("%-42s %s  sim %.2f  copy %3.0f%%  repl %3.0f%%  names %3.0f%%  $%.5f/call  %.1fs" % (
            r["model"], r["chains"], r["seq"], 100 * r["copy"], 100 * r["replace"], 100 * r["names"],
            r["cost_call"], r["secs"]))
    print("total cost $%.4f; report %s.md" % (total, base))


if __name__ == "__main__":
    main()
