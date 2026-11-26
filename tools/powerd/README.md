<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
<!-- Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com> -->
# t2-powerd: the Titan 2 sleep broker (mainline)

One root daemon decides when the phone sleeps, modelled on Android's SystemSuspend + Doze on
top of the kernel's suspend-to-idle.

## What it does

- **Screen off** (DPMS Off on the DRM connector DSI-1, or the `screen off` command from the
  compositor) → after `suspend_delay` seconds, `mem` is written to `/sys/power/state` (s2idle)
  if nothing blocks it.
- **Blockers (leases).** A client holds a lease while its socket is open (`t2-powerctl inhibit
  NAME [reason] -- COMMAND` holds it for the duration of the command), or for a time
  (`block NAME SEC`). A call in ModemManager (D-Bus `Modem.Voice`/`Call.StateChanged`, states
  dialing…waiting) is a lease; an SMS (`Messaging.Added`) gives `wake_grace_modem` seconds
  awake. Other modem wake-ups (registration/signal/IMS indications) only give
  `wake_grace_modem_quiet` (3 s; the kernel already holds every CCCI packet for 500 ms,
  `mtk_ccci.rx_wake_ms`); `0` restores the old behaviour (`wake_grace_modem` for any).
  Observed blockers (configurable): an SSH session on :22, a USB gadget configured by a host
  (charging from an adapter is not a host), the root on NBD.
- **`wakeup_count` protocol** (as SystemSuspend): read it, write it back, and only then write
  `mem`; if the kernel is handling a wakeup event (including a kernel wakelock from
  `/sys/power/wake_lock`, `CONFIG_PM_WAKELOCKS`), the attempt is refused and retried after 2 s.
- **Wake-up classification:** the `event_count` difference over `/sys/class/wakeup/*`, the
  `pm_wakeup_irq` name from `/proc/interrupts`, the SPM wake reason from the `mtk-lpm` driver
  (`.../lpm/last_wake`: R12 sources, time slept, whether the depth was reached) → `user` (keys,
  fingerprint), `modem` (CCIF/DPMAIF → `mddriver`), `alarm` (our RTC alarm), `network` (a
  Wi-Fi frame / BT event: `wake_grace_network` 3 s, Android keeps about 0.3 s after RX; hook
  `wake-network`), `maintenance` (maintenance window), `other`.
- **Quiet wake-up:** modem/RTC/other wake-ups do not turn the screen on: the phone stays awake
  for `wake_grace_*` seconds and sleeps again. A user wake-up (a key) turns the screen on
  through niri's own input handling; the `screen-on` hook backs it up.
- **Doze windows:** with the screen off, the RTC wakes the phone after 15, 30, 60, 120 minutes
  (config `maintenance`), `hooks/maintenance(.d)` run for `maintenance_window` seconds, then
  sleep again; any user wake-up restarts the cycle.
- **One RTC alarm** for everybody: user alarms (`alarm set NAME +SEC|EPOCH [quiet]`; when it
  fires, the `wake-alarm` hook and an event for subscribers, with `ui` `wake_grace_alarm`
  seconds awake) and the maintenance windows; the nearest one is armed before sleep.
- **Back-off on short sleeps without a reason:** `backoff` 2, 5, 15, 60 s (a sleep shorter than
  `short_sleep` measured on CLOCK_BOOTTIME, i.e. including the time asleep).
- **Time asleep:** the log says `resumed after N s asleep (M s in transitions)`: N is CLOCK_BOOTTIME
  minus CLOCK_MONOTONIC (MONOTONIC stops in suspend), M the entry and exit; `t2-powerctl status`
  shows `asleep N s (since boot: M s)`: our sleeps and all of the kernel's sleeps since boot.
- **GPU boost on input:** the GPU governor (panthor devfreq simple_ondemand) raises the clock only
  after a busy sampling period, so the first frames of a scroll or an animation that starts from an
  idle GPU run at the lowest clock. On a touch or a key the devfreq `min_freq` is raised to
  `gpu_boost_hz` for `gpu_boost_ms` (each event extends it), then put back; off while the screen
  is off; `gpu_boost = no` disables it.
- **Battery guard** (only while discharging): `battery_low` (15 %) and `battery_critical` (5 %) run
  their hooks and send `battery low|critical N` to subscribers, once per discharge;
  `battery_shutdown` (2 %) runs the `battery-shutdown` hooks and `battery_shutdown_cmd`
  (`/sbin/poweroff`), but only after two readings in a row at that level (one odd fuel-gauge value
  does not power the phone off). A charger, or `battery_low + 5 %`, resets the levels (`battery-ok`).
  The battery is read every `battery_poll` seconds (60) while awake and right after every wake-up,
  so a sleeping phone checks it in its maintenance windows (CLOCK_MONOTONIC stops in s2idle, one
  poll interval alone would not be enough). `status` has a line
  `battery N% Status level=ok|low|critical|shutdown`; `battery = no` disables the guard.
- **Supervision:** the OpenRC service runs under `supervise-daemon` (restart after a crash, at most
  5 within 10 minutes) and allows a core dump into `/var/crash`.
