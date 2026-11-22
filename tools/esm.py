"""
Minimal reader for Skyrim SE plugin files (.esm/.esp), enough for the stage 0
prototypes: walk records with their group context, decode subrecords, and turn
file-local form IDs into load-order form IDs.

Nothing here knows about any particular record type; extract.py does that.
"""

import os
import struct
import zlib

SKYRIM_DATA = r"G:/SteamLibrary/steamapps/common/Skyrim Special Edition/Data"
VANILLA = ["Skyrim.esm", "Update.esm", "Dawnguard.esm", "HearthFires.esm", "Dragonborn.esm"]

# Group types (the int32 at offset 12 of a GRUP header).
GRUP_TOP = 0
GRUP_WORLD_CHILDREN = 1
GRUP_CELL_CHILDREN = 6
GRUP_CELL_PERSISTENT = 8
GRUP_CELL_TEMPORARY = 9

FLAG_COMPRESSED = 0x00040000


class Plugin:
    """One plugin file, with its master list resolved against a load order."""

    def __init__(self, path, load_order):
        self.name = os.path.basename(path)
        self.path = path
        with open(path, "rb") as f:
            self.data = f.read()
        if self.data[:4] != b"TES4":
            raise ValueError("%s is not a plugin" % path)
        hsize = struct.unpack_from("<I", self.data, 4)[0]
        self.masters = []
        for st, sd in subrecords(self.data[24:24 + hsize]):
            if st == b"MAST":
                self.masters.append(sd.split(b"\0")[0].decode("utf-8", "replace"))
        lower = {n.lower(): i for i, n in enumerate(load_order)}
        self.index = lower[self.name.lower()]
        self.master_index = [lower[m.lower()] for m in self.masters]
        self._header_end = 24 + hsize

    def gid(self, raw):
        """Load-order form ID for a form ID as written in this file."""
        if raw == 0:
            return 0
        hi = raw >> 24
        idx = self.master_index[hi] if hi < len(self.master_index) else self.index
        return (idx << 24) | (raw & 0xFFFFFF)

    def records(self, wanted, skip_groups=None):
        """
        Yield (type, raw_formid, flags, data, ctx) for every record whose type is
        in `wanted`. Compressed records are inflated. `ctx` is a dict with the
        enclosing world (raw id), cell (raw id) and cell-children kind, valid for
        the duration of the yield only.

        `skip_groups` is a set of top-level record types whose groups are skipped
        entirely, which saves time on big files.
        """
        data = self.data
        ctx = {"world": None, "cell": None, "kind": None}
        stack = []  # (end, saved ctx)

        p = self._header_end
        n = len(data)
        while p < n:
            while stack and p >= stack[-1][0]:
                _, saved = stack.pop()
                ctx.update(saved)
            typ = data[p:p + 4]
            if typ == b"GRUP":
                gsize, label, gtype = struct.unpack_from("<IIi", data, p + 4)
                if gtype == GRUP_TOP and skip_groups and data[p + 8:p + 12] in skip_groups:
                    p += gsize
                    continue
                stack.append((p + gsize, dict(ctx)))
                if gtype == GRUP_WORLD_CHILDREN:
                    ctx["world"] = label
                elif gtype == GRUP_CELL_CHILDREN:
                    ctx["cell"] = label
                elif gtype in (GRUP_CELL_PERSISTENT, GRUP_CELL_TEMPORARY):
                    ctx["cell"] = label
                    ctx["kind"] = gtype
                p += 24
                continue
            dsize, flags, formid = struct.unpack_from("<III", data, p + 4)
            if typ in wanted:
                d = data[p + 24:p + 24 + dsize]
                if flags & FLAG_COMPRESSED:
                    d = zlib.decompress(d[4:])
                yield typ, formid, flags, d, ctx
            p += 24 + dsize


def subrecords(d):
    """Yield (type, bytes) for each subrecord, honouring XXXX size overrides."""
    q = 0
    n = len(d)
    big = None
    while q + 6 <= n:
        st = d[q:q + 4]
        sl = struct.unpack_from("<H", d, q + 4)[0]
        q += 6
        if st == b"XXXX":
            big = struct.unpack_from("<I", d, q)[0]
            q += sl
            continue
        if big is not None:
            sl, big = big, None
        yield st, d[q:q + sl]
        q += sl


def zstr(b):
    return b.split(b"\0")[0].decode("cp1252", "replace")


def u32(b, off=0):
    return struct.unpack_from("<I", b, off)[0]


def u32s(b):
    return list(struct.unpack_from("<%dI" % (len(b) // 4), b, 0))


def load(names=VANILLA, data_dir=SKYRIM_DATA):
    order = list(names)
    return [Plugin(os.path.join(data_dir, n), order) for n in order]


MO2 = r"G:/MO2Data/SkyrimSE"


def profile_paths(profile, mo2=MO2, data_dir=SKYRIM_DATA):
    """
    An MO2 profile's active plugins in load order, as (name, path): the vanilla
    masters, then plugins.txt's active entries. Each file comes from the
    overwrite folder, else the highest-priority enabled mod that has it, else
    the game's Data folder, as MO2's virtual Data folder would serve it.
    """
    pdir = os.path.join(mo2, "profiles", str(profile))
    with open(os.path.join(pdir, "modlist.txt"), encoding="utf-8") as f:
        mods = [l[1:].strip() for l in f if l.startswith("+")]  # highest priority first
    where = {}
    for m in reversed(mods):
        d = os.path.join(mo2, "mods", m)
        if os.path.isdir(d):
            for fn in os.listdir(d):
                if fn.lower().endswith((".esm", ".esp", ".esl")):
                    where[fn.lower()] = os.path.join(d, fn)
    ow = os.path.join(mo2, "overwrite")
    if os.path.isdir(ow):
        for fn in os.listdir(ow):
            if fn.lower().endswith((".esm", ".esp", ".esl")):
                where[fn.lower()] = os.path.join(ow, fn)
    with open(os.path.join(pdir, "plugins.txt"), encoding="utf-8") as f:
        active = [l[1:].strip() for l in f if l.startswith("*")]
    # Creation Club plugins the game loads by itself, after the masters, when present.
    cc = []
    ccc = os.path.join(os.path.dirname(data_dir), "Skyrim.ccc")
    if os.path.exists(ccc):
        with open(ccc, encoding="utf-8") as f:
            cc = [l.strip() for l in f if l.strip()]
        cc = [n for n in cc if n.lower() in where or os.path.exists(os.path.join(data_dir, n))]
    names, seen = [], set()
    for n in VANILLA + cc + active:
        if n.lower() not in seen:
            seen.add(n.lower())
            names.append(n)
    return [(n, where.get(n.lower(), os.path.join(data_dir, n))) for n in names]


def load_profile(profile):
    pairs = profile_paths(profile)
    order = [n for n, _ in pairs]
    return [Plugin(p, order) for _, p in pairs]


def fid_str(gid, load_order=VANILLA):
    """'Skyrim.esm:0x01A694' style, for reports and ini files."""
    return "%s:0x%06X" % (load_order[gid >> 24], gid & 0xFFFFFF)
