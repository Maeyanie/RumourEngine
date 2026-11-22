"""
Where one rumour would have spread by a given day: a port of the DLL's Spread::Simulate.

  python spreadsim.py requests.json --now 115.52 --until 122.4
  python spreadsim.py requests.json --now 115.52 --until 122.4 --ids 200   # over 200 rumour ids

`requests.json` is one AddRumour request, or a list of them sent in order at `--now`
(an add, then patches, as a client would). The hop rolls are fixed by the rumour's id,
so `--id` is the id it will get (the database's next one); `--ids N` runs N ids from
there and reports how often each mill is reached instead.

The mills come from the DLL's own dump (debug.dump_mills writes RumourEngine_Mills.json
at every build), so faction mills and the save's enable state are included. Origins by
settlement, hold or mill ids work; actors and locations need the game. Only what the
simulation decides is modelled: a witness's mill, an overheard telling carried home and
a proclaimed rumour's schedule are not.

Gossip scores here come from the built-in job table alone: the game's bio readings and
gossips.ini are not read, so a mill linked through one much-changed person may differ.
A shared member carries a rumour on only once it has reached them (--no-reach turns that
off, as reach.enabled does); an as_fact version, which everyone has heard, is not modelled.

--place and --link try a placement or a mills.ini [Links] line without loading the game.
The spread settings default to the DLL's defaults; pass them (--travel-rate-scale,
--travel-exponent, --decay-travel-per-n, ...) to try other values before changing them
in SkyrimNet's plugin config.
"""

import argparse
import collections
import json
import math
import os
import statistics

MASK = 0xFFFFFFFFFFFFFFFF
DUMP = os.path.join(os.path.expanduser("~"), "Documents", "My Games", "Skyrim.INI", "SKSE", "RumourEngine_Mills.json")

# Spread.cpp
RATE = {"shared": 1.0, "strata": 3.0, "road": 0.6, "carriage": 0.1, "ship": 0.3, "hold": 0.3}
TRAVEL_CELLS_PER_DAY = 40.0
LINK_WEIGHT = {"road": 1.0, "carriage": 0.6, "ship": 0.5, "hold": 0.15, "ears": 1.0}  # as the builder makes them

# Gossip.cpp's built-in jobs
GOSSIP_BY_FACTION = [
    ("JobInnkeeperFaction", 0.9), ("JobInnServer", 0.85), ("JobBardFaction", 0.85),
    ("FavorJobsBeggarsFaction", 0.8), ("FavorJobsDrunksFaction", 0.8), ("JobStreetVendorFaction", 0.75),
    ("JobMerchantFaction", 0.7), ("CarriageSystemFaction", 0.7), ("JobFenceFaction", 0.6),
    ("JobHostlerFaction", 0.6), ("JobStewardFaction", 0.5), ("JobJarlFaction", 0.4),
    ("IsGuardFaction", 0.3), ("JobHousecarlFaction", 0.25), ("JobCourtWizardFaction", 0.2),
]
DEFAULT_GOSSIP = 0.45


def mix(a, b):
    x = (a * 0x9E3779B97F4A7C15 + b * 0xBF58476D1CE4E5B9) & MASK
    x ^= x >> 31
    x = (x * 0x94D049BB133111EB) & MASK
    x ^= x >> 29
    return (x & 0xFFFFFFFF) / 4294967296.0


def fnv(s, h=0xCBF29CE484222325):
    for c in s.encode("utf-8"):
        h ^= c
        h = (h * 0x100000001B3) & MASK
    return h


def roll_key(key, person):
    """Gossip::RollKey: a named NPC's editor ID, else their form."""
    if person.get("unique") and person.get("name"):
        return fnv(person["name"].lower())
    return key


def heard_roll(who, rid):
    """Rumours::HeardRoll."""
    return mix(who, 0x48454152 + rid)


def reach(n, gossip, days, s):
    """Rumours::Reach."""
    if days <= 0.0:
        return 0.0
    return min(1.0, n + gossip) * (1.0 - math.exp(-(s.reach_base_rate + s.reach_gossip_rate * gossip) * days))


