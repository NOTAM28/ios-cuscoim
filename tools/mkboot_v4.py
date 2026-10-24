#!/usr/bin/env python3
"""Minimal Android boot.img header v4 writer (kernel + ramdisk)."""
import argparse, struct
ap = argparse.ArgumentParser()
ap.add_argument('--kernel', required=True)
ap.add_argument('--ramdisk', required=True)
ap.add_argument('--stock', help='stock boot.img to copy os_version from')
ap.add_argument('--cmdline', default='')
ap.add_argument('-o', required=True)
a = ap.parse_args()
PAGE = 4096
k = open(a.kernel, 'rb').read()
r = open(a.ramdisk, 'rb').read()
osv = 0
if a.stock:
    osv = struct.unpack_from('<I', open(a.stock, 'rb').read(64), 16)[0]
pad = lambda b: b + b'\0' * (-len(b) % PAGE)
cmd = a.cmdline.encode()
assert len(cmd) < 1536
hdr = b'ANDROID!' + struct.pack('<IIII', len(k), len(r), osv, 1584) + b'\0' * 16
hdr += struct.pack('<I', 4) + cmd.ljust(1536, b'\0') + struct.pack('<I', 0)
assert len(hdr) == 1584
open(a.o, 'wb').write(pad(hdr) + pad(k) + pad(r))
print('wrote', a.o, 'kernel', len(k), 'ramdisk', len(r), 'os_version', hex(osv))