- **Power key** (evdev `mtk-pmic-keys`, no grab): a short press runs the `screen-off` /
  `screen-on` hooks (default: `niri msg action power-off/on-monitors` for every niri socket in
  `/run/user/*`). Disabled with `power_key = no` when the desktop takes over the key.

## Files

- `src/t2-powerd.c`: daemon and client (`t2-powerctl` is a symlink to the same binary;
  `t2-powerd ctl …`).
- `t2-powerd.conf` → `/etc/t2-powerd.conf` (every value with comments), `t2-powerd.initd` →
  OpenRC, `hooks/` → `/etc/t2-powerd/hooks/`.
- `tests/fake-sysfs-test.sh`: a host run with fake `/sys` and `/proc` (`make test`, sanitizer
  build, dry run): leases, user/modem/other classification, alarm, USB blocker.
- `tools/netroot/install.d/t2-powerd.sh`: installation into the root file system (the service
  is not added to a runlevel).

## Socket protocol `/run/t2-powerd.sock` (lines)

```
status                         state (state, screen, blocker, leases, calls, alarms, counters, last_wake)
inhibit NAME [reason]          lease until the connection closes → ok
block NAME SEC [reason]        lease for a time;  unblock NAME
screen off|on                  screen state from the compositor (5 s priority over DRM)
activity                       the user is active (stay awake wake_grace_user s)
wake-screen [REASON]           switch the panel on now: the screen-on hook, "screen on" to subscribers,
                               stay awake wake_grace_user s (e.g. after a fingerprint match with the
                               panel off)
gpu boost [MS]                 raise the GPU min_freq for MS (default gpu_boost_ms); the daemon does this
                               itself on every touchscreen/keyboard event (gpu_boost in the configuration)
suspend [force]                sleep now (force: ignore the observed blockers)
alarm set NAME +SEC|EPOCH [quiet] | alarm clear NAME | alarm list
subscribe                      event stream: suspend, wake CLASS SOURCES, alarm NAME ui|quiet,
                               call PATH STATE, sms, maintenance, alarm-fired NAME,
                               screen on|off (the current state right after ok, then every change; t2-fpd)
reload                         re-read the configuration
```

Hooks (`/etc/t2-powerd/hooks/<name>` or `<name>.d/*`): `pre-suspend`, `post-resume`,
`wake-user`, `wake-modem`, `wake-alarm`, `wake-other`, `maintenance`, `screen-off`,
`screen-on`, `battery-low`, `battery-critical`, `battery-shutdown`, `battery-ok`. Environment:
`T2_WAKE_CLASS`, `T2_WAKE_SOURCES`, `T2_WAKE_SPM`, `T2_SLEPT_MS`, `T2_ALARM`,
`T2_MAINTENANCE_LEVEL`, `T2_BATTERY_CAPACITY`, `T2_BATTERY_STATUS`, `T2_BATTERY_LEVEL`.

## Build

Host (needs `pkg-config dbus-1`): `make`; `make test`. Without D-Bus: `make DBUS=0`.

Phone (Alpine aarch64, musl; dynamically linked against `libdbus-1.so.3` from the `dbus-libs`
package, which comes with `dbus`), with `$REPO` the checkout of this repository:

    SR=$REPO/out/musl-aarch64/sysroot
    PATH=${CLANG_DIR:-$REPO/toolchains/clang-r574158}/bin:$PATH \
    make BIN=t2-powerd-aarch64 \
         CC="clang --target=aarch64-alpine-linux-musl --sysroot=$SR -fuse-ld=lld --gcc-toolchain=$SR/usr" \
         DBUS_CFLAGS="-I$SR/usr/include/dbus-1.0 -I$SR/usr/lib/dbus-1.0/include" DBUS_LIBS="-ldbus-1"

If the sysroot has no dbus-dev:

    docker run --rm -v $REPO/out/musl-aarch64:/sr alpine:3.24 sh -c \
      'apk --arch aarch64 --root /sr/sysroot --allow-untrusted \
       -X https://dl-cdn.alpinelinux.org/alpine/v3.24/main --no-scripts add dbus-dev && chown -R 1000:1000 /sr'

## On the phone

1. The `rc-service t2-pmtest` checks (separately, with t2-powerd stopped: both write
   `wakealarm`).
2. First start: `dry_run = yes` in `/etc/t2-powerd.conf`, `rc-service t2-powerd start`,
   `t2-powerctl status`, turn the screen off: syslog shows "dry run" and the wake-up
   classification.
3. `dry_run = no`, `t2-powerctl reload`; to sleep on a cable/SSH: `inhibit_on_ssh = no`,
   `inhibit_on_usb_host = no` (SSH drops and comes back after the wake-up).
4. `rc-update add t2-powerd default`.

The hook `pre-suspend.d/10-wifi-wowlan` enables WoWLAN "any" (`iw`); otherwise cfg80211 drops
the Wi-Fi association before sleep. Limits: with `power_key = yes` the power key is read next
to libinput (no grab); a desktop client (a screen locker, the phone UI) can send
`screen off/on`, `activity`, `inhibit`: the protocol above.
