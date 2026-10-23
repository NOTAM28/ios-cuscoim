#!/usr/bin/env python3
"""Extract one logical partition (e.g. vendor) from a raw Android super image.

  tools/lpextract.py super.raw vendor vendor.img
  tools/lpextract.py super.raw --list

The super image must be raw: convert a sparse one first with
`simg2img super.img super.raw`. Reads the primary liblp metadata (geometry
at 4096, metadata at 12288) and copies the partition's linear extents.
Used to take the NT36675 touch firmware from Xiaomi's fastboot ROM.
"""
import argparse, struct, sys

GEOMETRY_MAGIC = 0x616c4467
HEADER_MAGIC = 0x414c5030
SECTOR = 512


def read_metadata(f):
    f.seek(4096)
    magic, _size, _csum = struct.unpack('<II32s', f.read(40))
    if magic != GEOMETRY_MAGIC:
        sys.exit('not a raw super image (no liblp geometry at 4096; sparse? run simg2img first)')
    f.seek(4096 + 2 * 4096)
    hdr = f.read(256)
    magic, _major, _minor, header_size = struct.unpack_from('<IHHI', hdr, 0)
    if magic != HEADER_MAGIC:
        sys.exit('liblp metadata header not found')
    # header: magic, major, minor, header_size, header_checksum[32],
    # tables_size, tables_checksum[32], then 4 table descriptors
    off = 4 + 2 + 2 + 4 + 32 + 4 + 32
    tables = [struct.unpack_from('<III', hdr, off + 12 * i) for i in range(4)]
    f.seek(4096 + 2 * 4096 + header_size)
    body = f.read(max(o + n * s for o, n, s in tables))
    (po, pn, ps), (eo, en, es) = tables[0], tables[1]
    parts = []
    for i in range(pn):
        name, _attr, first, count, _group = struct.unpack_from('<36sIIII', body, po + i * ps)
        parts.append((name.split(b'\0')[0].decode(), first, count))
    extents = [struct.unpack_from('<QIQI', body, eo + i * es) for i in range(en)]
    return parts, extents


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('super')
    ap.add_argument('name', nargs='?')
    ap.add_argument('out', nargs='?')
    ap.add_argument('--list', action='store_true')
    a = ap.parse_args()
    with open(a.super, 'rb') as f:
        parts, extents = read_metadata(f)
        if a.list or not a.name:
            for name, first, count in parts:
                size = sum(extents[i][0] for i in range(first, first + count)) * SECTOR
                print(f'{name:24} {size / 1048576:10.1f} MiB')
            return 0
        match = [p for p in parts if p[0] == a.name]
        if not match:
            sys.exit(f'no partition {a.name}; have: ' + ', '.join(p[0] for p in parts))
        _, first, count = match[0]
        out = a.out or a.name + '.img'
        with open(out, 'wb') as o:
            for sectors, ttype, data, _src in extents[first:first + count]:
                if ttype == 0:  # linear
                    f.seek(data * SECTOR)
                    left = sectors * SECTOR
                    while left:
                        chunk = f.read(min(left, 1 << 24))
                        if not chunk:
                            sys.exit('super image truncated')
                        o.write(chunk)
                        left -= len(chunk)
                else:  # zero extent
                    o.seek(sectors * SECTOR, 1)
            o.truncate()
        print(f'{a.name} -> {out}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
