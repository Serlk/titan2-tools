<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# titan2-tools: mainline Linux on the Unihertz Titan 2 (alpha1)

[Русская версия](README.ru.md)

This repository has the tools that build and check boot images for the first mainline Linux
(7.2) on the **Unihertz Titan 2** (MediaTek MT6878 / Dimensity 7300). The images are loaded by the
**stock bootloader (LK), which is not modified**. The kernel itself is in
[titan2-linux](../titan2-linux) (branch `titan2`). The documentation (LK requirements, boot
attempts, hardware notes) is in [titan2-docs](../titan2-docs). The alpha1 images are attached to
the GitHub release `alpha1` of this repository.

> **This is an alpha for developers.** It boots to a root shell over USB. There is no
> graphical system, no phone functions, no Wi-Fi and no battery management that has been
> verified. Use it only if you can restore your phone through fastboot. You do everything at
> your own risk.

## What works in alpha1 (4th boot on hardware)

| | |
|---|---|
| Screen | Yes: the framebuffer LK leaves (simpledrm, 1440x1440) plus the `mtk-cmdfb-kick` frame trigger for the command-mode DSI panel. The text console is shown on the screen. |
| Keyboard | Yes: AW9523B matrix (`aw9523-keypad`) |
| Touchscreen, touchpad | Yes: Synaptics RMI4 over I2C |
| Volume keys, Func2 | Yes: gpio-keys |
| Power key, Func1 (home) | **Fixed, not yet verified on hardware**: PMIC interrupts through SPMI, EINT table fix |
| Storage | UFS is detected. **All block devices are forced read-only** by the initramfs. |
| USB | Yes: gadget with NCM (network 172.16.42.1) and ACM (serial console `ttyGS0`), SSH (dropbear) |
| Regulators | Yes: MT6363/MT6369 (only the rails that are always on in the stock firmware) |
| Charging | Experimental driver (MT6375 + SH366100 fuel gauge). Limited to 4.35 V. **Not verified.** |
| Reading `/sys/kernel/debug/clk/clk_summary` | Used to hang the bus. The GPU/SCP clock controllers are now disabled; **not yet verified** |
| Not supported | GPU, modem/calls, Wi-Fi/Bluetooth, audio, cameras, sensors, suspend, the second (rear) display |

## Requirements

- Unihertz Titan 2 with the stock firmware **V01.00.14**, **unlocked bootloader**
  (`fastboot flashing unlock` wipes the phone).
- `vbmeta_a` flashed with verification disabled. If you installed a custom GSI this was already
  done; otherwise use `fastboot --disable-verity --disable-verification flash vbmeta_a vbmeta.img`
  with the stock `vbmeta.img` of **your** firmware version.
- **Slot A only.** Check with `fastboot getvar current-slot`. Do **not** run `fastboot set_active b`:
  slot B holds older bootloaders.
- **Backup** of `boot_a`, `init_boot_a`, `vendor_boot_a`, `dtbo_a` (and `vbmeta_a`) of your phone
  **before** you flash anything, with `sha256sum *.img > SHA256SUMS`. You can take them from the
  official firmware package of the same version, or read them on a rooted phone
  (`dd if=/dev/block/by-name/boot_a of=/sdcard/boot_a.img`, and so on).
- A Linux PC with `fastboot` and `ssh`; for building: Docker, git, and AOSP clang r574158.

## Installing the alpha1 images

The release has two variants with the same kernel and initramfs:

- `usb-minimal`: devicetree without SPMI/PMIC and without the keyboard, touchscreen and touchpad
  (screen, UFS, USB, volume keys). Use it first.
- `usb`: everything listed above.

```
sha256sum -c SHA256SUMS
adb reboot bootloader            # or: Volume Up + Power -> "Fastboot Mode"
fastboot getvar current-slot     # must be: a
cd usb-minimal                   # or: cd usb
fastboot flash boot_a        boot.img
fastboot flash init_boot_a   init_boot.img
fastboot flash vendor_boot_a vendor_boot.img
fastboot flash dtbo_a        dtbo.img
fastboot reboot
```

