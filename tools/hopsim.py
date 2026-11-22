"""
Monte Carlo of rumours hopping between mills (ADR-001, stage 0).

  python hopsim.py                         # every strategy, default parameters
  python hopsim.py --strategy merged --trials 40 --days 90
  python hopsim.py --hop-decay 0.75 --road 0.4   # try other parameters

A simulated player wanders between settlements. Rumours are seeded where the
player is, because that is where SkyrimNet forms memories. Each rumour then
hops along the mill graph by the formulas in the ADR, and the report gives:

  - how far rumours travel, and how long they take to reach the next hold;
  - who has heard them, inside a mill, by gossip score;
  - sameness: how alike two mills, or two people, are in what they know;
  - retell calls under lazy and eager retelling.
"""

import argparse
import bisect
import collections
import json
import math
import os
import random
import statistics

HERE = os.path.dirname(os.path.abspath(__file__))
STRATEGIES = ["settlement", "strata", "institutions", "merged"]

# Gossip score defaults by faction (gossips.ini in the real mod).
GOSSIP_BY_FACTION = [
    ("JobInnkeeperFaction", 0.9), ("JobInnServer", 0.85), ("JobBardFaction", 0.85),
    ("FavorJobsBeggarsFaction", 0.8), ("FavorJobsDrunksFaction", 0.8),
    ("JobStreetVendorFaction", 0.75), ("JobMerchantFaction", 0.7), ("CarriageSystemFaction", 0.7),
    ("JobFenceFaction", 0.6), ("JobHostlerFaction", 0.6),
    ("JobStewardFaction", 0.5), ("JobJarlFaction", 0.4),
    ("IsGuardFaction", 0.3), ("JobHousecarlFaction", 0.25), ("JobCourtWizardFaction", 0.2),
]
DEFAULT_GOSSIP = 0.45


def mix(a, b):
    """Deterministic hash of two ints to [0, 1): the per-person roll u(a, r)."""
    x = (a * 0x9E3779B97F4A7C15 + b * 0xBF58476D1CE4E5B9) & 0xFFFFFFFFFFFFFFFF
    x ^= x >> 31
    x = (x * 0x94D049BB133111EB) & 0xFFFFFFFFFFFFFFFF
    x ^= x >> 29
    return (x & 0xFFFFFFFF) / 4294967296.0


def gossip_score(pid, info):
    fac = set(info["fac"])
    base = next((s for f, s in GOSSIP_BY_FACTION if f in fac), DEFAULT_GOSSIP)
    jitter = (mix(pid, 7) - 0.5) * 0.2
    return min(1.0, max(0.0, base + jitter))


class Graph:
    def __init__(self, data, p):
        self.p = p
        self.mills = data["mills"]
        self.ids = sorted(self.mills)
        self.persons = {int(k): v for k, v in data["persons"].items()}
        self.gossip = {k: gossip_score(k, v) for k, v in self.persons.items()}
        self.members = {m: [int(x) for x in self.mills[m]["members"]] for m in self.ids}
        self.member_of = collections.defaultdict(list)
        for m, ms in self.members.items():
            for k in ms:
                self.member_of[k].append(m)

        self.adj = collections.defaultdict(list)  # mill -> [(other, edge)]
        for e in data["edges"]:
            self.adj[e["a"]].append((e["b"], e))
            self.adj[e["b"]].append((e["a"], e))
        self.shared = {}
        for e in data["edges"]:
            if e["kind"] == "shared":
                self.shared[(e["a"], e["b"])] = sorted(set(self.members[e["a"]]) & set(self.members[e["b"]]))

        # Places the player can be: one per settlement slug, with that place's mills.
        self.place_mills = collections.defaultdict(list)
        for m in self.ids:
            s = self.mills[m]["settlement"]
            if s and self.mills[m]["kind"] != "institution":
                self.place_mills[s].append(m)
        self.place_of = {m: self.mills[m]["settlement"] for m in self.ids}
        self.hold_of = {m: self.mills[m]["hold"] for m in self.ids}
        self.place_size = {s: sum(len(self.members[m]) for m in ms) for s, ms in self.place_mills.items()}
        self.place_pos = {s: next((self.mills[m]["pos"] for m in ms if self.mills[m]["pos"]), None)
                          for s, ms in self.place_mills.items()}
        # Travel graph between places, from the place-to-place edges.
        self.travel = collections.defaultdict(dict)
        for e in data["edges"]:
            if e["kind"] in ("road", "carriage", "ship", "hold"):
                a, b = self.place_of.get(e["a"]), self.place_of.get(e["b"])
                if a and b and a != b:
                    d = self.delay(e)
                    self.travel[a][b] = min(d, self.travel[a].get(b, 1e9))
                    self.travel[b][a] = min(d, self.travel[b].get(a, 1e9))

    def place_delay(self, a, b):
        pa, pb = self.place_pos.get(a), self.place_pos.get(b)
        if not pa or not pb or pa[0] != pb[0]:
            return 2.0
        return 0.25 + math.hypot(pa[1] - pb[1], pa[2] - pb[2]) / self.p.travel_cells_per_day

    def delay(self, e):
        """Days for news to cross an edge."""
        k = e["kind"]
        if k == "shared":
            return 0.1
        if k == "strata":
            return 0.2
        if k == "ship":
            return 2.0
        return 0.25 + e["dist"] / self.p.travel_cells_per_day

    def rate(self, e, g, h, r, t, sim):
        """Hop rate per day along edge e from mill g, before notability and interest."""
        k = e["kind"]
        if k == "shared":
            key = (e["a"], e["b"])
            return self.p.shared * sum(self.gossip[x] for x in self.shared[key] if sim.heard(x, r, t))
        return getattr(self.p, k) * e["w"]


