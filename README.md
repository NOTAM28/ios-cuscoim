# ios-apollo

**Porting ios-sweet (Apple's iOS 16 on a Snapdragon Android phone) to the Xiaomi Mi 10T 5G / Mi 10T Pro 5G (`apollo`, Qualcomm SM8250 / Snapdragon 865).**

This repository is a fork of **ios-sweet**. ios-sweet builds Apple's open-source XNU kernel (xnu-8796, the iOS 16 generation) for the Redmi Note 10 Pro (`sweet`, SM7150), then runs the **unmodified iOS 16.7.16 (20H392) userland of the iPhone X** on it: launchd, the system daemons, backboardd, SpringBoard and the stock apps. On `sweet` it reaches the iOS home screen with touch, buttons, all eight cores and USB networking.

This fork moves that work to `apollo`. As of **2026-10-23** the Mi 10T boots XNU on six cores and reaches **iOS Setup**. Power, volume-up and volume-down work. Touch is the last missing input: SPI to the touch chip works and its ID reads back, and the firmware loader is written but not yet tested. The section [apollo port status](#apollo-port-status) has the details.

> Experimental bring-up, not a usable phone OS. Apple's security model is mostly switched off, and installing the iOS root **erases Android's `userdata` partition**.

## apollo port status

| Area | State on apollo |
| --- | --- |
| Boot | Stock bootloader → `fastboot boot` shim → XNU → iOS 16.7 userland from `userdata` → **Setup (Siri page) on screen**. Kernel log readable from recovery |
| Boot shim | GIC, PSCI, IMEM restart reason and timer PPIs retargeted to SM8250 (virtual timer INTID 27, physical timer INTID 30, restart reason at `0x146bf65c`) |
| Device tree (ADT) | `tools/mkadt.py`: 1080×2400 framebuffer at `0x9c000000` (35 MiB splash region), virtual timer INTID 27, 4 GiB from `0x1_00000000`, root compatible gains `xiaomi,apollo` / `qcom,sm8250` (`xiaomi,sweet` stays: `SweetPlatformExpert` matches it). `--touch-fw` embeds the touch firmware |
| CPU | `cpus=6` boots and is noticeably faster than `cpus=1`; `cpus=8` untested (SM8250: 4×A55 + 4×A77) |
| Buttons | **Working** (0082, 0084): power and volume-down on the PON block, volume-up on PM8150 GPIO6 (PPID `0x0c5`), probed automatically; `sweetvolup=0xPPID` overrides |
| Touch | **In progress** (0082–0093), Novatek NT36675 on QUP0 SE4 SPI (`spi@990000`), reset GPIO 38, IRQ GPIO 39 (rising edge). Working: the GPIO pins (south TLMM tile), SE4 clocks (GCC votes, DFS level 0 = 19.2 MHz), SPI pin mux (GPIO 8–11 → `qup4`), the SPI engine setup and the chip-ID read (`0e 00 00 72 66 03` = NT36675). Written, **not yet tested**: the firmware download (0093). The chip has no flash, so its firmware (`novatek_nt36675_j3s_fw01.bin`, from Xiaomi's fastboot ROM `vendor` image, not in git) must be loaded at every boot |
| Display | Kernel text and the iOS GUI on the bootloader framebuffer. A bottom band of the Setup page is drawn red (unexplained) |
| Not ported yet | Display power (0079), USB device mode / Ethernet (0072–0077), battery, the `/memory` size check |

The next steps and the full engineering history are in [`NOTES.md`](NOTES.md).

## What ios-sweet achieved on `sweet` (the baseline)

| Area | State on sweet (as of 2026-10-18) |
| --- | --- |
| Boot | Stock Xiaomi bootloader → `fastboot boot` of our shim → XNU. Nothing is flashed to the boot partitions |
| CPU | All 8 cores (six Cortex-A55 + two Cortex-A76) via PSCI, 16K pages, GICv3 interrupts, timers |
| Memory | About 3.95 GiB of the upper DDR bank |
| Storage | Own UFS host driver; the iOS root is a journaled HFS+ volume in `userdata`, read/write |
| iOS userland | Real iPhone X 16.7.16 root filesystem and shared cache; launchd and the system daemons run |
| Display | Kernel IOSurface + IOMobileFramebuffer (`AppleCLCD`); CoreAnimation renders in software; frames copied to the bootloader framebuffer; presented as an iPhone X panel (1125×2436); display off/on with the power button |
| GUI | backboardd, SpringBoard, lock screen, home screen, Control Center, notification center, Settings, app switcher, home gesture |
| Input | Goodix GT9896 touch over GENI I²C; power and volume keys over SPMI; all delivered as HID events |
| USB | DWC3 device mode, CDC-ECM Ethernet (`ue0`, 192.168.7.2), live kernel log streamed over UDP |
| Identity / activation | iPhone X (`iPhone10,3` / `D22AP`); mobileactivationd's native "Hactivation" path, no Apple servers |

Not working on sweet either: Wi-Fi, Bluetooth, cellular, audio, camera, sensors, GPS, SEP / Face ID and the GPU. Rendering is done on the CPU at about 20–30 fps, the first boot takes about 4 minutes, and iOS does not use the USB link for internet yet. Code signing (AMFI), the sandbox and data protection are permissive or stubbed.

## How it boots

```
ABL (stock Xiaomi bootloader, unlocked)
  -> fastboot boot shim-boot.img              (temporary; nothing is flashed to boot)
     -> shim: own MMU setup, reads the live FDT, loads the payload from the ramdisk
        -> XNU Mach-O + generated Apple Device Tree (ADT) + boot-args
           -> XNU -> UFS driver -> HFS+ root in the userdata partition
              -> dyld + shared cache -> launchd -> daemons -> backboardd + SpringBoard
```

There is no usable serial port. The kernel draws its console into the framebuffer that ABL leaves at `0x9c000000` and keeps a persistent 4 MiB log ring in the last 1025 blocks of `userdata`. `tools/sweet-log-pull.sh` reads that ring from recovery. Once USB comes up (`sweetusb=2`), `tools/sweet-usbnet.sh` streams the log live instead.

## Build and boot

You need an **unlocked** device, a Linux PC and a GitHub account (the kernel builds on GitHub's macOS runners). **Everything on `userdata` is lost.** Bootloaders, recovery, modem and persist are never touched.

### 0. Back up the phone

Boot a recovery with adb root temporarily and pull at least `persist`, `modemst1`, `modemst2`, `fsg`, `fsc`, `boot`, `dtbo` and `vbmeta`. They hold per-device calibration data and serial numbers, so keep them out of git (`backup/`, `dtb/` and `recovery/` are already in `.gitignore`). The shim needs the stock DTB:

```sh
python3 tools/unpack_bootv2.py backup/boot.img dtb/boot_unpacked   # -> dtb/boot_unpacked/dtb
```

### 1. Validate, then build the kernel

```sh
python3 tools/validate-repo.py        # seconds; CI runs it before the macOS build
gh workflow run xnu.yml -R YOU/ios-apollo
gh run download RUN_ID -R YOU/ios-apollo -n xnu-kernel -D xnu/out
```

The workflow fetches xnu-8796 via [blacktop/darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build), applies `xnu/patches/` in order and stages the pinned open-source libpthread, HFS and BearSSL sources. `validate-repo.py` checks patch numbering, that every hunk matches its `@@` header, and that every file the series creates (`bsd/dev/sweet_*.c`, …) still applies through every later patch. A hand-edited or stale patch fails there in under a second. `tools/xnu-cc.py` replays single-file compiles from a previous CI log.

### 2. Build the iOS root image

From the iPhone X (iPhone10,3) iOS 16.7.16 (20H392) IPSW, on Linux with `apfs-fuse`, `hfsprogs` and sudo:

```sh
tools/mkiosroot.sh OS.dmg SystemOS.dmg AppOS.dmg ios16-root.img 16G
sudo fsck.hfsplus -fy ios16-root.img
sudo mount -t hfsplus -o loop,force ios16-root.img /mnt/iosroot
sudo python3 tools/sweet-root-tweaks.py /mnt/iosroot
sudo umount /mnt/iosroot
img2simg ios16-root.img ios16-root.simg
```

`sweet-root-tweaks.py` applies the bring-up changes: Setup skip, seeded activation state, no baseband daemon, no app watchdogs, and others. Its header explains each one. The ADT builder copies `/product` from the real iPhone X device tree (MobileGestalt needs it):

```sh
python3 tools/remotezip.py IPSW_URL DeviceTree.d22ap.im4p research/ipsw-x/dt
ipsw img4 im4p extract research/ipsw-x/dt/Firmware/all_flash/DeviceTree.d22ap.im4p -o research/ipsw-x/d22ap.adt
```

### 3. Build the boot image

Needs `aarch64-linux-gnu-gcc` and `mkbootimg`.

```sh
python3 tools/mkadt.py shim/out/apollo.adt --highmem --touch-fw research/apollo-fw/novatek_nt36675_j3s_fw01.bin
python3 tools/mkpayload.py xnu/out/kernel.development.sweet shim/out/apollo.adt shim/out/payload.bin \
    --cmdline 'debug=0xc serial=3 cpus=6 keepsyms=1 -enable_kprintf_spam -noprogress rd=ufsroot0 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin ufsrw=1 rootrw=1 launchd_unsecure_cache=1 wdt=-1 sweetfb_noconsole=1 sweettouchstage=4 sweettouchdelay=20 sweettouch_debug=1 ufslogsecs=1'
shim/build.sh ../shim/out/payload.bin      # -> shim/out/shim-boot.img
```

The touch firmware comes from Xiaomi's fastboot ROM. On Linux: run `simg2img super.img super.raw`, then `tools/lpextract.py super.raw vendor vendor.img`, mount `vendor.img` read-only and copy `firmware/novatek_nt36675_j3s_fw01.bin` to `research/apollo-fw/`. `research/` is in `.gitignore`. Without `--touch-fw`, the kernel boots normally and touch stops before the firmware download.

| Boot-arg | Effect |
| --- | --- |
| `rd=ufsroot0 ufsrw=1 rootrw=1` | Root on the `userdata` partition, writable |
| `cpus=N` | Number of cores to start; `cpus=1` is the safe fallback |
| `launchd_unsecure_cache=1` | launchd accepts its job cache without AMFI |
| `wdt=-1 wdt_test=no_user_checkin` | No hardware watchdog |
| `sweetfb_noconsole=1`, `sweetfb_hz=30` | Stop drawing kernel text over the GUI; panel copy rate |
| `sweettouch=0`, `sweetkeys=0` | Disable the touch or button driver |
| `sweettouchstage=1..4` | Touch bring-up depth: 1 reads the pins, 2 reads the SPI engine, 3 pulses reset, 4 sets up clocks, pins and SPI, reads the chip ID, loads the firmware and polls for reports (default 1) |
| `sweettouchdelay=S`, `sweettouch_debug=1` | Touch start delay in seconds (default 45); log every raw report |
| `sweettouchfw=0`, `sweettouchmux=0` | Skip the firmware download or the SPI pin mux |
| `sweettouchse=`, `sweettouchrst=`, `sweettouchirq=`, `sweettouchtlmm=`, `sweettouchdiv=`, `sweettouchbuf=` | Override the SPI engine address, reset/IRQ GPIOs, TLMM tile, SPI clock divider (default 4 = 4.8 MHz) and event buffer address |
| `ufslogsecs=N` | Persistent-log flush interval in seconds (use 1 when chasing hangs) |
| `sweetvolup=0xPPID` | PMIC GPIO peripheral for volume-up (SID << 8 \| peripheral) |
| `sweetusb=2` | USB device mode, CDC-ECM Ethernet and the live log |

### 4. Flash the root and boot

```sh
fastboot -S 512M flash userdata ios16-root.simg   # ~7 minutes; do not unplug
fastboot boot shim/out/shim-boot.img
```

On sweet, Xiaomi's fastboot often hangs after a large flash. Re-enter fastboot with Power + Volume down. The first boot sits on the Apple logo for 4–5 minutes.

### 5. Read the kernel log

```sh
fastboot boot recovery/recovery.img        # temporary recovery with adb
tools/sweet-log-pull.sh logs/boot.txt      # 4 MiB console ring from the end of userdata
adb reboot bootloader
```

The driver lines start with `SWEET ...`, and crash reports start with `CRASH name ...`. `tools/sweet-crash.py` symbolicates the crash reports against the shared cache. `tools/qemu-sweet.py` boots the same kernel on QEMU `virt` for kernel-only changes; see [`docs/QEMU_TESTING.md`](docs/QEMU_TESTING.md).

## Repository layout

| Path | What |
| --- | --- |
| `shim/` | Bare-metal boot shim (C + asm) packed as an Android boot.img v2 |
| `xnu/patches/` | The XNU patch series (0001–0093, applied in order by CI) plus staging scripts for libpthread, HFS and BearSSL |
| `.github/workflows/xnu.yml` | `validate` (Ubuntu, seconds), then the kernel `build` (macOS) |
| `tools/validate-repo.py` | Patch-series and script checks (numbering, hunk headers, replay of series-created files) |
| `tools/mkadt.py` | Apple Device Tree from the board's MMIO layout and the iPhone X identity |
| `tools/mkpayload.py` | Packs kernel + ADT + boot-args into the shim payload |
| `tools/mkiosroot.sh`, `sweet-root-tweaks.py` | HFS+ iOS root from the IPSW images, and the bring-up changes to it |
| `tools/sweet-log-pull.sh`, `sweet-log.py`, `sweet-usbnet.sh` | Persistent log from recovery; live log over USB Ethernet |
| `tools/sweet-crash.py` | Symbolicate console crash reports |
| `tools/xnu-cc.py` | Local single-file compile check from a CI log |
| `tools/qemu-sweet.py` | QEMU test boots with framebuffer text decoding |
| `tools/lpextract.py` | Extract one partition (e.g. `vendor`) from a raw Android `super` image |
| `tools/adt.py`, `remotezip.py`, `unpack_bootv2.py`, `sparsediff.py`, `check-exports.py` | Helpers |
| `site/` | The project website |
| `NOTES.md` | Engineering log: every boot, crash and decision, hardware facts, the active handoff |

Not in git: partition backups, live device trees, recovery images, Apple firmware and root images, CI build trees and pulled logs. They contain device serials and calibration data, or Apple's copyrighted files.

## Kernel patches

| # | What |
| --- | --- |
| 0001–0009 | Toolchain compatibility, SWEET machine config, non-PAC arm64, stubs for Apple-internal pieces |
| 0010–0018 | Early framebuffer console, physmap fix, PRNG without corecrypto, device-tree platform expert, timer PPI, boot CPU driver |
| 0019–0020 | Apple open-source libpthread kernel provider; BearSSL AES |
| 0021–0027 | RAM-disk root, mockfs, SHA-256, minimal AMFI, Apple open-source HFS+, MD5 |
| 0028–0034 | UFS host driver and root device, read/write root, DMA above 4 GiB |
| 0035–0045 | Entitlements, bindfs, AppleKeyStore stub, IOWatchdog, console crash reports |
| 0046 | IOSurface (IOCoreSurfaceRoot) and IOMobileFramebuffer (AppleCLCD) services |
| 0047–0055 | Diagnostics: persistent log, /dev/null tee, IOKit trace, heap strings and user stack sampler on crash; permissive Sandbox policy; HFS rename-swap; AppleKeyStore selectors |
| 0056–0057 | Throttled panel copy; no external identity resolver |
| 0058 | HID event services (Apple IOHIDFamily-1915.140.3 user client and event queue) |
| 0059 | iPhone X identity (`hw.product` / `hw.target` from the device tree) |
| 0060 | sweet power/volume keys over SPMI; touch over GENI I²C |
| 0061 | Present the display as 1125×2436 |
| 0062–0063 | RPMh touch rail; battery power source; GENI FIFO packing and the Goodix GT9896 protocol |
| 0064 | Grant PRIV_WORK_INTERVAL (UIKit's CoreAnimation work interval) |
| 0065 | SMP: secondary cores via PSCI CPU_ON |
| 0066–0069 | Fault-region crash info; console mute; IOSurface lookup fix; batched persistent-log flushes |
| 0070–0071 | `pid_hibernate` syscall (leaving apps); edge-swipe flags on touches (home gesture) |
| 0072–0077 | USB: DWC3 probe, ep0, VBUS override, CDC-ECM Ethernet `ue0`, RX into cluster mbufs, live log over UDP |
| 0078–0081 | All eight cores; display power off/on; network route/address trace; Ethernet link state |
| **0082** | **apollo:** Novatek NT36675 SPI touch driver (first version); volume-up PPID fallback and `sweetvolup=` |
| **0083–0087** | **apollo:** touch bring-up in logged stages (`sweettouchstage`); volume-up probe (finds PM8150 GPIO6); TLMM tile search; SPI engine firmware/clock logging; selectable SE |
| **0088–0089** | **apollo:** touch driver from the live device tree and Xiaomi's source: SE4 `0x990000`, GPIO 38/39, NT36xxx SPI framing, chip-ID read, event buffer `0x22D00` |
| **0090–0092** | **apollo:** SE4 clock votes in GCC (hardware-gated branches accepted); SPI pin mux GPIO 8–11 → `qup4` |
| **0093** | **apollo:** NT36675 firmware host download with hardware CRC (firmware from the ADT) |

## Key hardware facts

| | sweet (SM7150) | apollo (SM8250) |
| --- | --- | --- |
| GIC | GICv3, `0x17a00000` / `0x17a60000` | same addresses |
| Virtual timer | INTID 19 (PPI 3) | INTID 27 (PPI 11) |
| Framebuffer | `0x9c000000`, 1080×2400 | `0x9c000000`, 1080×2400, 35 MiB splash region |
| Touch | Goodix GT9896, I²C `0x5d`, QUP SE7 `0xa84000` | Novatek NT36675 (flashless), SPI on QUP0 SE4 `0x990000` (pins GPIO 8–11, `qup4`), reset GPIO 38 / IRQ GPIO 39 in the south TLMM tile `0x0f500000` |
| Touch clocks | Left on by ABL | Off under `fastboot boot`: GCC `0x100000`, vote `0x52008` bits 6–9 and 14, SE4 RCG `0x174d0` (DFS) |
| Volume-up | PM6150L GPIO2 (PPID `0x4c1`) | PM8150 GPIO6 (PPID `0x0c5`) |
| CPUs | 6×A55 + 2×A76 | 4×A55 + 4×A77 |

Both: PSCI through `smc`, 19.2 MHz timer, no pointer authentication, UFS host at `0x1d84000`. Details in [`NOTES.md`](NOTES.md).

## Credits

Built on [ios-sweet](https://github.com/dipa-studio/ios-sweet) and Apple's open-source releases (xnu, IOHIDFamily, libpthread, hfs), [BearSSL](https://bearssl.org), [blacktop/darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build), [ipsw](https://github.com/blacktop/ipsw), Qualcomm/Xiaomi's published Linux kernel sources for the hardware details, and [OrangeFox Recovery](https://orangefox.download). iOS itself is Apple's and is not distributed here. Licensed under the MIT License (see `LICENSE`).
