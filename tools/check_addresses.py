"""Check a new game version's Address Library against what the DLL can look up.

Collects every AE address ID the CommonLib source binds (RELOCATION_ID / RelocationID / VariantID), and
reports the ones the base table (1.6.1170 by default) has and a target table lacks. Those are the lookups that
would go wrong on the target. An ID whose 4-byte value appears in the DLL might be one RumourEngine uses; the
rest can't be. A match can be a coincidence, so look at the binding it names before deciding.

It can't see an ID that is already missing from the base table: CommonLib then quietly takes the next ID up.
That is how SKSE::log::log_directory() came to name the folder "Skyrim.INI" on 1.6.1130 and later, and why
RumourEngine works the folder out itself (Runtime::LogDirectory).

This is the check behind Runtime.cpp's list of 1.7 versions. To let another one through, run this on its table
and look at what CommonLib's own changes for that version say about the classes RumourEngine uses.

    python check_addresses.py --lib "<Address Library>/SKSE/Plugins" versionlib-1-7-104-0.bin versionlib-1-7-99-0.bin

Exits 1 if anything that appears in the DLL is missing and isn't in KNOWN_UNUSED.
"""
import argparse
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Bindings whose ID turns up in the DLL's bytes only by coincidence: RumourEngine never calls them.
KNOWN_UNUSED = {
    13358: 'TES::GetWaterHeight',
}


def read_format2(path):
    """Address Library format 2 (1.6.x): delta-encoded (id, offset) pairs."""
    d = path.read_bytes()
    p = 0

    def take(fmt):
        nonlocal p
        v = struct.unpack_from(fmt, d, p)
        p += struct.calcsize(fmt)
        return v[0] if len(v) == 1 else v

    if take('<i') != 2:
        raise ValueError(f'{path.name} isn\'t format 2')
    take('<4i')
    name_length = take('<i')
    p += name_length  # the executable's name
    pointer_size = take('<i')
    count = take('<i')

    def delta(code, previous):
        if code == 0: return take('<Q')
        if code == 1: return previous + 1
        if code == 2: return previous + take('<B')
        if code == 3: return previous - take('<B')
        if code == 4: return previous + take('<H')
        if code == 5: return previous - take('<H')
        if code == 6: return take('<H')
        if code == 7: return take('<I')
        raise ValueError(f'{path.name}: unknown encoding {code}')

    table = {}
    prev_id = prev_offset = 0
    for _ in range(count):
        kind = take('<B')
        id_ = delta(kind & 0xF, prev_id)
        scaled = kind & 0x80
        offset = delta((kind >> 4) & 7, prev_offset // pointer_size if scaled else prev_offset)
        if scaled:
            offset *= pointer_size
        table[id_] = offset
        prev_id, prev_offset = id_, offset
    return table


def read_format5(path):
    """Address Library format 5 (1.7.x): a u32 offset per ID after a 0x60-byte header; 0 means absent."""
    d = path.read_bytes()
    if struct.unpack_from('<i', d, 0)[0] != 5:
        raise ValueError(f'{path.name} isn\'t format 5')
    count = struct.unpack_from('<I', d, 0x5C)[0]
    if 0x60 + 4 * count != len(d):
        raise ValueError(f'{path.name}\'s length doesn\'t match its header')
    offsets = struct.unpack_from(f'<{count}I', d, 0x60)
    return {i: o for i, o in enumerate(offsets) if o}


def read_table(path):
    return read_format5(path) if struct.unpack_from('<i', path.read_bytes(), 0)[0] == 5 else read_format2(path)


def bindings(source):
    """AE ID -> (file, line, text) for every binding in the CommonLib source."""
    pair = re.compile(r'(?:RELOCATION_ID|RelocationID)\s*\(\s*(\d+)\s*,\s*(\d+)\s*\)')
    variant = re.compile(r'VariantID\s*\(\s*(\d+)\s*,\s*(\d+)\s*,')
    found = {}
    for f in sorted(list(source.rglob('*.h')) + list(source.rglob('*.cpp'))):
        text = f.read_text(encoding='utf-8', errors='replace')
        lines = text.splitlines()
        for m in list(pair.finditer(text)) + list(variant.finditer(text)):
            line = text.count('\n', 0, m.start()) + 1
            found.setdefault(int(m.group(2)), (f.relative_to(source), line, lines[line - 1].strip()))
    return found


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('targets', nargs='+', help='table file names in --lib to check')
    ap.add_argument('--lib', type=Path, required=True, help='the Address Library\'s SKSE/Plugins folder')
    ap.add_argument('--base', default='versionlib-1-6-1170-0.bin', help='the table the DLL is known to work on')
    ap.add_argument('--dll', type=Path, default=ROOT / 'SKSE' / 'build' / 'RumourEngine.dll')
    ap.add_argument('--commonlib', type=Path, help='CommonLib source (default: the vcpkg build tree)')
    args = ap.parse_args()

    source = args.commonlib
    if source is None:
        trees = sorted((ROOT / 'SKSE' / 'build' / 'vcpkg_installed' / 'vcpkg' / 'blds' / 'commonlibsse-ng' / 'src')
                       .glob('*.clean'))
        if not trees:
            sys.exit('No CommonLib source found: build the DLL once, or pass --commonlib')
        source = trees[-1]

    bound = bindings(source)
    dll = args.dll.read_bytes()
    base = read_table(args.lib / args.base)
    print(f'{len(bound)} AE IDs bound in {source.name}; base {args.base} has {len(base)} IDs')

    bad = False
    for name in args.targets:
        target = read_table(args.lib / name)
        missing = sorted(i for i in bound if i in base and i not in target)
        print(f'\n{name}: {len(target)} IDs; {len(missing)} bound IDs the base has are missing')
        for i in missing:
            where, line, text = bound[i]
            in_dll = struct.pack('<I', i) in dll
            note = ''
            if in_dll:
                note = f'  (known unused: {KNOWN_UNUSED[i]})' if i in KNOWN_UNUSED else '  <-- APPEARS IN THE DLL'
                bad = bad or i not in KNOWN_UNUSED
            print(f'  {i:>7}  {where}:{line}  {text[:90]}{note}')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