def gossip_score(key, person):
    fac = person.get("fac") or []
    base = next((s for f, s in GOSSIP_BY_FACTION if f in fac), DEFAULT_GOSSIP)
    return min(1.0, max(0.0, base + (mix(key, 7) - 0.5) * 0.2))


class Graph:
    def __init__(self, data, placements=None, links=(), secrecy=None):
        """`placements`: person key -> the mill they have settled in since the build.
        `links`: (a, b, kind, weight or None), as mills.ini [Links] lines.
        `secrecy`: mill -> how close it keeps what it hears, over the dump's [Secrecy]."""
        self.mills = data["mills"]
        self.secrecy = {m: v.get("secrecy") or 0.0 for m, v in self.mills.items()}
        for m, v in (secrecy or {}).items():
            if m not in self.mills:
                raise SystemExit("secrecy: no mill %r" % m)
            self.secrecy[m] = v
        self.persons = {int(k): v for k, v in data["persons"].items()}
        members = {m: sorted(int(x) for x in v["members"]) for m, v in self.mills.items()}
        # As MillsFor with a placement: where they settled replaces their place mill, and their
        # guilds and factions stay. Someone in no mill at all links nothing.
        for key, mill in (placements or {}).items():
            if mill not in self.mills:
                raise SystemExit("unknown mill %r to place someone in" % mill)
            built = [m for m, ks in members.items() if key in ks]
            if not built:
                continue
            now = [mill] + [m for m in built if m != mill and self.mills[m]["kind"] == "institution"]
            for m in built:
                if m not in now:
                    members[m].remove(key)
            for m in now:
                if m not in built:
                    members[m].append(key)
        for ks in members.values():
            ks.sort()
        self.members = members

        def gossip(k):
            return gossip_score(k, self.persons[k]) if k in self.persons else 0.0
        self.max_gossip = {m: max((gossip(k) for k in ks), default=0.0) for m, ks in members.items()}
        self.place_pos = {}
        for m in sorted(self.mills):
            v = self.mills[m]
            if v["settlement"] and v["pos"] and v["settlement"] not in self.place_pos:
                self.place_pos[v["settlement"]] = v["pos"]

        # Shared members link mills, as the builder links them; then its other edges, the stronger
        # kept where two would join the same pair. In pair order, as the DLL's Graph has them.
        member_of = collections.defaultdict(list)
        for m, ks in members.items():
            for k in ks:
                member_of[k].append(m)
        by_pair = {}
        for k, ms in member_of.items():
            ms = sorted(ms)
            for i in range(len(ms)):
                for j in range(i + 1, len(ms)):
                    e = by_pair.setdefault((ms[i], ms[j]), {"a": ms[i], "b": ms[j], "kind": "shared", "dist": 0.0, "w": 0.0})
                    e["w"] += 1.0
        for e in data["edges"]:
            if e["kind"] == "shared":
                continue
            key = (e["a"], e["b"]) if e["a"] < e["b"] else (e["b"], e["a"])
            if key not in by_pair or by_pair[key]["w"] < e["w"]:
                by_pair[key] = e
        # As the builder applies mills.ini [Links]: in place of whatever it made between the two.
        for a, b, kind, w in links:
            for m in (a, b):
                if m not in self.mills:
                    raise SystemExit("link %s=%s: no mill %r" % (a, b, m))
            key = (a, b) if a < b else (b, a)
            if key in by_pair and by_pair[key]["kind"] == "shared":
                raise SystemExit("link %s=%s: they're linked by their shared members already" % (a, b))
            if kind == "none":
                by_pair.pop(key, None)
                continue
            if kind not in LINK_WEIGHT:
                raise SystemExit("link %s=%s: unknown kind %r (road, carriage, ship, hold or none)" % (a, b, kind))
            ds = [math.hypot(pa[1] - pb[1], pa[2] - pb[2]) for pa in self.points_of(a) for pb in self.points_of(b)
                  if pa[0] == pb[0]]
            d = 60.0 if kind == "ship" else min(ds, default=0.0)
            # Ears carry news from b, where it is said, to a, who listens.
            ends = (b, a) if kind == "ears" else key
            by_pair[key] = {"a": ends[0], "b": ends[1], "kind": kind, "dist": d, "w": LINK_WEIGHT[kind] if w is None else w}
        self.edges = [by_pair[k] for k in sorted(by_pair)]
        self.adj = collections.defaultdict(list)  # mill -> [(to, edge, gossip, shared)], in edge order
        for e in self.edges:
            g = 0.0
            shared = None  # shared edges: (roll key, score) of each shared member
            if e["kind"] == "shared":
                shared = []
                for k in sorted(set(members[e["a"]]) & set(members[e["b"]])):
                    g += gossip(k)
                    shared.append((roll_key(k, self.persons[k]) if k in self.persons else k, gossip(k)))
            self.adj[e["a"]].append((e["b"], e, g, shared))
            if e["kind"] != "ears":  # ears hear, never tell
                self.adj[e["b"]].append((e["a"], e, g, shared))

    def points_of(self, mill):
        """Where a mill is on the map: its place, or for a guild or faction, its members' towns."""
        v = self.mills[mill]
        out = [v["pos"]] if v["pos"] else []
        if v["kind"] == "institution":
            for k in v["members"]:
                home = self.persons.get(int(k), {}).get("home")
                if home in self.mills and self.mills[home]["pos"]:
                    out.append(self.mills[home]["pos"])
        return out

    @staticmethod
    def edge_delay(e):
        k = e["kind"]
        if k == "shared":
            return 0.1
        if k == "strata":
            return 0.2
        if k == "ship":
            return 2.0
        return 0.25 + e["dist"] / TRAVEL_CELLS_PER_DAY

    @staticmethod
    def rate(e, gossip, n, s):
        """Hops a day along an edge, from a mill where the rumour has notability n (Graph::Rate)."""
        local = n * n
        travel = s.travel_rate_scale * n ** s.travel_exponent
        if e["kind"] == "shared":
            return s.rates["shared"] * gossip * local
        if e["kind"] == "strata":
            return s.rates["strata"] * e["w"] * local
        if e["kind"] == "ears":
            return s.rates["shared"] * e["w"] * local
        return s.rates[e["kind"]] * e["w"] * travel

    def place_delay(self, a, b):
        pa, pb = self.place_pos.get(a), self.place_pos.get(b)
        if not a or not b or not pa or not pb or pa[0] != pb[0]:
            return 2.0
        return 0.25 + math.hypot(pa[1] - pb[1], pa[2] - pb[2]) / TRAVEL_CELLS_PER_DAY

    def place_of(self, r, mill):
        """The place a mill's telling is in. An institution carries the place it heard it in."""
        for _ in range(64):
            m = self.mills.get(mill)
            if not m:
                return None
            if m["kind"] != "institution":
                return m["settlement"]
            p = r["mills"].get(mill)
            if not p or not p["parent"]:
                return None
            mill = p["parent"]
        return None


