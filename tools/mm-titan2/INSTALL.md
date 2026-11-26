<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
<!-- Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com> -->
# ModemManager with the mtk-ccci plugin on the Titan 2: install, test, roll back

Phone: Alpine 3.24 aarch64 with the modem brought to READY by the OpenRC services
`t2-ccci-fsd`, `t2-ccci-rpcd`, `t2-modem` (radio power belongs to ModemManager). ModemManager and
`t2-modemd` both need `/dev/wwan0at0` and `/dev/wwan0mipc1`, so only one of them may run. The
switching steps below also cover an installation still using `t2-modemd`. Nothing below touches
the modem firmware, NVRAM or SIM locks. Design, status and the list of unverified items:
[DESIGN.md](DESIGN.md).

Before installing, save the installed plugin, udev rule, `/etc/conf.d/t2-modem` and sysctl
configuration. Set `T2_MODEM_RADIO=` in that conf.d file: `t2-modem` must stop at READY;
ModemManager alone sends EFUN after the IMS setup. Do not run t2-modemd alongside ModemManager.
Install `60-titan2-wwan.conf` from the build into `/etc/sysctl.d/` and apply it before checking
simultaneous Wi-Fi/WWAN connectivity. Keep SSH on USB/Wi-Fi while testing.

**Both SIMs as two modems (default).** The plugin shows each SIM as its own ModemManager modem
(DESIGN.md §11): SIM1 as before, SIM2 with `Device` `mtk-ccci-sim2`, its own signal, registration,
calls and SMS; mobile data stays on SIM1. When installing over a single-modem build:

1. Bind the NetworkManager profile to SIM1 before starting the new ModemManager, otherwise NM may
   try an `ifname '*'` profile on the SIM2 modem (which refuses bearers):
   `nmcli c modify mobile gsm.sim-id <ICCID of the SIM in slot 1>` (from `mmcli -m <n>` → `SIM`
   path → `mmcli -i <sim> -K | grep iccid`) **and** bind it to the SIM1 modem's port:
   `nmcli c modify mobile connection.interface-name wwan0at0`. Without the port binding NM may
   activate the profile on both modems right after a service restart (the SIM2 SIM identifier is
   not known to NM yet), and the failing SIM2 attempt takes the SIM1 activation down with it.
2. `udevadm control --reload && udevadm trigger -s wwan` after installing the new rules file
   (`wwan0mipc2` moves to its own device), then restart ModemManager.
3. Back to one modem object without changing the plugin: `/etc/udev/rules.d/79-mm-mtk-ccci-single.rules`
   with these two lines, then reload, trigger and restart ModemManager:

       SUBSYSTEM=="wwan", DRIVERS=="mtk-ccci-md", ENV{ID_MM_MTK_CCCI_DUAL_MODEM}="0"
       SUBSYSTEM=="wwan", DRIVERS=="mtk-ccci-md", KERNEL=="wwan*mipc2", ENV{ID_MM_PHYSDEV_UID}="mtk-ccci"

The new configuration properties can be overridden in a later 78-mm/79-mm user udev rule:
`ID_MM_MTK_CCCI_IMS=0` disables IMS for a data-only diagnostic, optional
`ID_MM_MTK_CCCI_DATA_APN` supplies the operator's internet APN before attach, and
`ID_MM_MTK_CCCI_IMS_APN` / `ID_MM_MTK_CCCI_SOS_APN` override ims/sos. Do not hard-code the
current operator in the plugin. Without a supplied internet APN the attach context is read
and the APN list refreshed once available.

First verify cold boot without t2-modemd, then IMS registration, NM data, both call directions,
SMS, HiFi restoration, disable/enable, MM restart and modem restart. Keep the debug log,
kernel version, plugin SHA256 and result for each case. A successful earlier call with a
modem initialized by t2-modemd is not evidence that a build's cold-start path works.

Rollback means stopping MM, restoring the saved plugin/rules/conf.d/sysctl files and choosing
the previous single AT/MIPC owner. Restore D-Bus activation policy consistently: stopping the
OpenRC MM service alone does not prevent NetworkManager from activating MM again when
D-Bus activation is enabled. The tested configuration keeps the activation file disabled and
starts MM through OpenRC.

## 1. Packages on the phone

