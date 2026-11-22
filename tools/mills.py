"""
Build rumour mills from the cached game data under several strategies, and the
edges between them, then report coverage and sizes.

  python mills.py                      # all strategies, summary table
  python mills.py --strategy merged -v # one strategy, every mill and its members

Strategies (ADR-001, stage 0):
  settlement    one mill per settlement
  strata        cities split into court / garrison / commons / underworld
  institutions  strata, plus guild-like mills that span settlements
  merged        institutions, with mills below --floor members folded into
                their settlement's commons (or dropped, for institutions)

Writes cache/mills_<strategy>.json for hopsim.py.
"""

import argparse
import collections
import json
import math
import os
import pickle
import re
import statistics

import esm

HERE = os.path.dirname(os.path.abspath(__file__))

HOLD_KW = {"LocTypeHold", "LocTypeHoldMajor", "LocTypeHoldMinor"}
SETTLEMENT_KW = {"LocTypeHabitation", "LocTypeHabitationHasInn", "LocTypeCity", "LocTypeTown",
                 "LocTypeSettlement", "LocTypeHoldCapital", "LocTypeOrcStronghold"}
CIVIL_KW = {"LocTypeDwelling", "LocTypeHouse", "LocTypeInn", "LocTypeStore", "LocTypeGuild",
            "LocTypeTemple", "LocTypeCastle", "LocTypeBarracks", "LocTypeStewardsDwelling",
            "LocTypePlayerHouse", "LocTypeFarm", "LocTypeLumberMill", "LocTypeMine",
            "LocTypeJail", "LocTypeShip", "BYOH_LocTypeHomestead"}
HOSTILE_KW = {"LocTypeBanditCamp", "LocTypeForswornCamp", "LocTypeVampireLair",
              "LocTypeWarlockLair", "LocTypeDungeon", "LocTypeDraugrCrypt", "LocTypeDragonLair",
              "LocTypeGiantCamp", "LocTypeHagravenNest", "LocTypeAnimalDen", "LocTypeFalmerHive",
              "LocTypeDwarvenAutomatons", "LocTypeSprigganGrove", "LocTypeMilitaryCamp",
              "LocTypeMilitaryFort"}
HOSTILE_FACTIONS = {"BanditFaction", "ForswornFaction", "VampireFaction", "NecromancerFaction",
                    "WarlockFaction", "HagravenFaction", "DragonPriestFaction", "DLC1VampireFaction",
                    "DLC2BanditDialogueFaction", "SilverHandFaction", "DLC1UndeadGuardianFaction"}

COURT_FACTIONS = {"JobJarlFaction", "JobStewardFaction", "JobHousecarlFaction", "JobCourtWizardFaction"}
GUARD_FACTIONS_PREFIX = ("GuardFaction",)
GUARD_FACTIONS = {"IsGuardFaction", "JobGuardCaptainFaction", "DragonsreachBasementGuards"}
UNDERWORLD_FACTIONS = {"ThievesGuildFaction", "RiftenRaggedFlagonFaction", "JobFenceFaction",
                       "TownCidhnaMinePrisonerFaction"}
# Guild members sometimes carry guard factions; they belong with the townsfolk.
GUILD_FACTIONS = {"CompanionsFaction", "CollegeofWinterholdFaction", "SolitudeBardsCollegeFaction"}

INSTITUTIONS = {
    "companions": {"CompanionsFaction"},
    "college": {"CollegeofWinterholdFaction"},
    "thievesguild": {"ThievesGuildFaction"},
    "darkbrotherhood": {"DarkBrotherhoodFaction"},
    "bardscollege": {"SolitudeBardsCollegeFaction"},
    "eastempire": {"TG04EastEmpireFaction", "ServicesSolitudeEastEmpireCompany",
                   "WindhelmEastEmpireFaction"},
    "dawnguard": {"DLC1DawnguardFaction"},
}
# Travelling trades (inn bards, carriage drivers, caravans) are travel links
# between places, not mills: a mill is people who talk to each other daily.

# Authored travel links that geography alone wouldn't find.
SHIP_LINKS = [("windhelm", "ravenrock")]

# Quest copies, corpses, props and battle extras rather than people who live there,
# and the crowd for Windhelm's fighting pit, which was cut from the game.
NOT_A_RESIDENT = re.compile(r"Dead|Corpse|Captive|Mannequin|Decoy|TortureVictim|Ghost|^CW|^MQ\d|"
                            r"^DA\d\d|Sentry|^dun|^BYOHHouse\dBard|^BYOHCarriageDriver\d|^WindhelmPitFan")