class Rumour:
    __slots__ = ("id", "n0", "origin", "born", "at", "place", "scheduled", "versions")

    def __init__(self, rid, n0, origin, born):
        self.id, self.n0, self.origin, self.born = rid, n0, origin, born
        self.at = {}            # mill -> (arrived, n, fresh_until, parent)
        self.place = {}         # mill -> the settlement this telling is in (institutions carry it)
        self.scheduled = set()  # mills with an arrival on the way
        self.versions = {origin}


class Sim:
    def __init__(self, G, p, rng):
        self.G, self.p, self.rng = G, p, rng
        self.rumours = []
        self.events = []        # (time, seq, rumour, mill, n, parent)
        self.seq = 0
        self.lazy_calls = 0
        self.lazy_versions = 0
        self.player_place = None
        self.cross_kinds = collections.Counter()

    # ---- who has heard it
    def reach(self, pid, r, t):
        """Highest reach of r over pid's mills at time t, and the mill it came from."""
        best, best_m = 0.0, None
        gs = self.G.gossip[pid]
        for m in self.G.member_of[pid]:
            st = r.at.get(m)
            if not st or st[0] > t:
                continue
            arrived, n, fresh_until, _ = st
            age = min(t, fresh_until) - arrived
            ceiling = min(1.0, n + gs)
            rate = self.p.reach_base_rate + self.p.reach_gossip_rate * gs
            v = ceiling * (1.0 - math.exp(-rate * age))
            if v > best:
                best, best_m = v, m
        return best, best_m

    def heard(self, pid, r, t):
        if not self.p.weighting:
            return any(m in r.at and r.at[m][0] <= t for m in self.G.member_of[pid])
        return mix(pid, r.id) < self.reach(pid, r, t)[0]

    # ---- spreading
    def arrive(self, r, m, t, n, parent, place):
        r.scheduled.discard(m)
        if m in r.at or len(r.at) >= self.p.max_mills or n < self.p.n_floor:
            return
        fresh = t + self.p.base_fresh_days + self.p.fresh_per_n * n
        r.at[m] = (t, n, fresh, parent)
        r.place[m] = place
        if self.p.retell == "lazy" and self.G.place_of.get(m) == self.player_place:
            self.materialise(r, self.player_place, t)

    def schedule(self, r, m, t, n, parent, place):
        r.scheduled.add(m)
        self.seq += 1
        bisect.insort(self.events, (t, self.seq, r.id, m, n, parent, place))

    def step(self, t, dt):
        while self.events and self.events[0][0] <= t:
            et, _, rid, m, n, parent, place = self.events.pop(0)
            self.arrive(self.rumours[rid], m, et, n, parent, place)
        for r in self.rumours:
            for g, (arrived, n, fresh_until, _) in list(r.at.items()):
                if t > fresh_until or arrived > t:
                    continue
                for h, e in self.G.adj[g]:
                    if h in r.at or h in r.scheduled:
                        continue
                    rate = self.G.rate(e, g, h, r, t, self) * n ** self.p.n_exponent * self.interest(r, h)
                    if rate <= 0:
                        continue
                    if self.rng.random() < 1.0 - math.exp(-rate * dt):
                        # Carried within a place, or to another one (possibly via a guild).
                        here = r.place[g]
                        if self.G.mills[h]["kind"] == "institution":
                            there, delay = here, self.G.delay(e)
                        else:
                            there = self.G.place_of[h]
                            delay = self.G.delay(e) if there == here or e["kind"] not in ("shared", "strata")                                 else self.G.place_delay(here, there)
                        decay = self.p.local_decay if there == here else self.p.hop_decay
                        if self.G.hold_of.get(h) and self.G.hold_of[h] != self.G.hold_of.get(g):
                            self.cross_kinds[e["kind"]] += 1
                        self.schedule(r, h, t + delay, n * decay, g, there)

    def interest(self, r, h):
        if not self.p.interest_spread:
            return 1.0
        return 0.5 + mix(r.id, hash(h) & 0xFFFFFFFF) * self.p.interest_spread

    # ---- lazy retelling
    def materialise(self, r, place, t):
        """One call per (rumour, settlement): write every missing version on the paths there."""
        need = [m for m in self.G.place_mills.get(place, []) if m in r.at and r.at[m][0] <= t
                and r.at[m][2] >= t and m not in r.versions]
        if not need:
            return
        written = set()
        for m in need:
            x = m
            while x not in r.versions and x not in written:
                written.add(x)
                x = r.at[x][3]
        self.lazy_calls += 1
        self.lazy_versions += len(written)
        r.versions |= written


