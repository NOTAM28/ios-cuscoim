# ios-cuscoi

This repository is a fork of **ios-apollo**. ios-apollo builds Apple's open-source XNU kernel (xnu-8796, the iOS 16 generation) for the MI 10T 5G (`apollo`, SM8250), then runs the **unmodified iOS 16.7.16 (20H392) userland of the iPhone X** on it: launchd, the system daemons, backboardd, SpringBoard and the stock apps. On `sweet` it reaches the iOS home screen with touch, buttons, all eight cores and USB networking.

**Booting an iOS 16 XNU kernel on a Qualcomm Snapdragon phone (codename `cuscoi`).**

An experimental port that takes Apple's open-source XNU kernel (`xnu-8796.141.3`, Darwin 22.6.0, the iOS 16.7.x line), patches it to run on non-Apple silicon, and boots it straight from the Android bootloader with `fastboot boot`. The kernel presents itself as an `iPhone10,3` (iPhone X).

> **Status as of 24/10/2026: early bring-up.** The kernel boots through the whole early-init stage into IOKit, the display console works, and the phone's UFS storage is readable. It does not yet reach userspace (`launchd`). This is a research project, not a usable OS.

---

## Aim

1. Get a patched XNU kernel to boot to userspace on a Snapdragon device.
2. Mount a real iOS 16.7.16 root filesystem (iPhone X IPSW) from the phone's UFS storage.
3. Longer term: graphics (Adreno GPU), touch, USB, keys and buttons.

A realistic near-term goal is a kernel that boots, runs a few processes and logs to a framebuffer or serial console. A daily-usable iOS phone is a long shot, mainly because iOS's graphics stack targets Apple's GPU and the phone has an Adreno.

---

## How it works

```
 GitHub Actions (macOS runner)            Local machine (WSL)                    Phone
 ┌───────────────────────────┐   gh run   ┌─────────────────────┐  fastboot boot ┌──────────────┐
 │ XNU source + patches      │ ─────────► │ kernel.development  │ ─────────────► │ runs from RAM│
 │ xnu/patches/0001–0107     │  artifact  │ .sweet + shim       │  cuscoim-boot  │ (nothing is  │
 │ → kernel.development.sweet│            │ → cuscoim-boot2.img │     2.img      │  written)    │
 └───────────────────────────┘            └─────────────────────┘                └──────────────┘
```

- **`shim/`** – small bare-metal loader that runs first. It loads the kernel, sets up the framebuffer, prints early UFS diagnostics, and hands over to XNU.
- **`xnu/patches/`** – 107 numbered patches (`0001`–`0107`) applied on top of upstream XNU by CI. The XNU source itself is not checked in. Much of this stack descends from earlier work on another device (the "apollo" lineage).
- **`.github/workflows/xnu.yml`** – builds the patched kernel on a macOS runner (about 8+ minutes). It triggers on changes under `xnu/patches/**`.
- **`tools/validate-repo.py`** – checks patch numbering and format before CI.
- **Boot image** – built locally from the kernel plus the shim, then booted with `fastboot boot`, which runs entirely from RAM.

---

## What works today

| Area | State |
|---|---|
| Shim → XNU handoff | Working. Kernel starts on CPU 0 (unslid, text base `0xfffffe0007004000`) |
| Timer | Working. Uses the virtual timer (`cntv_*`). Patch `0105` removes the last physical-timer write, which the Qualcomm hypervisor traps at EL1 |
| Early kernel init | Working: zones, VM, compressor, MAC Framework, vnode guard policy |
| AMFI | Stubbed. Built-in interface registered, trust caches unavailable |
| Platform expert | Device-tree based, registered |
| Drivers registered | AKS (keystore), WDT (watchdog), SURF, HID buttons, battery (static fallback 100% / 4200 mV) |
| CPU | CPU 0 up, SGI1 verified, running single-CPU (`cpus=1`) |
| Display | Framebuffer console works: 1080x2400 panel presented as 1125x2436 |
| UFS storage | Working, **read-only**. The driver issues a go-active to the device W-LUN (this fixed the earlier "not ready" / SCSI status 2 failures), scans LUNs, and finds `userdata` on **LUN 4** |
| Diagnostics | Patches `0106` / `0107` print SCSI sense data, controller registers and a per-LUN survey, and hold the screen 30 s before a root panic |

