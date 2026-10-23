# ios-apollo

**Porting ios-sweet (Apple's iOS 16 on a Snapdragon Android phone) to the Xiaomi Mi 10T 5G / Mi 10T Pro 5G (`apollo`, Qualcomm SM8250 / Snapdragon 865).**

This repository is a fork of **ios-sweet**. ios-sweet builds Apple's open-source XNU kernel (xnu-8796, the iOS 16 generation) for the Redmi Note 10 Pro (`sweet`, SM7150), then runs the **unmodified iOS 16.7.16 (20H392) userland of the iPhone X** on it: launchd, the system daemons, backboardd, SpringBoard and the stock apps. On `sweet` it reaches the iOS home screen with touch, buttons, all eight cores and USB networking.

This fork moves that work to `apollo`. The boot shim, the device-tree generator and the touch and button drivers have been retargeted. **apollo has not booted to iOS yet.** The section [apollo port status](#apollo-port-status) lists what has changed and what is still unverified.

> Experimental bring-up, not a usable phone OS. Apple's security model is mostly switched off, and installing the iOS root **erases Android's `userdata` partition**.

## apollo port status

| Area | State on apollo |
| --- | --- |
| Boot shim | GIC, PSCI, IMEM restart reason and timer PPIs retargeted to SM8250 (virtual timer INTID 27, physical timer INTID 30, restart reason at `0x146bf65c`). Builds cleanly |
| Device tree (ADT) | `tools/mkadt.py`: 1080×2400 framebuffer at `0x9c000000` (`cont_splash_region`, 35 MiB), virtual timer INTID 27, 4 GiB from `0x1_00000000`, root compatible gains `xiaomi,apollo` / `qcom,sm8250`. `xiaomi,sweet` stays because `SweetPlatformExpert` (patch 0016) matches only that string |
| Touch | **Patch 0082**: Novatek NT36675 over GENI SPI (`spi@994000`, reset on TLMM GPIO 8, IRQ on GPIO 9) replaces sweet's Goodix GT9896 I²C driver. It uses full-duplex FIFO transfers and NT36xxx read framing, starts after `sweettouchdelay` seconds and logs the first raw report. Builds in the series; **not tested on the phone** |
| Buttons | **Patch 0082**: power and volume-down on the PON block as on sweet. Volume-up tries sweet's PMIC GPIO (PPID `0x4c1`) and then `0x1c1`, and `sweetvolup=PPID` selects any other PMIC GPIO without a rebuild. **Not tested on the phone** |
| Kernel log | The first log pull from apollo (2026-10-23) contains only zero bytes, so the kernel did not write its persistent log. The cause is not known yet |
| Not ported yet | Display power (0079), USB device mode / Ethernet (0072–0077), battery, SMP layout (SM8250 has 4×A55 + 4×A77, sweet had 6×A55 + 2×A76), the `/memory` size check |

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
python3 tools/mkadt.py shim/out/apollo.adt --highmem
python3 tools/mkpayload.py xnu/out/kernel.development.sweet shim/out/apollo.adt shim/out/payload.bin \
    --cmdline 'debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress rd=ufsroot0 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin ufsrw=1 rootrw=1 launchd_unsecure_cache=1 wdt=-1 sweettouch_debug=1'
shim/build.sh ../shim/out/payload.bin      # -> shim/out/shim-boot.img
```

On apollo, start with `cpus=1` until the SMP layout has been checked.

| Boot-arg | Effect |
| --- | --- |
| `rd=ufsroot0 ufsrw=1 rootrw=1` | Root on the `userdata` partition, writable |
| `cpus=N` | Number of cores to start; `cpus=1` is the safe fallback |
| `launchd_unsecure_cache=1` | launchd accepts its job cache without AMFI |
| `wdt=-1 wdt_test=no_user_checkin` | No hardware watchdog |
| `sweetfb_noconsole=1`, `sweetfb_hz=30` | Stop drawing kernel text over the GUI; panel copy rate |
| `sweettouch=0`, `sweetkeys=0` | Disable the touch or button driver |
| `sweettouchdelay=S`, `sweettouch_debug=1` | Touch start delay in seconds (default 45); log every raw report |
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
| `xnu/patches/` | The XNU patch series (0001–0082, applied in order by CI) plus staging scripts for libpthread, HFS and BearSSL |
| `.github/workflows/xnu.yml` | `validate` (Ubuntu, seconds), then the kernel `build` (macOS) |
| `tools/validate-repo.py` | Patch-series and script checks (numbering, hunk headers, replay of series-created files) |
| `tools/mkadt.py` | Apple Device Tree from the board's MMIO layout and the iPhone X identity |
| `tools/mkpayload.py` | Packs kernel + ADT + boot-args into the shim payload |
| `tools/mkiosroot.sh`, `sweet-root-tweaks.py` | HFS+ iOS root from the IPSW images, and the bring-up changes to it |
| `tools/sweet-log-pull.sh`, `sweet-log.py`, `sweet-usbnet.sh` | Persistent log from recovery; live log over USB Ethernet |
| `tools/sweet-crash.py` | Symbolicate console crash reports |
| `tools/xnu-cc.py` | Local single-file compile check from a CI log |
| `tools/qemu-sweet.py` | QEMU test boots with framebuffer text decoding |
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
| **0082** | **apollo: Novatek NT36675 SPI touch; volume-up PPID fallback and `sweetvolup=`** |

## Key hardware facts

| | sweet (SM7150) | apollo (SM8250) |
| --- | --- | --- |
| GIC | GICv3, `0x17a00000` / `0x17a60000` | same addresses |
| Virtual timer | INTID 19 (PPI 3) | INTID 27 (PPI 11) |
| Framebuffer | `0x9c000000`, 1080×2400 | `0x9c000000`, 1080×2400, 35 MiB splash region |
| Touch | Goodix GT9896, I²C `0x5d`, QUP SE7 `0xa84000` | Novatek NT36675, SPI, `0x994000`, GPIO 8 reset / 9 IRQ |
| Volume-up | PM6150L GPIO2 (PPID `0x4c1`) | unconfirmed (`0x1c1` fallback, `sweetvolup=`) |
| CPUs | 6×A55 + 2×A76 | 4×A55 + 4×A77 |

Both: PSCI through `smc`, 19.2 MHz timer, no pointer authentication, UFS host at `0x1d84000`. Details in [`NOTES.md`](NOTES.md).

## Credits

Built on [ios-sweet](https://github.com/dipa-studio/ios-sweet) and Apple's open-source releases (xnu, IOHIDFamily, libpthread, hfs), [BearSSL](https://bearssl.org), [blacktop/darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build), [ipsw](https://github.com/blacktop/ipsw), Qualcomm/Xiaomi's published Linux kernel sources for the hardware details, and [OrangeFox Recovery](https://orangefox.download). iOS itself is Apple's and is not distributed here. Licensed under the MIT License (see `LICENSE`).