def simulate(G, r, k_from, k_to, dt, s):
    """Ticks (k_from, k_to], as Spread::Simulate, for one rumour."""
    for k in range(k_from + 1, k_to + 1):
        t = k * dt
        if r["scope"] != "spread":
            continue
        spreading = [g for g, p in sorted(r["mills"].items())
                     if p["arrived"] <= t <= p["fresh_until"] and p["n"] >= s.floor]
        for g in spreading:
            keep = max(r["secrecy"], G.secrecy.get(g, 0.0))
            if keep > 0.0 and G.max_gossip.get(g, 0.0) <= keep:
                continue
            src = dict(r["mills"][g])
            # A shared member carries it on only once it has reached them here.
            all_heard = s.no_reach or G.mills[g].get("eager")
            days = min(t, src["fresh_until"]) - src["arrived"]
            for to, e, gossip, shared in G.adj.get(g, []):
                if len(r["mills"]) >= s.max_mills:
                    break
                if to in r["mills"]:
                    continue
                if shared is not None and not all_heard:
                    gossip = sum(sc for who, sc in shared if heard_roll(who, r["id"]) < reach(src["n"], sc, days, s))
                rate = G.rate(e, gossip, src["n"], s)
                if rate <= 0.0:
                    continue
                u = mix(fnv(to, fnv(g, (r["id"] * 0x9E3779B97F4A7C15) & MASK)), k)
                if u >= 1.0 - math.exp(-rate * dt):
                    continue
                h = G.mills.get(to)
                if not h:
                    continue
                here = G.place_of(r, g)
                walk = e["kind"] in ("shared", "strata")
                # A faction told directly has no place: a shared member passes it on where they live.
                everywhere = not here and walk
                if h["kind"] == "institution":
                    there, delay = here, G.edge_delay(e)
                else:
                    there = h["settlement"]
                    if everywhere:
                        here = there
                    delay = G.edge_delay(e) if there == here or not walk else G.place_delay(here, there)
                travel = min(1.0, s.decay_travel + s.decay_travel_per_n * src["n"])
                n = src["n"] * (s.decay_local if (there and there == here) or everywhere else travel)
                if n < s.floor:
                    continue
                arrives = t + delay
                r["mills"][to] = {"decided": t, "arrived": arrives, "n": n, "hops": src["hops"] + 1, "parent": g,
                                  "edge": e["kind"], "fresh_until": arrives + fresh_days(r, n, s)}