```sh
apk add modemmanager modemmanager-udev modemmanager-openrc libmm-glib \
        networkmanager networkmanager-wwan networkmanager-cli networkmanager-openrc \
        dbus dbus-openrc polkit polkit-openrc eudev udev-init-scripts-openrc
```

`modemmanager` must be `1.25.95_git20260527-r1` (`apk info modemmanager`): the plugin is built
from the same ModemManager commit (`fcc9d19f`) with the same build options; a different package
version needs a rebuild (`build-on-phone.sh`). `networkmanager-wwan` is NetworkManager's modem
device plugin (Alpine splits it out). Alpine's `modemmanager-openrc` service needs `dbus` and
`polkit`.

## 2. The plugin files

Cross-built on the host by `tools/mm-titan2/build-aarch64.sh` into `$OUT/dist/`
(`SHA256SUMS` there; check after copying):

The cross-build script requires host `patchelf` and removes the build-tree RUNPATH
before stripping and checksumming the distributed files.

| file | destination on the phone |
|---|---|
| `libmm-plugin-mtk-ccci.so` | `/usr/lib/ModemManager/libmm-plugin-mtk-ccci.so` (mode 0755) |
| `77-mm-mtk-ccci.rules` | `/usr/lib/udev/rules.d/77-mm-mtk-ccci.rules` (mode 0644) |
| `60-titan2-wwan.conf` | `/etc/sysctl.d/60-titan2-wwan.conf` (mode 0644) |
| `t2-mm-speech-trigger` | `/usr/local/bin/t2-mm-speech-trigger` (optional, see) |

```sh
scp "$OUT"/dist/* phone:/tmp/mm/
ssh phone 'cd /tmp/mm && sha256sum -c SHA256SUMS && \
  install -m 0755 libmm-plugin-mtk-ccci.so /usr/lib/ModemManager/ && \
  install -m 0644 77-mm-mtk-ccci.rules /usr/lib/udev/rules.d/ && \
  install -m 0644 60-titan2-wwan.conf /etc/sysctl.d/ && \
  sysctl -p /etc/sysctl.d/60-titan2-wwan.conf && \
  install -m 0755 t2-mm-speech-trigger /usr/local/bin/ && udevadm control --reload'
```

If the `.so` does not load (`ModemManager --debug` says "plugin ... could not be loaded" or the
daemon aborts at start), build it on the phone instead: `sh tools/mm-titan2/build-on-phone.sh`
(abuild, produces a full package set with the plugin inside) or
`MODE=plugin sh tools/mm-titan2/build-on-phone.sh` (plugin only).

### The udev rule

`77-mm-mtk-ccci.rules` tags the modem's `wwan` ports and `net` links (driver chain
`mtk-ccci-md`) for the plugin, marks `wwan0at0` as the primary AT port, `wwan0mipc1` as the MIPC
data port, the other MIPC ports as ignored, and carries the plugin's configuration:

| property (on `wwan0at0`) | default | meaning |
|---|---|---|
| `ID_MM_MTK_CCCI_RADIO_MASK` | 3 | `AT+EFUN=<mask>`: 1 SIM1 radio, 2 SIM2, 3 both |
| `ID_MM_MTK_CCCI_SIM_SLOT` | 1 | single-object mode only: protocol stack on the AT port (`AT+ESUO=4`/`5`) and MIPC slot bit; overridden by a slot chosen with `mmcli --set-primary-sim-slot` (stored in `/var/lib/ModemManager/mtk-ccci-sim-slot`) |
| `ID_MM_MTK_CCCI_SPEECHD_SOCKET` | `/run/t2-speechd.sock` | the t2-speechd socket for the voice audio path; `""` disables the in-plugin trigger |
| `ID_MM_MTK_CCCI_DUAL_MODEM` (all wwan ports) | 1 | 1: SIM1 and SIM2 as two modem objects; 0: the single object with switchable primary SIM |
| `ID_MM_MTK_CCCI_DSDS_BUSY` | 1 | 1: an incoming call on one SIM while the other SIM has a call (dialing, ringing, active, held, waiting) is rejected at the modem's approval step (cause 21) and shown as an incoming call ended "refused-or-busy" on that SIM's modem; 0: every incoming call is approved. Not set in the rules file (default 1) |
| LTE start-up switches (`ID_MM_MTK_CCCI_DATA_ALLOW`, `_DATA_SIM`, `_RADIO_CONFIG`, `_PREFER_SBP`, `_REG_CAUSE`, `_START_PROFILES`, the IMS step switches, `_RADIO_OFF`, `_NW_DIAG`, `_IA`) | see DESIGN.md §12 | the start-up requests before radio-on and the read-only diagnostics, one switch per group |

