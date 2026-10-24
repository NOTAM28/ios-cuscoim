#!/usr/bin/env python3
"""List the blocks of an Adreno GMU firmware file (a6xx block format).

  tools/gmu-fw-info.py research/apollo-gpu/a650_gmu.bin

Each block is <addr, size, type, value> (little-endian u32) followed by
size bytes. size 0 = metadata: type 1/7 asks for value bytes of memory at
addr, types 2-6 carry versions. kgsl: drivers/gpu/msm/kgsl_gmu.h.
"""
import struct, sys

TYPES = {0: 'data', 1: 'prealloc', 2: 'core ver', 3: 'core dev ver', 4: 'pwr ver',
         5: 'pwr dev ver', 6: 'hfi ver', 7: 'prealloc keep'}


def where(addr):
    if addr < 0x4000:
        return 'ITCM'
    if 0x10004000 <= addr < 0x10008000:
        return 'DTCM'
    if addr < 0x01000000:
        return 'cache'
    if 0x60000000 <= addr < 0x80000000:
        return 'noncached kernel'
    return '?'


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    data = open(sys.argv[1], 'rb').read()
    at = 0
    while at + 16 <= len(data):
        addr, size, typ, value = struct.unpack_from('<4I', data, at)
        print(f'@{at:#07x} addr={addr:#010x} size={size:#07x} type={typ} ({TYPES.get(typ, "?")}) '
              f'value={value:#x}  [{where(addr)}]')
        at += 16 + size
    print(f'{len(data)} bytes, ends at {at:#x}')


if __name__ == '__main__':
    main()
