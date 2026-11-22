"""
Pull what the mill builder needs out of the vanilla masters and cache it.

  python extract.py                # writes cache/world.pkl from the vanilla masters
  python extract.py --profile 5    # writes cache/world-p5.pkl from MO2 profile 5's load order
  python extract.py --summary      # also prints counts

Everything is keyed by load-order form ID (int). Later files override earlier
ones record by record, except a location's unique-NPC rows (LCUN/ACUN), which
are merged across files as NarrativeEngine found the engine does.
"""

import os
import pickle
import struct
import sys
import time

import esm

HERE = os.path.dirname(os.path.abspath(__file__))
CACHE = os.path.join(HERE, "cache", "world.pkl")

# Top-level groups that hold nothing we read; skipping them saves a lot of time.
SKIP = {b"DIAL", b"INFO", b"QUST", b"SCEN", b"PACK", b"IDLE", b"NAVI", b"NAVM",
        b"LAND", b"SOUN", b"SNDR", b"TXST", b"STAT", b"MSTT", b"SCOL", b"TREE",
        b"FLOR", b"FURN", b"WEAP", b"ARMO", b"ARMA", b"BOOK", b"MISC", b"ALCH",
        b"INGR", b"AMMO", b"KEYM", b"LIGH", b"ACTI", b"DOOR", b"CONT", b"SPEL",
        b"MGEF", b"ENCH", b"PERK", b"IMAD", b"IMGS", b"EFSH", b"ARTO", b"PROJ",
        b"HAZE", b"EXPL", b"DEBR", b"WTHR", b"CLMT", b"REGN", b"LSCR", b"MUSC",
        b"MUST", b"IPCT", b"IPDS", b"ANIO", b"WATR", b"COBJ", b"IDLM", b"HDPT",
        b"EYES", b"CLFM", b"MATT", b"MATO", b"SHOU", b"WOOP", b"DLVW", b"DLBR"}


def kwda(sd):
    return esm.u32s(sd)


def extract(plugins):
    keywords, factions, classes, races, worlds = {}, {}, {}, {}, {}
    locations, cells, npcs, lvln, achr = {}, {}, {}, {}, {}
    unique_rows = set()
    world_persistent = {}  # world gid -> persistent cell gid

    wanted = {b"KYWD", b"FACT", b"CLAS", b"RACE", b"WRLD", b"LCTN", b"CELL",
              b"NPC_", b"LVLN", b"ACHR"}

    for pl in plugins:
        t0 = time.time()
        g = pl.gid
        for typ, raw, flags, d, ctx in pl.records(wanted, SKIP):
            fid = g(raw)
            subs = list(esm.subrecords(d))
            edid = next((esm.zstr(sd) for st, sd in subs if st == b"EDID"), None)

            if typ == b"KYWD":
                keywords[fid] = edid
            elif typ == b"FACT":
                factions[fid] = edid
            elif typ == b"CLAS":
                classes[fid] = edid
            elif typ == b"RACE":
                kws = []
                for st, sd in subs:
                    if st == b"KWDA":
                        kws = [g(k) for k in kwda(sd)]
                races[fid] = {"edid": edid, "kw": kws}
            elif typ == b"WRLD":
                parent = loc = None
                for st, sd in subs:
                    if st == b"WNAM":
                        parent = g(esm.u32(sd))
                    elif st == b"XLCN":
                        loc = g(esm.u32(sd))
                worlds[fid] = {"edid": edid, "parent": parent, "loc": loc}
            elif typ == b"LCTN":
                loc = {"edid": edid, "parent": None, "kw": [], "marker": None}
                for st, sd in subs:
                    if st == b"PNAM":
                        loc["parent"] = g(esm.u32(sd))
                    elif st == b"KWDA":
                        loc["kw"] = [g(k) for k in kwda(sd)]
                    elif st == b"MNAM":
                        loc["marker"] = g(esm.u32(sd))
                    elif st in (b"LCUN", b"ACUN"):
                        for i in range(0, len(sd) - 11, 12):
                            a, r, l = struct.unpack_from("<III", sd, i)
                            unique_rows.add((g(a), g(r), g(l) if l else fid))
                locations[fid] = loc
            elif typ == b"CELL":
                c = {"edid": edid, "interior": False, "loc": None, "grid": None,
                     "world": g(ctx["world"]) if ctx["world"] else None}
                for st, sd in subs:
                    if st == b"DATA":
                        c["interior"] = bool(sd[0] & 0x01)
                    elif st == b"XCLC":
                        c["grid"] = struct.unpack_from("<ii", sd, 0)
                    elif st == b"XLCN":
                        c["loc"] = g(esm.u32(sd))
                if c["world"] and flags & 0x400:
                    world_persistent[c["world"]] = fid
                prev = cells.get(fid)
                if prev and c["world"] is None:
                    c["world"] = prev["world"]
                cells[fid] = c
            elif typ == b"NPC_":
                n = {"edid": edid, "flags": 0, "tflags": 0, "fac": [], "tpl": None,
                     "cls": None, "race": None, "kw": []}
                for st, sd in subs:
                    if st == b"ACBS":
                        n["flags"] = esm.u32(sd)
                        n["tflags"] = struct.unpack_from("<H", sd, 18)[0]
                    elif st == b"SNAM":
                        n["fac"].append((g(esm.u32(sd)), struct.unpack_from("<b", sd, 4)[0]))
                    elif st == b"TPLT":
                        n["tpl"] = g(esm.u32(sd))
                    elif st == b"CNAM":
                        n["cls"] = g(esm.u32(sd))
                    elif st == b"RNAM":
                        n["race"] = g(esm.u32(sd))
                    elif st == b"KWDA":
                        n["kw"] = [g(k) for k in kwda(sd)]
                npcs[fid] = n
            elif typ == b"LVLN":
                ents = []
                for st, sd in subs:
                    if st == b"LVLO":
                        ents.append(g(esm.u32(sd, 4)))
                lvln[fid] = ents
            elif typ == b"ACHR":
                a = {"base": None, "pos": None, "ploc": None, "esp": None,
                     "cell": g(ctx["cell"]) if ctx["cell"] else None,
                     "disabled": bool(flags & 0x800), "persistent": bool(flags & 0x400)}
                for st, sd in subs:
                    if st == b"NAME":
                        a["base"] = g(esm.u32(sd))
                    elif st == b"DATA":
                        a["pos"] = struct.unpack_from("<3f", sd, 0)
                    elif st == b"XLCN":
                        a["ploc"] = g(esm.u32(sd))
                    elif st == b"XESP":
                        # enable parent and whether this ref takes the opposite state
                        a["esp"] = (g(esm.u32(sd)), bool(sd[4] & 0x01))
                achr[fid] = a
        print("  %-16s %.1fs" % (pl.name, time.time() - t0))

    return {
        "keywords": keywords, "factions": factions, "classes": classes, "races": races,
        "worlds": worlds, "locations": locations, "cells": cells, "npcs": npcs,
        "lvln": lvln, "achr": achr, "unique_rows": sorted(unique_rows),
        "world_persistent": world_persistent,
    }