### Boot arguments (current)

```
debug=0xc serial=3 cpus=1 keepsyms=1 -noprogress rd=ufsroot0 wdt=-1 ... sweettouch=0 sweetkeys=0 sweetusb=0
```

`ufsrw=1 rootrw=1` have been removed on purpose, so no boot test can write to storage.

---

## Current blocker

After the UFS stage the kernel hits a **kernel data abort** (`esr 0x96000010`, a synchronous external abort).

- Faulting access: `_gpu_rd` (`add x8, x0, w1; ldr w0, [x8]`), called from `_sweet_gpu_probe + 0x4c8` with offset `0x1098`.
- Meaning: the Adreno GPU bring-up (patches `0097`–`0104`) is reading a register in a block that is unpowered, clock-gated, or blocked by the hypervisor.
- Open question: the README says GPU bring-up is off by default (`sweetgpu=` boot-arg), yet the probe ran. The default step count in `sweet_gpu.c`, or something else starting the probe, is what we are checking.
- Likely fix: a small patch (`0108`) so the probe only runs when `sweetgpu=N` is explicitly given. The framebuffer console already works without the GPU, so skipping it is the low-risk path.

Next expected stage after that: a clean "can't mount root" panic, because `userdata` still holds Android data and not an iOS HFS+ root.

---

## Not working / not started

- iOS root filesystem (nothing has been flashed; see warning below)
- Userspace / `launchd`
- GPU (Adreno), touch, USB, buttons/keys (`sweettouch=0 sweetkeys=0 sweetusb=0`)
- Multi-CPU (`cpus=1`)
- Framebuffer text is clipped at the left edge of the screen (small known bug)

---

## Safety and data

- Everything so far uses `fastboot boot`, which **runs from RAM and writes nothing to the phone**. Android and its data are untouched.
- The storage driver is read-only unless `ufsrw=1` is set. Patch `0047` (persistent console log) only writes once the partition is known and writable.
- **Installing the iOS root will erase Android's `userdata` partition** (`fastboot flash userdata ios16-root.simg`). Back up everything first. Bootloader, recovery, modem and persist partitions are not touched.
- Do not flash anything until the root image is built and the partition layout is verified.


---

## Roadmap

- [x] Boot XNU and reach IOKit
- [x] Replace physical timer use with the virtual timer (`0105`)
- [x] UFS bring-up: W-LUN go-active, LUN survey, locate `userdata` (LUN 4) (`0106`, `0107`)
- [ ] Gate or skip the GPU probe by default (`0108`)
- [ ] Clean "can't mount root" panic
- [ ] Scan the kernel for other Apple-only system registers and handle them in one batch
- [ ] Build `ios16-root.simg` (needs the iPhone X 16.7.16 IPSW)
- [ ] Verify partition layout, back up, then flash `userdata`
- [ ] First `launchd` / userspace
- [ ] Display text clipping fix, then GPU, touch, USB


## Credits

Built on [ios-sweet](https://github.com/dipa-studio/ios-sweet) and Apple's open-source releases (xnu, IOHIDFamily, libpthread, hfs), [BearSSL](https://bearssl.org), [blacktop/darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build), [ipsw](https://github.com/blacktop/ipsw), Qualcomm/Xiaomi's published Linux kernel sources for the hardware details, and [OrangeFox Recovery](https://orangefox.download). iOS itself is Apple's and is not distributed here. Licensed under the MIT License (see `LICENSE`).