def fresh_days(r, n, s):
    return r["fresh_days"] if r.get("fresh_days") is not None else s.base_fresh + s.fresh_per_n * n


def origin_mills(G, origin):
    def slug(x):
        return "".join(c for c in x.lower() if c.isalnum())
    if "mills" in origin:
        bad = [m for m in origin["mills"] if m not in G.mills]
        if bad:
            raise SystemExit("unknown mills %s" % bad)
        return list(dict.fromkeys(origin["mills"]))
    if "settlement" in origin:
        want = slug(origin["settlement"])
        out = sorted(m for m, v in G.mills.items() if v["kind"] != "institution" and v["settlement"] == want)
        if not out:
            raise SystemExit("no mills for settlement %r in the dump (a folded place needs the game)" % origin["settlement"])
        return out
    if "hold" in origin:
        want = slug(origin["hold"])
        out = sorted(m for m, v in G.mills.items() if v["hold"] in (want, want + "hold"))
        if not out:
            raise SystemExit("unknown hold %r" % origin["hold"])
        return out
    raise SystemExit("origin %s needs the game; give settlement, hold or mills" % json.dumps(origin))


def run(G, requests, rid, now, until, s):
    """The requests in order at `now`, as Rumours::Add, then ticks to `until`. Returns the rumour."""
    dt = s.tick_hours / 24.0
    r, stored = None, None
    for req in requests:
        if req.get("if_absent") and r is not None:
            continue
        if req.get("patch") and stored is not None:
            merged = dict(stored)
            for k, v in req.items():
                if k in ("patch", "if_absent"):
                    continue
                if v is None:
                    merged.pop(k, None)
                elif k == "versions" and isinstance(v, dict):
                    vs = dict(merged.get("versions") or {})
                    for vk, vv in v.items():
                        if vv is None:
                            vs.pop(vk, None)
                        else:
                            vs[vk] = vv
                    merged["versions"] = vs
                else:
                    merged[k] = v
        else:
            merged = {k: v for k, v in req.items() if k not in ("patch", "if_absent")}
        stored = merged
        updated = r is not None
        if r is None:
            r = {"id": rid, "mills": {}}
        r["notability"] = min(1.0, max(0.0, merged.get("notability", 0.5)))
        r["secrecy"] = min(1.0, max(0.0, merged.get("secrecy", 0.0)))
        r["scope"] = merged.get("scope", "spread")
        r["fresh_days"] = merged.get("fresh_days")
        if r["scope"] == "proclaimed":
            raise SystemExit("proclaimed rumours are scheduled, not simulated; not modelled here")
        happened = merged.get("happened", now)
        start = min(now, max(now - 30.0, happened))
        delay = merged.get("delay_hours", 0.0) / 24.0
        fresh = fresh_days(r, r["notability"], s)
        before = len(r["mills"])
        for m, d, how in [(m, delay, "origin") for m in origin_mills(G, merged["origin"])] + \
                         [(m, 0.0, "reach") for m in merged.get("reach", []) if m in G.mills]:
            p = r["mills"].setdefault(m, {"decided": start, "arrived": start + d, "hops": 0, "parent": "",
                                          "edge": how, "fresh_until": 0.0})
            p["fresh_until"] = max(p["fresh_until"], p["arrived"] + fresh)
            p["n"] = r["notability"]
        added = len(r["mills"]) > before
        if start < now and r["scope"] == "spread" and (not updated or added):
            k_from, k_to = math.floor(start / dt), math.floor(now / dt)
            if k_from < k_to:
                simulate(G, r, k_from, k_to, dt, s)
    simulate(G, r, math.floor(now / dt), math.floor(until / dt), dt, s)
    return r