| property (on `wwan0mipc2`, the SIM2 anchor) | default | meaning |
|---|---|---|
| `ID_MM_PHYSDEV_UID` | `mtk-ccci-sim2` | its own device, hence its own modem object |
| `ID_MM_MTK_CCCI_SIM_ANCHOR` | 2 | the plugin builds the SIM2 object for this device; the port stays ignored and is never opened |
| `ID_MM_MTK_CCCI_MIPC_PORT` | `<wwan>mipc1` | the shared MIPC port |
| `ID_MM_MTK_CCCI_SIM2_DATA` | 0 | 1 allows bearers on SIM2 (untested) |
| `ID_MM_MTK_CCCI_SPEECHD_SOCKET`, `ID_MM_MTK_CCCI_IMS`, `…_DATA_APN_SIM2`, `…_IMS_APN`, `…_SOS_APN`, `…_IA_SIM2` | as on wwan0at0 | read by the SIM2 object; the SIM1 object configures both stacks' IMS profiles from wwan0at0 |

Check after a reload with the ports present:
`udevadm info -q property /sys/class/wwan/wwan0at0 | grep ID_MM` (expect `ID_MM_MTK_CCCI=1`,
`ID_MM_PORT_TYPE_AT_PRIMARY=1`, `ID_MM_CANDIDATE=1`) and the same for `wwan0mipc1`
(`ID_MM_MTK_CCCI_MIPC_DATA=1`) and `wwan0mipc2` (`ID_MM_PORT_IGNORE=1`, and in the dual-modem
mode `ID_MM_PHYSDEV_UID=mtk-ccci-sim2`, `ID_MM_MTK_CCCI_SIM_ANCHOR=2`). If the ports were created
before the rule was installed: `udevadm trigger -s wwan`.

### D-Bus activation

With `networkmanager-wwan` installed, NetworkManager can activate ModemManager over D-Bus as soon
as it starts. While t2-modemd owns the ports, disable activation to prevent simultaneous
probing:

    mv /usr/share/dbus-1/system-services/org.freedesktop.ModemManager1.service{,.disabled}

The tested configuration keeps this file disabled and starts MM through OpenRC in the default
runlevel. Enabling D-Bus activation is a separate installation choice; only do so after
removing t2-modemd as an owner. When the plugin loads, `ModemManager --debug` logs
`loaded plugin 'mtk-ccci'` and `registered plugin allowlist tag: ID_MM_MTK_CCCI`.

## 3. Switching from t2-modemd to ModemManager

```sh
rc-service t2-modemd stop            # frees wwan0at0 and wwan0mipc1 (data link torn down)
rc-update del t2-modemd default      # (optional for the test: keep it out of the boot)
rc-service dbus start; rc-service polkit start
rc-update add modemmanager default; rc-update add networkmanager default
rc-service modemmanager start
rc-service networkmanager start
rc-service t2-speechd start          # voice audio path (needed for calls to be audible)
```

For the first run, ModemManager in the foreground with debug logging is more useful than the
service: `rc-service modemmanager stop; ModemManager --debug 2>&1 | tee /tmp/mm.log`. Watch for:
`[plugin manager] ... mtk-ccci`, the ports being probed (`wwan0at0`: `probe step: AT` ... `port is
AT-capable`; `wwan0mipc1`: `MIPC port: AT probing skipped`), `modem ... successfully created`,
then on enable `MIPC session ready (slot 0x01)`.

The modem must be READY (`cat /sys/kernel/debug/d180000.mddriver/fsm` → `state: ready`) and the
ports present (`ls /dev/wwan0*`) — the `t2-modem` service does that; ModemManager picks the ports
up whenever they appear.

## 4. Verifying

```sh
mmcli -L                                   # one modem, plugin "mtk-ccci"
mmcli -m any                               # state: registered; ports: wwan0at0 (at), wwan0mipc1 (unknown)
mmcli -m any --3gpp-scan --timeout=120     # optional (AT+COPS=?)
mmcli -i any                               # SIM: ICCID/IMSI/operator
```

Mobile data (static IPv4 over a wwan link; the APN of the default bearer is used when none is
given):