# Non-hostile places outside any settlement that hold a community of their own.
ENCLAVE_MIN = 4
NOT_ENCLAVES = {"PersistAll", "HelgenLocation", "SovngardeHallofHeroesLocation", "SovngardeLocation",
                "HoldingCell", "DLC1SoulCairnLocation", "ThroatoftheWorldLocation",
                "DLC1FalmerValleyLocation", "DLC2DremoraShopLocation"}

ACTOR_TYPE_NPC = "ActorTypeNPC"
CELL = 4096.0
STRATEGIES = ["settlement", "strata", "institutions", "merged"]


def load_world(name=None):
    fn = "world-%s.pkl" % name if name else "world.pkl"
    with open(os.path.join(HERE, "cache", fn), "rb") as f:
        return pickle.load(f)


class World:
    def __init__(self, w):
        self.w = w
        kw = w["keywords"]
        self.loc_kw = {fid: {kw.get(k) for k in l["kw"]} for fid, l in w["locations"].items()}
        self.fac = w["factions"]
        self._world_root = {}
        self.cell_by_grid = {}
        for cfid, c in w["cells"].items():
            if c["world"] and c["grid"] is not None:
                self.cell_by_grid[(c["world"], tuple(c["grid"]))] = cfid

    # ---- locations
    def ancestors(self, loc):
        seen = set()
        while loc and loc in self.w["locations"] and loc not in seen:
            seen.add(loc)
            yield loc
            loc = self.w["locations"][loc]["parent"]

    def tier_of(self, loc, kws):
        return next((a for a in self.ancestors(loc) if self.loc_kw[a] & kws), None)

    def hold_of(self, loc):
        """
        The outermost ancestor with a hold keyword. Not the nearest: keyword
        distributors can copy a hold's keyword down to everything inside it (in
        game, OCF does this for Eastmarch and Haafingar), and holds don't nest.
        """
        out = None
        for a in self.ancestors(loc):
            if self.loc_kw[a] & HOLD_KW:
                out = a
        return out

    def hostile(self, loc):
        return any(self.loc_kw[a] & HOSTILE_KW for a in self.ancestors(loc))

    def edid(self, loc):
        return self.w["locations"][loc]["edid"] if loc in self.w["locations"] else "?"

    def world_root(self, world):
        if world in self._world_root:
            return self._world_root[world]
        root, seen = world, set()
        while root in self.w["worlds"] and self.w["worlds"][root]["parent"] and root not in seen:
            seen.add(root)
            root = self.w["worlds"][root]["parent"]
        self._world_root[world] = root
        return root

    # ---- actors
    def resolve_npc(self, fid, flag, depth=0):
        """Follow the template chain while the NPC takes `flag` data from its template."""
        n = self.w["npcs"].get(fid)
        while n and n["tflags"] & flag and n["tpl"] and depth < 8:
            tpl = n["tpl"]
            # Leveled lists can nest (Requiem's do): take the first entry down to an NPC.
            for _ in range(8):
                if tpl not in self.w["lvln"]:
                    break
                ents = self.w["lvln"][tpl]
                tpl = ents[0] if ents else None
            n = self.w["npcs"].get(tpl)
            depth += 1
        return n

    def is_person(self, fid):
        n = self.w["npcs"].get(fid)
        if not n or n["flags"] & 0x04:  # chargen face preset
            return False
        t = self.resolve_npc(fid, 0x01)
        race = self.w["races"].get(t["race"]) if t else None
        if not race:
            return False
        return any(self.w["keywords"].get(k) == ACTOR_TYPE_NPC for k in race["kw"])

    def factions(self, fid):
        n = self.resolve_npc(fid, 0x04)
        return {self.fac.get(f) for f, rank in (n["fac"] if n else []) if rank >= 0} - {None}

    def effectively_disabled(self, a, depth=0):
        """Initially disabled, directly or through its enable-parent chain."""
        if a["disabled"]:
            return True
        if not a["esp"] or depth > 6:
            return False
        parent, opposite = a["esp"]
        pr = self.w["refs"].get(parent) or self.w["achr"].get(parent)
        if pr is None:
            return False
        return self.effectively_disabled(pr, depth + 1) != opposite

    def ref_location(self, a):
        if a["ploc"]:
            return a["ploc"]
        c = self.w["cells"].get(a["cell"])
        if c and c["loc"]:
            return c["loc"]
        if c and c["world"] and a["pos"]:
            gx, gy = math.floor(a["pos"][0] / CELL), math.floor(a["pos"][1] / CELL)
            ext = self.w["cells"].get(self.cell_by_grid.get((c["world"], (gx, gy))))
            if ext and ext["loc"]:
                return ext["loc"]
        if c and c["world"]:
            # A city worldspace names its own location (Riften's is Riften).
            return self.w["worlds"].get(c["world"], {}).get("loc")
        return None