def report_one(G, r, until):
    rows = sorted(r["mills"].items(), key=lambda kv: kv[1]["arrived"])
    known = [(m, p) for m, p in rows if p["arrived"] <= until]
    coming = [(m, p) for m, p in rows if p["arrived"] > until]
    holds = collections.Counter(G.mills[m]["hold"] or "-" for m, _ in known)
    people = len({k for m, _ in known for k in G.members[m]})
    print("Rumour id %d by day %.2f: %d mills know it (%d people), %d more on the way" % (
        r["id"], until, len(known), people, len(coming)))
    print("  holds: %s" % ", ".join("%s %d" % kv for kv in holds.most_common()))
    print("  %-26s %-11s %8s %8s %4s %5s %8s  %s" % ("mill", "kind", "decided", "arrives", "hops", "n", "fresh to", "from"))
    for m, p in rows:
        mark = "" if p["arrived"] <= until else "  (on the way)"
        stale = " stale" if p["arrived"] <= until and p["fresh_until"] < until else ""
        print("  %-26s %-11s %8.2f %8.2f %4d %5.3f %8.2f  %s%s%s" % (
            m, G.mills[m]["kind"], p["decided"], p["arrived"], p["hops"], p["n"], p["fresh_until"],
            (p["parent"] + " (" + p["edge"] + ")") if p["parent"] else p["edge"], stale, mark))


def report_many(G, runs, until):
    got = collections.defaultdict(list)
    sizes = []
    for r in runs:
        known = [m for m, p in r["mills"].items() if p["arrived"] <= until]
        sizes.append(len(known))
        for m in known:
            got[m].append(r["mills"][m]["arrived"])
    n = len(runs)
    print("Over %d rumour ids (%d-%d), by day %.2f: mills knowing it median %d, range %d-%d" % (
        n, runs[0]["id"], runs[-1]["id"], until, statistics.median(sizes), min(sizes), max(sizes)))
    print("  %-26s %-11s %-14s %6s %10s" % ("mill", "kind", "hold", "share", "p50 day"))
    for m, ts in sorted(got.items(), key=lambda kv: (-len(kv[1]), statistics.median(kv[1]))):
        print("  %-26s %-11s %-14s %5.0f%% %10.2f" % (m, G.mills[m]["kind"], G.mills[m]["hold"] or "-",
                                                     100.0 * len(ts) / n, statistics.median(ts)))


def parse_link(text):
    a, _, rest = text.partition("=")
    b, _, rest = rest.partition(":")
    kind, _, w = rest.partition(":")
    if not (a and b and kind):
        raise SystemExit("--link %r: give A=B:KIND or A=B:KIND:WEIGHT" % text)
    return a.strip().lower(), b.strip().lower(), kind.strip().lower(), float(w) if w else None