def other_refs(plugins, wanted_refs):
    """
    Second pass over REFRs, keeping only `wanted_refs`: location map markers and
    the enable parents of placed actors. Returns {fid: {pos, world, disabled, esp}}.
    """
    out = {}
    for pl in plugins:
        for typ, raw, flags, d, ctx in pl.records({b"REFR", b"ACHR"}, SKIP | {b"NPC_", b"LVLN"}):
            fid = pl.gid(raw)
            if fid not in wanted_refs:
                continue
            r = {"pos": None, "world": pl.gid(ctx["world"]) if ctx["world"] else None,
                 "disabled": bool(flags & 0x800), "esp": None}
            for st, sd in esm.subrecords(d):
                if st == b"DATA":
                    r["pos"] = struct.unpack_from("<3f", sd, 0)
                elif st == b"XESP":
                    r["esp"] = (pl.gid(esm.u32(sd)), bool(sd[4] & 0x01))
            out[fid] = r
    return out


def main():
    profile = sys.argv[sys.argv.index("--profile") + 1] if "--profile" in sys.argv else None
    plugins = esm.load_profile(profile) if profile else esm.load()
    cache = os.path.join(HERE, "cache", "world-p%s.pkl" % profile) if profile else CACHE
    print("%d plugins" % len(plugins))
    print("pass 1: records")
    w = extract(plugins)
    print("pass 2: location markers and enable parents")
    wanted = {l["marker"] for l in w["locations"].values() if l["marker"]}
    wanted |= {a["esp"][0] for a in w["achr"].values() if a["esp"]}
    refs = other_refs(plugins, wanted)
    # Enable parents can have parents of their own; fetch until the chain ends.
    for _ in range(4):
        more = {r["esp"][0] for r in refs.values() if r["esp"]} - set(refs)
        if not more:
            break
        refs.update(other_refs(plugins, more))
    w["refs"] = refs
    os.makedirs(os.path.dirname(cache), exist_ok=True)
    with open(cache, "wb") as f:
        pickle.dump(w, f, protocol=pickle.HIGHEST_PROTOCOL)
    print("wrote", cache)
    if "--summary" in sys.argv:
        for k, v in w.items():
            print("  %-16s %d" % (k, len(v)))


if __name__ == "__main__":
    main()
