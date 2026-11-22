"""
Apply an MO2 profile's Keyword Item Distributor rules for locations to a
mills.World, so the stage 0 builder sees the keywords the game has at runtime.

Only `Keyword = <keyword>|Location|<filters>` lines are read. A filter is a
location editor ID, or a keyword a location already has. A matched location's
children get the keyword too: that is what the game showed when OCF's
LocTypeHoldMajor rule for Eastmarch and Haafingar reached every house in them.
Keywords given as form IDs (0x123~Plugin.esp) are skipped.
"""

import os

import esm


def rules(profile, mo2=esm.MO2):
    """(keyword editor ID, [filters]) for every active location rule, in mod priority order."""
    with open(os.path.join(mo2, "profiles", str(profile), "modlist.txt"), encoding="utf-8") as f:
        mods = [l[1:].strip() for l in f if l.startswith("+")]
    out = []
    for m in reversed(mods):
        d = os.path.join(mo2, "mods", m)
        if not os.path.isdir(d):
            continue
        for fn in sorted(os.listdir(d)):
            if not fn.lower().endswith("_kid.ini"):
                continue
            with open(os.path.join(d, fn), encoding="utf-8", errors="replace") as f:
                for line in f:
                    line = line.strip()
                    if line.startswith(";") or "=" not in line:
                        continue
                    key, value = line.split("=", 1)
                    parts = [p.strip() for p in value.split("|")]
                    if key.strip().lower() != "keyword" or len(parts) < 3 or parts[1].lower() != "location":
                        continue
                    if "~" in parts[0] or not parts[2] or parts[2].lower() == "none":
                        continue
                    out.append((parts[0], [x.strip() for x in parts[2].split(",") if x.strip()]))
    return out


def apply(W, profile):
    """Add the profile's KID location keywords to W.loc_kw. Returns how many locations changed."""
    by_edid = {l["edid"]: fid for fid, l in W.w["locations"].items() if l["edid"]}
    children = {}
    for fid, l in W.w["locations"].items():
        children.setdefault(l["parent"], []).append(fid)

    def subtree(fid):
        stack, seen = [fid], set()
        while stack:
            x = stack.pop()
            if x in seen:
                continue
            seen.add(x)
            stack.extend(children.get(x, []))
        return seen

    changed = set()
    for keyword, filters in rules(profile):
        targets = set()
        for f in filters:
            if f in by_edid:
                targets |= subtree(by_edid[f])
            else:
                targets |= {fid for fid, kws in W.loc_kw.items() if f in kws}
        for fid in targets:
            if keyword not in W.loc_kw[fid]:
                W.loc_kw[fid].add(keyword)
                changed.add(fid)
    return len(changed)