def person_key(data, name):
    """A person's key from a --place name: their key itself, or a display name one person in a mill has."""
    if name.isdigit():
        return int(name)
    in_mills = {k for v in data["mills"].values() for k in v["members"]}
    found = [int(k) for k, v in data["persons"].items() if k in in_mills and v["display"].lower() == name.lower()]
    if len(found) != 1:
        raise SystemExit("%d people in mills are called %r%s" % (len(found), name, "; give a key: %s" % found if found else ""))
    return found[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("requests")
    ap.add_argument("--mills", default=DUMP)
    ap.add_argument("--now", type=float, required=True, help="game day the requests are sent")
    ap.add_argument("--until", type=float, required=True)
    ap.add_argument("--id", type=int, default=1, help="the rumour id it gets: the database's next")
    ap.add_argument("--ids", type=int, default=0, help="run this many ids from --id and report shares")
    # Settings, at their defaults. The user's own values in SkyrimNet's plugin config aren't read.
    ap.add_argument("--tick-hours", type=float, default=2.0)
    ap.add_argument("--decay-local", type=float, default=0.9)
    ap.add_argument("--decay-travel", type=float, default=0.6)
    ap.add_argument("--floor", type=float, default=0.15)
    ap.add_argument("--base-fresh", type=float, default=2.0)
    ap.add_argument("--fresh-per-n", type=float, default=6.0)
    ap.add_argument("--max-mills", type=int, default=80)
    ap.add_argument("--decay-travel-per-n", type=float, default=0.3,
                    help="spread.decay_travel_per_notability: big news keeps its size on the road")
    ap.add_argument("--travel-rate-scale", type=float, default=8.0, help="spread.travel_rate_scale")
    ap.add_argument("--travel-exponent", type=float, default=4.0, help="spread.travel_exponent")
    # Not settings: the DLL's constant base rates, for trying other values.
    for kind, v in RATE.items():
        ap.add_argument("--rate-" + kind, type=float, default=v, help="base hops a day on %s edges" % kind)
    ap.add_argument("--reach-base-rate", type=float, default=0.5, help="reach.base_rate")
    ap.add_argument("--reach-gossip-rate", type=float, default=2.0, help="reach.gossip_rate")
    ap.add_argument("--no-reach", action="store_true", help="reach.enabled off: every shared member carries it at once")
    ap.add_argument("--notability", type=float, help="override the requests' notability")
    ap.add_argument("--place", action="append", default=[], metavar="NAME=MILL",
                    help="someone settled in a mill since the build, by name or person key; repeatable")
    ap.add_argument("--link", action="append", default=[], metavar="A=B:KIND[:WEIGHT]",
                    help="a travel link as a mills.ini [Links] line makes it (road, carriage, ship, hold, ears "
                         "for A hearing B one-way, or none to take one away); repeatable")
    ap.add_argument("--mill-secrecy", action="append", default=[], metavar="MILL=VALUE",
                    help="a mills.ini [Secrecy] line: the mill keeps what it hears; repeatable")
    s = ap.parse_args()
    s.rates = {k: getattr(s, "rate_" + k) for k in RATE}

    with open(s.mills, encoding="utf-8") as f:
        data = json.load(f)
    G = Graph(data, {person_key(data, name): mill for name, _, mill in (p.partition("=") for p in s.place)},
              [parse_link(x) for x in s.link],
              {m.strip().lower(): float(v) for m, _, v in (x.partition("=") for x in s.mill_secrecy)})
    with open(s.requests, encoding="utf-8") as f:
        reqs = json.load(f)
    if isinstance(reqs, dict):
        reqs = [reqs]
    if s.notability is not None:
        for q in reqs:
            if "notability" in q:
                q["notability"] = s.notability
    if s.ids:
        report_many(G, [run(G, reqs, s.id + i, s.now, s.until, s) for i in range(s.ids)], s.until)
    else:
        report_one(G, run(G, reqs, s.id, s.now, s.until, s), s.until)


if __name__ == "__main__":
    main()