Once the kernel has started, the PC sees a USB network adapter and a serial port. The phone is
172.16.42.1 and gives the PC an address over DHCP:

```
ssh root@172.16.42.1             # root has an EMPTY password in the release images
t2-report > t2-report.txt        # dmesg, interrupts, regulators, clocks, I2C, input
t2-noreboot                      # cancel the safety reboot
```

**Safety reboot:** if there is no SSH or serial login within 10 minutes, the phone reboots by
itself (`t2-noreboot` cancels this). Android stays broken until you restore the four partitions
(see below).

**Security:** the release initramfs lets `root` log in **without a password**, but only over the
USB cable (there is no other network). To use key-only login, build your own initramfs with your
public key: `T2_AUTHORIZED_KEYS=~/.ssh/id_ed25519.pub`.

**Privacy:** `t2-report` and `dmesg` contain your device serial number, the UFS part ID and MAC
addresses. Remove these before you post logs publicly.

## Going back to Android

```
scripts/restore-stock-boot.sh /path/to/your/backup     # waits for fastboot, flashes, reboots
```

Or flash the same four partitions by hand: `fastboot flash boot_a boot_a.img`, then the same for
`init_boot_a`, `vendor_boot_a` and `dtbo_a`, then `fastboot reboot`.

**Getting into fastboot from a boot loop:** keep the cable connected and hold **Volume Up + Power**
until the "Select Boot Mode" menu appears. Volume Up moves the selection and Volume Down
confirms; choose **Fastboot Mode**. Volume Up alone starts recovery, which boots the same kernel
from `boot_a`. A long press on Power reboots the phone; it does not switch it off.

**Never** flash or erase `preloader`, `lk`, `tee`, `seccfg`, `nvram`, `nvdata`, `nvcfg`,
`proinfo`, `protect1/2` or `md*`, and do not lock the bootloader. Do not run `fastboot -w`. These
images never write to the phone's storage: all UFS block devices are read-only.

## Dual boot: Android and Linux on its own partition

