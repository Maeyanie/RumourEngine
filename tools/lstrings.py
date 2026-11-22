"""
Localized string tables for the vanilla masters: `Strings/<plugin>_<language>.(STRINGS|DLSTRINGS|ILSTRINGS)`,
loose in the Data folder or else from `Skyrim - Interface.bsa`. A localized plugin's FULL, NNAM and the like
hold an id into STRINGS; quest log entries (CNAM) and descriptions hold one into DLSTRINGS.

Only uncompressed archives are read, which the vanilla interface archive is.
"""

import os
import struct

from esm import SKYRIM_DATA

ARCHIVE = "Skyrim - Interface.bsa"


def _bsa_file(path, wanted):
    """The bytes of one file in a v105 (Skyrim SE) archive, by its path inside it."""
    wanted = wanted.lower().replace("/", "\\")
    with open(path, "rb") as f:
        magic, ver, _, flags, nfold, _, _, lfile, _ = struct.unpack("<4sIIIIIIII", f.read(36))
        if magic != b"BSA\0" or ver != 105:
            raise ValueError("%s is not a Skyrim SE archive" % path)
        if flags & 4:
            raise ValueError("%s is compressed, which this reader doesn't handle" % path)
        counts = [struct.unpack("<QIIQ", f.read(24))[1] for _ in range(nfold)]
        entries = []
        for count in counts:
            folder = ""
            if flags & 1:
                ln = f.read(1)[0]
                folder = f.read(ln)[:-1].decode("cp1252")
            for _ in range(count):
                _, size, off = struct.unpack("<QII", f.read(16))
                entries.append((folder, size, off))
        names = f.read(lfile).split(b"\0") if flags & 2 else []
        for (folder, size, off), name in zip(entries, names):
            if (folder + "\\" + name.decode("cp1252")).lower() == wanted:
                f.seek(off)
                data = f.read(size & 0x3FFFFFFF)
                if flags & 0x100:
                    data = data[1 + data[0]:]
                return data
    raise KeyError("%s not in %s" % (wanted, path))


def _parse(data, counted):
    count = struct.unpack_from("<I", data)[0]
    base = 8 + count * 8
    out = {}
    for k in range(count):
        sid, off = struct.unpack_from("<II", data, 8 + k * 8)
        p = base + off
        if counted:
            ln = struct.unpack_from("<I", data, p)[0]
            s = data[p + 4:p + 4 + ln]
        else:
            s = data[p:data.index(b"\0", p)]
        out[sid] = s.split(b"\0")[0].decode("cp1252", "replace")
    return out


def table(plugin, kind="strings", language="english", data_dir=SKYRIM_DATA):
    """{id: text} for one plugin's table, e.g. table("Skyrim.esm", "dlstrings")."""
    rel = "Strings/%s_%s.%s" % (os.path.splitext(plugin)[0], language, kind)
    loose = os.path.join(data_dir, rel)
    if os.path.exists(loose):
        with open(loose, "rb") as f:
            data = f.read()
    else:
        data = _bsa_file(os.path.join(data_dir, ARCHIVE), rel)
    return _parse(data, kind.lower() != "strings")