def run(G, p, seed):
    rng = random.Random(seed)
    sim = Sim(G, p, rng)
    dt = p.tick_hours / 24.0
    places = [s for s in G.place_mills if G.travel.get(s)]
    start = "whiterun" if "whiterun" in places else max(places, key=lambda s: G.place_size[s])
    sim.player_place = start
    leave_at = rng.uniform(0.5, 2.0)
    travelling_to, arrive_at = None, None
    samples = collections.defaultdict(list)
    t = 0.0
    while t < p.days:
        # Player itinerary.
        if travelling_to is None and t >= leave_at:
            opts = list(G.travel[sim.player_place].items())
            wts = [G.place_size[s] ** 0.7 for s, _ in opts]
            dest, d = rng.choices(opts, wts)[0]
            travelling_to, arrive_at = dest, t + d
            sim.player_place = None
        if travelling_to is not None and t >= arrive_at:
            sim.player_place = travelling_to
            travelling_to = None
            size = G.place_size[sim.player_place]
            leave_at = t + rng.uniform(0.3, 1.0) * (2.0 if size > 40 else 1.0)
            if p.retell == "lazy":
                for r in sim.rumours:
                    sim.materialise(r, sim.player_place, t)
        # Seeding where the player is.
        if sim.player_place and rng.random() < p.seed_per_day * dt:
            ms = G.place_mills[sim.player_place]
            origin = rng.choices(ms, [len(G.members[m]) for m in ms])[0]
            u = rng.random()
            n0 = rng.uniform(0.2, 0.4) if u < 0.6 else rng.uniform(0.4, 0.7) if u < 0.9 else rng.uniform(0.7, 1.0)
            r = Rumour(len(sim.rumours), n0, origin, t)
            r.at[origin] = (t, n0, t + p.base_fresh_days + p.fresh_per_n * n0, None)
            r.place[origin] = sim.player_place
            sim.rumours.append(r)
            if p.retell == "lazy":
                sim.materialise(r, sim.player_place, t)
        sim.step(t, dt)
        if abs(t - round(t)) < dt / 2 and t >= 5:
            sample(sim, t, samples)
        t += dt
    return sim, samples