```sh
mmcli -m any --simple-connect="apn=<your APN>"
mmcli -b 0                                 # (or the bearer path mmcli printed): connected, interface wwan0-2, IPv4 address/DNS
ip addr show wwan0-2                       # the link is up but ModemManager does not configure addresses: NetworkManager does
```

With NetworkManager (what we want in the end): `nmcli c add type gsm ifname '*' con-name mobile
apn <your APN>` then `nmcli c up mobile`; `nmcli d` shows the modem device (`gsm`),
`ip route` the default route through `wwan0-N`, then `ping -c3 1.1.1.1` and a DNS lookup. The
first connection is the first real test of the MIPC path through ModemManager: the modem's
answer (result code, transIntfId, address) is in the debug log (`activating PDN over MIPC`,
`PDN activated over MIPC: aid ..., transIntfId ..., link id ..., address ...`); every TLV of the
confirm is logged at debug level for the open questions.

Disconnect: `mmcli -m any --simple-disconnect` (or `nmcli c down mobile`): `DATA_DEACT_CALL`,
link down, link deleted.

Voice call (outgoing; t2-speechd running; low volume first — see `tools/speechd/README.md`):

```sh
mmcli -m any --voice-create-call="number=+49..."
mmcli -m any --voice-list-calls
mmcli -m any -o 0 --start                  # ATD...; → +ECPI 130/2/6 → dialing/ringing-out/active
                                           # the in-call hook sends t2-speechd "call-start --rate 48000"
mmcli -m any -o 0 --hangup                 # +CHLD=1<id>; → "call-stop"
```

Incoming: the `+EAIC` indication is approved at once (`AT+EAIC=0,<id>,<seq>`), the call rings
(`RING`, `+CLIP`, `+ECPI 0`), `mmcli -m any --voice-list-calls` shows it ringing-in;
`mmcli -m any -o <n> --accept` (ATA) makes it active and switches the audio on.

SMS: `mmcli -m any --messaging-create-sms="text='hi',number='+49...'"` then
`mmcli -m any -s 0 --send`; received messages: `mmcli -m any --messaging-list-sms`.

Dual SIM, dual-modem mode: `mmcli -L` lists two modems; `mmcli -m <n>` shows `Device:
mtk-ccci` (SIM1) or `mtk-ccci-sim2` (SIM2), each with its own SIM, signal, registration, calls
(`mmcli -m <n> --voice-create-call=…`) and SMS. Use the modem index instead of `-m any` (which
picks one of them). Single-object mode: `mmcli -m any` shows two SIM slots, slot 1 primary;
`mmcli -m any --set-primary-sim-slot=2` stores the choice and reprobes the modem on the SIM2
stack (`AT+ESUO=5`); back with `=1`.

## 5. Rolling back to t2-modemd

```sh
rc-service networkmanager stop; rc-service modemmanager stop
rc-update del modemmanager default; rc-update del networkmanager default   # if added
ip link del wwan0-2 2>/dev/null                                              # any leftover link
rc-service t2-modemd start; rc-update add t2-modemd default
```

The plugin files can stay installed; nothing else was changed. To remove them:
`rm /usr/lib/ModemManager/libmm-plugin-mtk-ccci.so /usr/lib/udev/rules.d/77-mm-mtk-ccci.rules
/var/lib/ModemManager/mtk-ccci-sim-slot`.

## 6. The speech trigger

By default the plugin itself tells t2-speechd when the first call is dialing/active and when the
last one ends (DESIGN.md). The alternative helper `t2-mm-speech-trigger` does the same from the
D-Bus Call objects; use it only with the in-plugin trigger disabled (set
`ID_MM_MTK_CCCI_SPEECHD_SOCKET=""` in the rules file, reload udev, restart ModemManager):
`t2-mm-speech-trigger -v` (foreground) or an OpenRC service running it after `modemmanager` and
`t2-speechd`.

## 7. What to collect if something fails

`ModemManager --debug` output (the MIPC frames are dumped as hex at debug level: `<--`/`-->`
lines of the `wwan0mipc1` port), `dmesg | grep -i ccci`, `udevadm info -q property
/sys/class/wwan/wwan0at0`, `ip -d link show`, and for data `cat
/sys/kernel/debug/d180000.mddriver/*` as before. The AT dialogue is in the same log (`wwan0at0`
`<--`/`-->`).
