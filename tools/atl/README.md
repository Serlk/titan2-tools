<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Android applications on the Titan 2: Android Translation Layer (ATL)

[Android Translation Layer](https://gitlab.com/android_translation_layer/android_translation_layer)
runs Android applications as ordinary Linux processes: ART and an implementation of the Android API
on GTK4, without a container and without Android system images. On the Titan 2 it renders straight
through the GPU (GTK4 -> GSK -> Mesa panfrost/panvk -> Wayland compositor). This directory holds
our patches to it, the tools around it and the PC test benches.

Nothing of Android is shipped here: no APKs, no Google or microG components, no firmware. Test APKs
are downloaded at build time from their upstream repositories and checked (F-Droid: index signature
by the known repository certificate and sha256 from the signed index; ATL's test APKs: git blob id
from the GitLab API).

## Status

- On the phone: Gravity Defied started and rendered on niri through panfrost (an earlier ATL build
  with patches 0001-0007). A build with the package catalogue (0008-0010) installs and answers
  `atl-pm list`; no application has been run with the later patches on the phone yet.
- On the PC VM bench (`scripts/atl-vm.sh`, the same overlay as the phone image): Gravity Defied,
  ATL's demo app and the GLES 3 JNI sample render on the GPU path; touch and the hardware keys
  (Back, Alt, Sym, Fn) work with the Titan 2 keymap; 75 of the 76 package catalogue checks
  (`test-apps/catalogue-test.sh`) passed on an intermediate build; the TLS test of the test
  application goes from 1 to 24 of 24 connections with the wolfssljni patches; the Editor
  application (from F-Droid) shows typed text.
- Open: the F-Droid client itself (catalogue download and installation from it), Google services
  (microG is not part of this), cameras, notifications in the desktop, telephony.

## Pieces

| path | what |
|---|---|
| `patches/android-translation-layer/0001-0017` | our ATL patches (GPL-3.0-only, the license of ATL): hardware Back key, ActionBar/SearchView stubs, launcher intent, device identity from a device profile (0006), sensors from Linux IIO (0007), an installed-package catalogue with verified installs (0008), EditText fixes (0009, 0012, 0014), location/connectivity (0010), real input devices and key map (0011), ScrollView fillViewport (0013), activities of another package in its own process (0015), battery state and process names (0016), AndroidCAStore from the system's trusted roots (0017) |
| `patches/art_standalone/0001-0002` | wolfSSL JNI (TLS provider of art_standalone, GPL-2.0-or-later): session cache without freed sessions and races, a connection closed by the peer reads as end of stream |
| `tools/aports/titan2-art-wolfssljni` | rebuilds only the TLS provider jar of art_standalone with these two patches (`patches/titan2-art-wolfssljni` links to them) |
| `tools/aports/titan2-atl-tools` | packages the overlay tools below to `/usr/bin` |
| `tools/atl/overlay` | `atl-run` (start a package or an APK file, GPU only, per-application logs), `atl-install`, `atl-pm`, `atl-check` (first-run checks of the GPU path), `atl-open-apk` + `atl-install.desktop` (open APK files from a file manager), `atl-cacerts` (the system's CA bundle in Android's `/system/etc/security/cacerts` layout), the Titan 2 xkb keymap, niri and sway snippets, the panvk switch for the Mali-G615 |
| `tools/atl/atl-device-profile` | writes `/etc/atl/device.prop` (read by patch 0006) from **your own phone's** build.prop files and boot parameters. The result is device data (serial number, firmware fingerprint): keep it on the phone |
| `scripts/build-atl.sh`, `tools/atl/inner.sh` | build pipeline on an x86 host: aarch64 rootfs, test APKs, rebuilds of the ATL stack from Alpine's aports with our patches (qemu-user in a private binfmt_misc of a user namespace, no host changes) |
| `scripts/atl-vm.sh`, `docker/atl-vm`, `tools/atl/vm` | full-system aarch64 VM bench (qemu + virtio-gpu venus, niri, uinput input injection) |
| `tools/atl/test-apps` | our test applications (sensor test, package catalogue/EditText/TLS test application) and the catalogue test script; signed at build time with throwaway keys |
| `tools/netroot/install.d/atl.sh` | `ATL=1` image hook of `scripts/netroot-serve.sh` |

## Build

```sh
scripts/t2-repo.sh keygen                         # once: the key of the Titan 2 apk repository
scripts/build-atl.sh rootfs                       # Alpine aarch64 rootfs with ATL from edge/testing -> out/atl
scripts/build-atl.sh apks                         # verified test APKs -> out/atl-dl/test-apks
scripts/build-atl.sh build android-translation-layer   # ATL + patches/android-translation-layer (hours)
scripts/build-atl.sh build titan2-art-wolfssljni titan2-atl-tools
EXTRA_APK_DIRS=out/atl/apks scripts/t2-repo.sh build
LOCAL_REPO=out/t2-repo GFX=1 ATL=1 scripts/netroot-serve.sh build
```

`out/atl/apks` holds the unmodified edge/testing packages ATL needs (art_standalone,
bionic_translation, libopensles-standalone, libandroidfw, vixl); they keep Alpine's signatures.
The packages built here (`out/atl/packages/aports/aarch64`, which `scripts/t2-repo.sh build`
indexes by default) are signed with the key of the Titan 2 apk repository,
`out/t2-repo-keys/t2-repo.rsa` (`KEYS=<dir>` and `KEY=<file name>` for another key;
`build-atl.sh build` runs `scripts/t2-repo.sh keygen` when the default key does not exist yet).
`PACKAGER="Name <address>"` sets the packager field (default `titan2 builder <builder@localhost>`).
`scripts/t2-repo.sh build` makes a new index from scratch on every call: give it every
`EXTRA_APK_DIRS` directory you want in the repository (Mesa, libcamera, `out/atl/apks`) in one call.
On the phone, as the user of the niri session: `atl-check`, then
`atl-check run org.happysanta.gd_29.apk`, or `atl-install app.apk` and start it from the launcher.
The device profile: `atl-device-profile -o /etc/atl/device.prop <root of your mounted partitions>`.