def residents(W):
    """Return [(actor key, base npc, residence location)] for every placed person."""
    w = W.w
    out = []
    unique_by_npc = collections.defaultdict(list)
    for npc, ref, loc in w["unique_rows"]:
        unique_by_npc[npc].append((ref, loc))
    achr_by_base = collections.defaultdict(list)
    for rfid, a in w["achr"].items():
        achr_by_base[a["base"]].append((rfid, a))

    stats = collections.Counter()
    for npc, n in w["npcs"].items():
        if not W.is_person(npc):
            continue
        if n["edid"] and NOT_A_RESIDENT.search(n["edid"]):
            stats["skipped by name"] += 1
            continue
        unique = bool(n["flags"] & 0x20)
        if unique:
            rows = unique_by_npc.get(npc)
            if rows:
                enabled = [r for r in rows if r[0] not in w["achr"]
                           or not W.effectively_disabled(w["achr"][r[0]])]
                out.append((npc, npc, (enabled or rows)[0][1]))
                stats["unique via LCUN"] += 1
                continue
            refs = achr_by_base.get(npc, [])
            refs = [r for r in refs if not W.effectively_disabled(r[1])] or refs
            if refs:
                loc = W.ref_location(refs[0][1])
                out.append((npc, npc, loc))
                stats["unique via placement"] += 1
            continue
        for rfid, a in achr_by_base.get(npc, []):
            if W.effectively_disabled(a):
                stats["generic, initially disabled"] += 1
                continue
            out.append((rfid, npc, W.ref_location(a)))
            stats["generic placements"] += 1
    return out, stats


def location_positions(W):
    """
    Mean exterior cell centre per location, counting a location's descendants'
    cells too, as (world root, x, y) in cells. Locations with no exterior cells
    fall back to their map marker.
    """
    acc = collections.defaultdict(list)
    for c in W.w["cells"].values():
        if c["interior"] or c["grid"] is None or not c["loc"] or not c["world"]:
            continue
        pt = (W.world_root(c["world"]), c["grid"][0] + 0.5, c["grid"][1] + 0.5)
        hold = W.hold_of(c["loc"])
        for a in W.ancestors(c["loc"]):
            if a == hold:
                break
            acc[a].append(pt)
    pos = {}
    for s, pts in acc.items():
        root = collections.Counter(p[0] for p in pts).most_common(1)[0][0]
        pts = [p for p in pts if p[0] == root]
        pos[s] = (root, statistics.mean(p[1] for p in pts), statistics.mean(p[2] for p in pts))
    for fid, l in W.w["locations"].items():
        r = W.w["refs"].get(l["marker"]) if l["marker"] else None
        if fid not in pos and r and r["world"] and r["pos"]:
            pos[fid] = (W.world_root(r["world"]), r["pos"][0] / CELL, r["pos"][1] / CELL)
    return pos


def place_under_hold(W, loc):
    """
    The ancestor directly below the hold (or below the world's root location,
    where there is no hold): the place someone lives, outside settlements.
    """
    chain = list(W.ancestors(loc))
    for i in range(len(chain) - 1, -1, -1):
        if W.loc_kw[chain[i]] & HOLD_KW:
            return chain[max(i - 1, 0)]
    return chain[-2] if len(chain) > 1 else chain[0]


def slug(edid):
    s = edid
    for suffix in ("Location", "Loc"):
        if s.endswith(suffix):
            s = s[: -len(suffix)]
    for prefix in ("DLC2", "DLC1", "BYOH"):
        if s.startswith(prefix):
            s = s[len(prefix):]
    return s.lower()


