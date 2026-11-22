"""
Compare the mills RumourEngine.dll built in game with the stage 0 builder's,
run on the same load order.

  python extract.py --profile 5
  python mills.py --world p5 --strategy merged --name p5
  python compare_mills.py "C:/Users/image/Documents/My Games/Skyrim.INI/SKSE/RumourEngine_Mills.json" cache/mills_p5.json

Members are compared by NPC editor ID, so the two numbering schemes don't
matter. Generic NPCs repeat, so each mill's members are a multiset.
"""

import collections
import json
import sys


def load(path):
    with open(path, encoding="utf-8") as f:
        d = json.load(f)
    names = {k: p["name"] for k, p in d["persons"].items()}
    mills = {m: collections.Counter(names.get(k, "?" + k) for k in v["members"]) for m, v in d["mills"].items()}
    kinds = {m: v["kind"] for m, v in d["mills"].items()}
    edges = {(e["a"], e["b"], e["kind"]) for e in d["edges"]}
    return mills, kinds, edges


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)
    a, ak, ae = load(sys.argv[1])
    b, bk, be = load(sys.argv[2])
    print("mills: %d in game, %d from stage 0" % (len(a), len(b)))
    only_a, only_b = sorted(set(a) - set(b)), sorted(set(b) - set(a))
    if only_a:
        print("only in game:", ", ".join("%s (%d)" % (m, sum(a[m].values())) for m in only_a))
    if only_b:
        print("only from stage 0:", ", ".join("%s (%d)" % (m, sum(b[m].values())) for m in only_b))
    same = 0
    for m in sorted(set(a) & set(b)):
        if a[m] == b[m] and ak[m] == bk[m]:
            same += 1
            continue
        plus, minus = a[m] - b[m], b[m] - a[m]
        print("%-28s game %3d, stage 0 %3d%s" % (m, sum(a[m].values()), sum(b[m].values()),
                                               "" if ak[m] == bk[m] else "  kind %s vs %s" % (ak[m], bk[m])))
        if plus:
            print("    + " + ", ".join("%s%s" % (n, "" if c == 1 else " x%d" % c) for n, c in sorted(plus.items())))
        if minus:
            print("    - " + ", ".join("%s%s" % (n, "" if c == 1 else " x%d" % c) for n, c in sorted(minus.items())))
    print("identical mills: %d" % same)
    print("edges: %d in game, %d from stage 0, %d in both" % (len(ae), len(be), len(ae & be)))


if __name__ == "__main__":
    main()