Since 2026-10-24: a boot menu ([titan2-boot](../titan2-boot)) in `boot_a` starts Android or
mainline Linux; Linux lives on its own partition `linux`, carved from the end of `userdata`
(Android's data is wiped once), and cannot write to Android's partitions (a one-way read-only
lock in the kernel, `CONFIG_BLK_DEV_RO_LOCK`). The installation (with its own wipe order) and the
way back are in titan2-docs: `docs/DUALBOOT.md` and `docs/DUALBOOT-ROLLBACK.md`. The scripts:

| path | what it does |
|---|---|
| `scripts/make-bootmenu.sh` | `boot_a` image: titan2-boot (cloned into `titan2-boot/`) + the Android kernel + a fallback mainline kernel |
| `scripts/make-linux-root.sh` | image of the `linux` partition: Alpine + `/boot/extlinux/extlinux.conf` (sparse, for `fastboot flash linux`) |
| `scripts/netroot-serve.sh` | builds the Alpine root (also served over NBD for a network root) |
| `scripts/dualboot-repartition.sh`, `scripts/gpt-carve.py` | from TWRP: shorten `userdata`, add `linux`, with a backup and `restore` |
| `scripts/test-gpt-carve.sh`, `scripts/test-bootmenu-qemu.sh` | tests without the phone (the GPT of a real Titan 2, the menu in qemu) |
| `tools/initramfs/qemu/diskroot.sh` | root on a partition in qemu: the lock and write attempts as root |

## Building

```
git clone <titan2-tools> && cd titan2-tools
git clone -b titan2 <titan2-linux> linux        # kernel tree (or kernel-source-alpha1.tar.gz + apply.sh)
# AOSP prebuilt clang r574158 (clang 21) into toolchains/clang-r574158:
#   from https://android.googlesource.com/platform/prebuilts/clang/host/linux-x86 (directory clang-r574158)
# AOSP mkbootimg (Apache-2.0, not included) into tools/mkbootimg:
git clone https://android.googlesource.com/platform/system/tools/mkbootimg tools/mkbootimg
docker build -t titan2-kbuild docker/
make -C tools CC=$PWD/toolchains/clang-r574158/bin/clang     # mini-init, devmem
scripts/build-mainline.sh                                   # -> out/mainline-build
USB=1 VARIANT=minimal scripts/make-mainline-images.sh       # -> out/mainline-images-usb-minimal
USB=1 scripts/make-mainline-images.sh                       # -> out/mainline-images-usb
docker run --rm -u $(id -u):$(id -g) -v "$PWD":/work titan2-kbuild \
    python3 /work/scripts/lk-fdt-check.py --vendor-boot /work/out/mainline-images-usb/vendor_boot.img
```

The initramfs (`tools/initramfs/build.sh`) downloads Alpine v3.24 aarch64 packages (busybox,
dropbear, i2c-tools, evtest, musl, ...) and checks their signatures. `tools/initramfs/qemu/run.sh`
tests it in qemu. Nothing in this repository flashes the phone.

| path | what it does |
|---|---|
| `scripts/build-mainline.sh` | builds the kernel (`Image.gz`) and DTBs in Docker |
| `scripts/make-mainline-images.sh` | builds boot/vendor_boot/init_boot/dtbo v4 images the way the stock LK expects them |
| `scripts/lk-fdt-check.py` | offline check of a DTB or vendor_boot against the known LK requirements |
| `scripts/restore-stock-boot.sh` | flashes your backup of slot A back |
| `tools/initramfs/` | initramfs: USB console, network root (`t2.root=nbd:`), root on the `linux` partition (`t2.root=part:linux`), read-only protection of the UFS |
| `tools/netroot/` | first boot and services of the Alpine root, qemu test of the network root |
| `tools/t2-reboot/` | `reboot` with an argument (`t2-reboot bootloader`) |
| `tools/mini-init/`, `tools/devmem/` | static aarch64 helpers without libc |
| `configs/mainline-titan2.config` | kernel config fragment on top of arm64 `defconfig` |
| `docker/Dockerfile` | the `titan2-kbuild` build container |

## How this project is made / affiliation

- This project is developed with the help of AI coding assistants (Anthropic Claude). Commits
  carry a `Co-Authored-By: Claude` trailer; every change is reviewed and tested on the device
  by the human maintainer (serlk), who is responsible for it.
- It is **not affiliated with postmarketOS** or MT6878-Mainline. We build on their GPL kernel
  work (thanks!), but postmarketOS does not accept AI-assisted contributions, so nothing from
  here is submitted to postmarketOS, and this is not a postmarketOS port.

## Sources

The facts these tools rely on (load addresses, what the stock boot loader expects
from the images and the device tree, register values) come from the phone itself:
its boot logs, readings on the device, the factory device tree, the phone's
GPL-declared kernel modules and an analysis of the behaviour of the stock boot
loader (no boot loader code is reproduced); public MediaTek GPL sources and
upstream Linux were used for comparison. No proprietary code, firmware or blobs
are included.

## License

Copyright (c) 2026 serlk. GPL-2.0-or-later, see [LICENSE](LICENSE). Every file names its
license in an `SPDX-License-Identifier` line. `tools/initramfs/keys/*.pub` is the Alpine Linux
signing public key (from Alpine's `alpine-keys`, MIT). The kernel is GPL-2.0 (titan2-linux).
Third-party packages in the release initramfs, and where to get their sources:
`initramfs-packages.md` in the release.

## Thanks

- [MT6878-Mainline](https://github.com/MT6878-Mainline/linux) (Igor Belwon, Arseniy Velikanov and
  contributors) and [postmarketOS](https://postmarketos.org/): the MT6878 SoC support this port
  is built on, and the USB-console conventions.
- MediaTek, for the GPL kernel sources of MT6878 published through Nothing (CMF Phone 1), which
  the register descriptions of the PMIC, clock and power-domain drivers come from.
- Alpine Linux, BusyBox and Dropbear.

This project is not affiliated with Unihertz or MediaTek.