def build(W, strategy, floor):
    people, res_stats = residents(W)
    lpos = location_positions(W)

    persons = {}      # key -> info
    dropped = collections.Counter()
    enclave_count = collections.Counter()
    pending = []
    for key, npc, loc in people:
        name = W.w["npcs"][npc]["edid"]
        facs = W.factions(npc)
        if not loc:
            dropped["no location"] += 1
            continue
        if facs & HOSTILE_FACTIONS:
            dropped["hostile faction"] += 1
            continue
        if W.hostile(loc):
            dropped["hostile location"] += 1
            continue
        settlement = W.tier_of(loc, SETTLEMENT_KW)
        hold = W.hold_of(loc)
        kind = "settlement"
        if not settlement and (W.loc_kw[loc] & CIVIL_KW) and hold and loc in lpos:
            # A farm, mill or mine outside town: nearest settlement in the same hold.
            cands = [s for s in lpos if W.loc_kw[s] & SETTLEMENT_KW
                     and W.hold_of(s) == hold]
            if cands:
                settlement = min(cands, key=lambda s: dist(lpos[s], lpos[loc]))
        if not settlement:
            place = place_under_hold(W, loc)
            if W.edid(place) in NOT_ENCLAVES:
                dropped["no settlement"] += 1
                continue
            settlement, kind = place, "enclave"
            if key == npc:
                enclave_count[place] += 1
        pending.append((key, {"name": name, "npc": npc, "loc": loc, "settlement": settlement,
                              "hold": hold or W.hold_of(settlement), "fac": facs,
                              "unique": key == npc, "skind": kind}))
    for key, p in pending:
        if p["skind"] == "enclave" and enclave_count[p["settlement"]] < ENCLAVE_MIN:
            dropped["no settlement"] += 1
            continue
        persons[key] = p

    mills = collections.defaultdict(set)
    mill_info = {}

    def place_mill(p):
        s = p["settlement"]
        base = slug(W.edid(s))
        city = "LocTypeCity" in W.loc_kw[s]
        if strategy == "settlement" or not city:
            return base, p["skind"]
        f = p["fac"]
        if f & UNDERWORLD_FACTIONS:
            return base + ".underworld", "underworld"
        if f & COURT_FACTIONS:
            return base + ".court", "court"
        if f & GUILD_FACTIONS:
            return base + ".commons", "commons"
        if f & GUARD_FACTIONS or any(x.startswith(GUARD_FACTIONS_PREFIX) for x in f) \
                or W.tier_of(p["loc"], {"LocTypeBarracks"}):
            return base + ".garrison", "garrison"
        if W.tier_of(p["loc"], {"LocTypeCastle"}):
            return base + ".court", "court"
        return base + ".commons", "commons"

    for key, p in persons.items():
        mid, kind = place_mill(p)
        mills[mid].add(key)
        mill_info[mid] = {"kind": kind, "settlement": p["settlement"], "hold": p["hold"]}
        p["home"] = mid

    if strategy in ("institutions", "merged"):
        for iid, facs in INSTITUTIONS.items():
            members = {k for k, p in persons.items() if p["fac"] & facs}
            if len(members) < 2:
                continue
            # An institution that is really one place's mill adds nothing.
            biggest = max(len(members & ms) for ms in mills.values())
            if biggest >= 0.8 * len(members):
                continue
            mills[iid] = members
            mill_info[iid] = {"kind": "institution", "settlement": None, "hold": None}

    if strategy == "merged":
        def fold(mid, target):
            members = mills.pop(mid)
            mill_info.pop(mid)
            mills[target] |= members
            for k in members:
                if persons[k]["home"] == mid:
                    persons[k]["home"] = target

        # Small strata fold into their town's commons; small institutions go.
        for mid in [m for m in mills if len(mills[m]) < floor]:
            kind = mill_info[mid]["kind"]
            if kind == "institution":
                mills.pop(mid)
                mill_info.pop(mid)
            elif kind in ("court", "garrison", "underworld"):
                fold(mid, slug(W.edid(mill_info[mid]["settlement"])) + ".commons")
        # Small settlements and enclaves fold into the nearest bigger one in their
        # hold; a place with no position on the map, into the hold's biggest.
        place_mills = {m: i for m, i in mill_info.items() if i["kind"] in ("settlement", "commons", "enclave")}
        for mid in sorted(place_mills, key=lambda m: len(mills[m])):
            info = mill_info.get(mid)
            if not info or len(mills[mid]) >= floor:
                continue
            here = lpos.get(info["settlement"])
            cands = [m for m, i in mill_info.items()
                     if m != mid and i["kind"] in ("settlement", "commons", "enclave")
                     and i["hold"] == info["hold"] and len(mills[m]) >= floor
                     and (here is None or i["settlement"] in lpos)]
            if not cands:
                continue
            if here is None:
                fold(mid, max(cands, key=lambda m: len(mills[m])))
            else:
                fold(mid, min(cands, key=lambda m: dist(lpos[mill_info[m]["settlement"]], here)))

    # ---- edges
    edges = {}

    def edge(a, b, kind, d=0.0, w=1.0):
        if a == b or a not in mills or b not in mills:
            return
        k = tuple(sorted((a, b)))
        e = edges.get(k)
        if e is None or e["w"] < w:
            edges[k] = {"a": k[0], "b": k[1], "kind": kind, "dist": d, "w": w}

    member_of = collections.defaultdict(set)
    for mid, ms in mills.items():
        for k in ms:
            member_of[k].add(mid)
    shared = collections.Counter()
    for k, ms in member_of.items():
        ms = sorted(ms)
        for i in range(len(ms)):
            for j in range(i + 1, len(ms)):
                shared[(ms[i], ms[j])] += 1
    for (a, b), n in shared.items():
        edge(a, b, "shared", 0.0, float(n))

    by_settlement = collections.defaultdict(list)
    for mid, info in mill_info.items():
        if mid in mills and info["settlement"]:
            by_settlement[info["settlement"]].append(mid)
    strata_w = {("commons", "garrison"): 1.0, ("commons", "court"): 0.4, ("court", "garrison"): 0.7,
                ("commons", "underworld"): 0.7, ("garrison", "underworld"): 0.2,
                ("court", "underworld"): 0.1}
    for s, ms in by_settlement.items():
        for i in range(len(ms)):
            for j in range(i + 1, len(ms)):
                ka, kb = mill_info[ms[i]]["kind"], mill_info[ms[j]]["kind"]
                wgt = strata_w.get(tuple(sorted((ka, kb))), 0.3)
                edge(ms[i], ms[j], "strata", 0.0, wgt)

    # Roads: each settlement to its 3 nearest in the same world root.
    commons_of = {}
    for s, ms in by_settlement.items():
        commons_of[s] = next((m for m in ms if mill_info[m]["kind"] in ("commons", "settlement")), ms[0])
    settled = [s for s in by_settlement if s in lpos]
    for s in settled:
        near = sorted((t for t in settled if t != s and lpos[t][0] == lpos[s][0]),
                      key=lambda t: dist(lpos[s], lpos[t]))[:3]
        for t in near:
            d = dist(lpos[s], lpos[t])
            inn = any(W.tier_of(x, {"LocTypeHabitationHasInn"}) or "LocTypeCity" in W.loc_kw[x]
                      for x in (s, t))
            edge(commons_of[s], commons_of[t], "road", d, 1.0 if inn else 0.5)

    # Carriages between the settlements where drivers live; the ship to Solstheim.
    carriage = sorted({persons[k]["settlement"] for k in persons
                       if "CarriageSystemFaction" in persons[k]["fac"]})
    for i in range(len(carriage)):
        for j in range(i + 1, len(carriage)):
            a, b = carriage[i], carriage[j]
            if a in lpos and b in lpos and lpos[a][0] == lpos[b][0]:
                edge(commons_of[a], commons_of[b], "carriage", dist(lpos[a], lpos[b]), 0.6)
    slugs = {slug(W.edid(s)): s for s in commons_of}
    for a, b in SHIP_LINKS:
        if a in slugs and b in slugs:
            edge(commons_of[slugs[a]], commons_of[slugs[b]], "ship", 60.0, 0.5)

    # Hold fallback: every settlement's commons to the hold's largest settlement.
    capital = {}
    for s in commons_of:
        h = W.hold_of(s)
        if h and (h not in capital or len(mills[commons_of[s]]) > len(mills[commons_of[capital[h]]])):
            capital[h] = s
    for s in commons_of:
        h = W.hold_of(s)
        if h and capital[h] != s and s in lpos and capital[h] in lpos:
            edge(commons_of[s], commons_of[capital[h]], "hold", dist(lpos[s], lpos[capital[h]]), 0.15)

    return {
        "strategy": strategy,
        "persons": persons, "mills": mills, "mill_info": mill_info, "edges": list(edges.values()),
        "lpos": lpos, "dropped": dropped, "res_stats": res_stats, "W": W,
    }