def sample(sim, t, out):
    """Sameness and who-has-heard figures at one moment."""
    G, rng = sim.G, sim.rng
    fresh = {m: set() for m in G.ids}
    for r in sim.rumours:
        for m, (arrived, n, fu, _) in r.at.items():
            if arrived <= t <= fu:
                fresh[m].add(r.id)

    def jac(a, b):
        return len(a & b) / len(a | b) if a | b else None

    by_place = collections.defaultdict(list)
    by_hold = collections.defaultdict(list)
    for m in G.ids:
        if G.mills[m]["kind"] == "institution":
            continue
        by_place[G.place_of[m]].append(m)
        by_hold[G.hold_of[m]].append(m)
    for key, groups in (("mills, same town", by_place), ("mills, same hold", by_hold)):
        for ms in groups.values():
            for i in range(len(ms)):
                for j in range(i + 1, len(ms)):
                    v = jac(fresh[ms[i]], fresh[ms[j]])
                    if v is not None:
                        out[key].append(v)

    def known(pid):
        rs = set()
        for m in G.member_of[pid]:
            rs |= fresh[m]
        return {rid for rid in rs if sim.heard(pid, sim.rumours[rid], t)}

    for _ in range(60):
        m = rng.choice(G.ids)
        if len(G.members[m]) < 2:
            continue
        a, b = rng.sample(G.members[m], 2)
        v = jac(known(a), known(b))
        if v is not None:
            out["people, same mill"].append(v)
    for s, ms in by_place.items():
        people = sorted({k for m in ms for k in G.members[m]})
        if len(people) >= 2:
            a, b = rng.sample(people, 2)
            v = jac(known(a), known(b))
            if v is not None:
                out["people, same town"].append(v)

    # Who has heard: share of a mill that has heard each fresh rumour, by age and gossip.
    for m in G.ids:
        for rid in fresh[m]:
            r = sim.rumours[rid]
            age = t - r.at[m][0]
            band = "<0.5d" if age < 0.5 else "0.5-1d" if age < 1 else "1-2d" if age < 2 else "2-4d" if age < 4 else "4d+"
            for pid in G.members[m]:
                gs = G.gossip[pid]
                gband = "gossip <0.35" if gs < 0.35 else "gossip 0.35-0.65" if gs < 0.65 else "gossip >0.65"
                nband = "n<0.3" if r.at[m][1] < 0.3 else "n 0.3-0.6" if r.at[m][1] < 0.6 else "n>0.6"
                h = 1.0 if sim.heard(pid, r, t) else 0.0
                out["heard|" + band + "|" + gband].append(h)
                out["heardn|" + nband + "|" + gband].append(h)


def pct(xs, q):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(q * len(xs)))] if xs else float("nan")


def nband(n):
    return "minor" if n < 0.4 else "moderate" if n < 0.7 else "major"


def report(G, sims, samples, p, name):
    reach, deep = [], []
    holds = collections.defaultdict(list)
    cross = collections.defaultdict(list)
    town_all, town_t = [], []
    count = collections.Counter()
    eager_calls = lazy_calls = lazy_versions = 0
    for sim in sims:
        lazy_calls += sim.lazy_calls
        lazy_versions += sim.lazy_versions
        for r in sim.rumours:
            if r.born > p.days - 15:  # too young to judge how far it went
                continue
            b = nband(r.n0)
            count[b] += 1
            reach.append(len(r.at))
            hs = {G.hold_of[m] for m in r.at if G.hold_of[m]}
            holds[b].append(len(hs))
            h0 = G.hold_of[r.origin]
            t_cross = min((st[0] - r.born for m, st in r.at.items() if G.hold_of[m] and G.hold_of[m] != h0),
                          default=None)
            if t_cross is not None:
                cross[b].append(t_cross)
            town = G.place_mills.get(G.place_of[r.origin], [])
            if len(town) > 1:
                got = [r.at[m][0] - r.born for m in town if m in r.at]
                town_all.append(len(got) == len(town))
                if len(got) == len(town):
                    town_t.append(max(got))
            deep.append(max((hops(r, m) for m in r.at), default=0))
            eager_calls += len(r.at) - 1
    trials = len(sims)
    print("\n== %s: %d mills, %d trials x %d days, %.1f rumours/day ==" % (
        name, len(G.ids), trials, p.days, p.seed_per_day))
    print("  mills reached      median %d, p90 %d, max %d" % (statistics.median(reach), pct(reach, .9), max(reach)))
    if town_all:
        print("  own town, all mills %.0f%% of rumours; p50 %.1f days" % (
            100.0 * sum(town_all) / len(town_all), statistics.median(town_t) if town_t else float("nan")))
    print("  holds reached          1     2     3   4-5    6+   next hold (p50 days)")
    for b in ("minor", "moderate", "major"):
        hb = collections.Counter(1 if h <= 1 else 2 if h == 2 else 3 if h == 3 else 4 if h <= 5 else 6
                                 for h in holds[b])
        tot = len(holds[b]) or 1
        print("    %-9s %4d  %s   %3.0f%% (%.1f)" % (
            b, count[b], " ".join("%4.0f%%" % (100.0 * hb[k] / tot) for k in (1, 2, 3, 4, 6)),
            100.0 * len(cross[b]) / tot, statistics.median(cross[b]) if cross[b] else float("nan")))
    ck = collections.Counter()
    for sim in sims:
        ck.update(sim.cross_kinds)
    print("  hold crossings by  %s" % ", ".join("%s %d%%" % (k, 100 * v / sum(ck.values())) for k, v in ck.most_common()))
    print("  hops deep          median %d, p90 %d" % (statistics.median(deep), pct(deep, .9)))
    for key in ("mills, same town", "mills, same hold", "people, same mill", "people, same town"):
        xs = samples.get(key)
        if xs:
            print("  sameness %-18s mean %.2f" % (key, statistics.mean(xs)))
    print("  retell calls       eager %.0f/trial, lazy %.0f/trial writing %.0f versions" % (
        eager_calls / trials, lazy_calls / trials, lazy_versions / trials))
    if p.weighting and p.verbose:
        print("  who has heard (share of the mill, by age of the rumour there):")
        for g in ("gossip <0.35", "gossip 0.35-0.65", "gossip >0.65"):
            row = []
            for band in ("<0.5d", "0.5-1d", "1-2d", "2-4d", "4d+"):
                xs = samples.get("heard|%s|%s" % (band, g))
                row.append("%5.0f%%" % (100 * statistics.mean(xs)) if xs else "    - ")
            print("    %-17s %s" % (g, " ".join(row)))
        print("  who has heard (by notability in that mill):")
        for g in ("gossip <0.35", "gossip 0.35-0.65", "gossip >0.65"):
            row = []
            for nb in ("n<0.3", "n 0.3-0.6", "n>0.6"):
                xs = samples.get("heardn|%s|%s" % (nb, g))
                row.append("%5.0f%%" % (100 * statistics.mean(xs)) if xs else "    - ")
            print("    %-17s %s" % (g, " ".join(row)))


