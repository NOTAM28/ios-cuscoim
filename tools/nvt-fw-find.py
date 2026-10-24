#!/usr/bin/env python3
"""Find Novatek NT36xxx touch firmware blobs built into a kernel Image.

  tools/nvt-fw-find.py dtb/boot_unpacked/kernel research/apollo-fw/

Xiaomi's apollo kernel requests novatek_nt36675_j3s_fw01.bin, but the file
is on no partition (vendor, odm, system, modem): it is compiled into the
kernel (CONFIG_EXTRA_FIRMWARE). This scans for blobs shaped like the files
nt36xxx_fw_update.c accepts:
  - a 4 KiB block ending in "NVT" or "MOD" (fw_need_write_size),
  - FW_VER + FW_VER_BAR == 0xFF at need_size - 4096,
  - a sane header: ILM/DLM <bin, sram, size> at 0/12, header end at 0x30+.
Each match is written as nvt-fw-<offset>-<size>.bin into OUTDIR.
"""
import os, struct, sys

ENDS = (b'NVT', b'MOD')


def word(b, o):
    return struct.unpack_from('<I', b, o)[0]


def plausible(img, start, need):
    if start < 0 or start + need > len(img) or need < 0x8000:
        return False
    b = img[start:start + need]
    if (b[need - 4096] + b[need - 4096 + 1]) != 0xff:
        return False
    hdr_end = word(b, 0)
    if not (0x30 <= hdr_end <= 0x400 and hdr_end % 4 == 0):
        return False
    ilm_bin, ilm_sram, ilm_size = word(b, 0), word(b, 4), word(b, 8)
    dlm_bin, dlm_sram, dlm_size = word(b, 12), word(b, 16), word(b, 20)
    if not (0 < ilm_size < need and 0 < dlm_size < need):
        return False
    if ilm_bin + ilm_size > need or dlm_bin + dlm_size > need or dlm_bin < ilm_bin:
        return False
    if ilm_sram >= 0x80000 or dlm_sram >= 0x80000:
        return False
    return True


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    img = open(sys.argv[1], 'rb').read()
    out = sys.argv[2]
    os.makedirs(out, exist_ok=True)
    found = set()
    for end_flag in ENDS:
        pos = img.find(end_flag)
        while pos >= 0:
            end = pos + 3  # end of a 4 KiB block in the blob
            for need in range(0x8000, 0x80001, 4096):
                start = end - need
                if start in found or not plausible(img, start, need):
                    continue
                found.add(start)
                b = img[start:start + need]
                name = os.path.join(out, f'nvt-fw-{start:#x}-{need:#x}.bin')
                open(name, 'wb').write(b)
                print(f'{name}: offset {start:#x} size {need:#x} ({need} bytes) '
                      f'version {b[need - 4096]:#04x} end "{end_flag.decode()}" '
                      f'ILM sram {word(b, 4):#x} size {word(b, 8):#x}  DLM sram {word(b, 16):#x} size {word(b, 20):#x}')
                break
            pos = img.find(end_flag, pos + 1)
    if not found:
        print('no NT36xxx firmware-shaped blob found')
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