def dist(a, b):
    if a[0] != b[0]:
        return 1e9
    return math.hypot(a[1] - b[1], a[2] - b[2])


def export(r, path):
    W = r["W"]
    hold_name = lambda h: slug(W.edid(h)) if h else None
    mills = {}
    for mid, ms in r["mills"].items():
        info = r["mill_info"].get(mid, {"kind": "commons", "settlement": None, "hold": None})
        s = info["settlement"]
        pos = r["lpos"].get(s) if s else None
        mills[mid] = {"kind": info["kind"], "settlement": slug(W.edid(s)) if s else None,
                      "hold": hold_name(info["hold"]), "pos": pos, "members": sorted(map(str, ms))}
    persons = {str(k): {"name": p["name"], "home": p["home"], "settlement": slug(W.edid(p["settlement"])),
                        "hold": hold_name(p["hold"]), "unique": p["unique"], "fac": sorted(p["fac"])}
               for k, p in r["persons"].items()}
    with open(path, "w") as f:
        json.dump({"strategy": r["strategy"], "mills": mills, "persons": persons,
                   "edges": r["edges"]}, f, indent=1)


def report(r, verbose=False):
    ms = r["mills"]
    sizes = sorted(len(v) for v in ms.values())
    covered = len(r["persons"])
    total = covered + sum(r["dropped"].values())
    memberships = collections.Counter()
    for v in ms.values():
        for k in v:
            memberships[k] += 1
    kinds = collections.Counter(r["mill_info"][m]["kind"] for m in ms if m in r["mill_info"])
    edge_kinds = collections.Counter(e["kind"] for e in r["edges"])
    print("\n== %s ==" % r["strategy"])
    print("  people placed      %d of %d candidates (%.1f%%)" % (covered, total, 100.0 * covered / total))
    print("    unique %d, generic %d" % (sum(p["unique"] for p in r["persons"].values()),
                                         sum(not p["unique"] for p in r["persons"].values())))
    print("  dropped            %s" % dict(r["dropped"]))
    print("  mills              %d  %s" % (len(ms), dict(kinds)))
    print("  mill size          min %d, median %d, p90 %d, max %d" % (
        sizes[0], statistics.median(sizes), sizes[int(0.9 * (len(sizes) - 1))], sizes[-1]))
    print("  size bands         1-2: %d, 3-5: %d, 6-15: %d, 16-40: %d, 41+: %d" % (
        sum(s <= 2 for s in sizes), sum(3 <= s <= 5 for s in sizes), sum(6 <= s <= 15 for s in sizes),
        sum(16 <= s <= 40 for s in sizes), sum(s > 40 for s in sizes)))
    print("  in 2+ mills        %d people" % sum(1 for n in memberships.values() if n > 1))
    print("  edges              %d  %s" % (len(r["edges"]), dict(edge_kinds)))
    if verbose:
        for mid in sorted(ms, key=lambda m: (r["mill_info"].get(m, {}).get("hold") or 0, m)):
            names = sorted(r["persons"][k]["name"] for k in ms[mid])
            print("  %-28s %3d  %s" % (mid, len(names), ", ".join(names[:14]) + (" …" if len(names) > 14 else "")))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--strategy", choices=STRATEGIES)
    ap.add_argument("--floor", type=int, default=4)
    ap.add_argument("--name", help="write cache/mills_<name>.json instead of the strategy's name")
    ap.add_argument("--world", help="read cache/world-<world>.pkl, e.g. p5 from extract.py --profile 5")
    ap.add_argument("--kid", help="apply this MO2 profile's KID location keywords, as the game does")
    ap.add_argument("-v", action="store_true")
    a = ap.parse_args()
    W = World(load_world(a.world))
    if a.kid:
        import kid
        print("KID: keywords added to %d locations" % kid.apply(W, a.kid))
    for s in ([a.strategy] if a.strategy else STRATEGIES):
        r = build(W, s, a.floor)
        report(r, a.v)
        export(r, os.path.join(HERE, "cache", "mills_%s.json" % (a.name or s)))
    if not a.strategy:
        print("\nfirst run's resident sources:", dict(r["res_stats"]))


if __name__ == "__main__":
    main()