def hops(r, m):
    k = 0
    while r.at[m][3] is not None:
        m = r.at[m][3]
        k += 1
    return k


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--strategy", nargs="*", help="cache/mills_<name>.json files to run; default all four strategies")
    ap.add_argument("--trials", type=int, default=12)
    ap.add_argument("--days", type=float, default=60)
    ap.add_argument("--seed-per-day", type=float, default=1.5)
    ap.add_argument("--tick-hours", type=float, default=2.0)
    ap.add_argument("--hop-decay", type=float, default=0.6, help="notability kept per hop between places")
    ap.add_argument("--local-decay", type=float, default=0.9, help="notability kept per hop within a place")
    ap.add_argument("--n-floor", type=float, default=0.15)
    ap.add_argument("--n-exponent", type=float, default=2.0,
                    help="hop rate scales with notability to this power; >1 keeps minor talk local")
    ap.add_argument("--base-fresh-days", type=float, default=2.0)
    ap.add_argument("--fresh-per-n", type=float, default=6.0)
    ap.add_argument("--max-mills", type=int, default=40)
    ap.add_argument("--travel-cells-per-day", type=float, default=40.0)
    # Hop rates per day, per unit of edge weight and notability.
    ap.add_argument("--shared", type=float, default=1.0)
    ap.add_argument("--strata", type=float, default=3.0)
    ap.add_argument("--road", type=float, default=0.6)
    ap.add_argument("--carriage", type=float, default=0.1)
    ap.add_argument("--ship", type=float, default=0.3)
    ap.add_argument("--hold", type=float, default=0.3)
    ap.add_argument("--interest-spread", type=float, default=0.0,
                    help="0 = no topic interest; 1 = each mill weights each rumour 0.5-1.5")
    ap.add_argument("--reach-base-rate", type=float, default=0.5)
    ap.add_argument("--reach-gossip-rate", type=float, default=2.0)
    ap.add_argument("--no-weighting", dest="weighting", action="store_false",
                    help="everyone in a mill has heard everything in it")
    ap.add_argument("--retell", choices=["lazy", "eager"], default="lazy")
    ap.add_argument("-v", dest="verbose", action="store_true")
    p = ap.parse_args()

    for s in (p.strategy or STRATEGIES):
        with open(os.path.join(HERE, "cache", "mills_%s.json" % s)) as f:
            G = Graph(json.load(f), p)
        sims, samples = [], collections.defaultdict(list)
        for i in range(p.trials):
            sim, smp = run(G, p, 1000 + i)
            sims.append(sim)
            for k, v in smp.items():
                samples[k].extend(v)
        report(G, sims, samples, p, s)


if __name__ == "__main__":
    main()
