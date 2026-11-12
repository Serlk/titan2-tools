#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
#
# t2-powerd against a fake /sys and /proc (no phone, no suspend): the
# state machine, the socket protocol, the leases, the wake classification
# and the RTC alarm arithmetic. Run by "make test" with the sanitizer build.
#
#   sh tests/fake-sysfs-test.sh ./t2-powerd

set -eu
BIN=${1:-./t2-powerd}
T=$(mktemp -d)
trap 'kill $PID 2>/dev/null || true; rm -rf "$T"' EXIT INT TERM
R=$T/root
SOCK=$T/sock
fail() { echo "FAIL: $*" >&2; cat "$T/log" >&2; exit 1; }
ctl() { "$BIN" ctl "$@"; }

# a fake sysfs: a connected DSI connector, a few wakeup sources, an RTC, the LPM driver
mkdir -p "$R/sys/class/drm/card1-DSI-1" "$R/sys/class/rtc/rtc0" "$R/sys/power" \
	"$R/sys/class/udc/11201000.usb0" "$R/proc/net" "$R/dev/input" \
	"$R/sys/bus/platform/drivers/mtk-lpm/1c001000.syscon:lpm/lpm"
echo connected >"$R/sys/class/drm/card1-DSI-1/status"
echo On >"$R/sys/class/drm/card1-DSI-1/dpms"
echo 7 >"$R/sys/power/wakeup_count"
echo "[s2idle]" >"$R/sys/power/mem_sleep"
echo "freeze mem" >"$R/sys/power/state"
echo 60 >"$R/sys/power/pm_wakeup_irq"
: >"$R/sys/class/rtc/rtc0/wakealarm"
echo "not attached" >"$R/sys/class/udc/11201000.usb0/state"
printf '  sl  local_address rem_address   st\n' >"$R/proc/net/tcp"
: >"$R/proc/net/tcp6"
: >"$R/proc/mounts"
printf '           CPU0\n 60:          0  GICv3 531 Level     mtk-lpm-spm\n 61:          0  mt6685-rtc\n' >"$R/proc/interrupts"
i=0
for n in gpio-keys mddriver mt6685-rtc 11201000.usb0; do
	mkdir -p "$R/sys/class/wakeup/wakeup$i"
	echo "$n" >"$R/sys/class/wakeup/wakeup$i/name"
	echo 0 >"$R/sys/class/wakeup/wakeup$i/event_count"
	i=$((i + 1))
done
printf 'seq 1\nsources EINT_EVENT_B\nslept_ms 1234\ndeepest 1\n' \
	>"$R/sys/bus/platform/drivers/mtk-lpm/1c001000.syscon:lpm/lpm/last_wake"

cat >"$T/conf" <<EOF
suspend_delay = 1
poll_ms = 200
inhibit_on_ssh = yes
inhibit_on_usb_host = yes
inhibit_on_nbd_root = no
power_key = no
dbus_modem = no
wake_grace_user = 2
wake_grace_modem = 2
wake_grace_modem_quiet = 0
wake_grace_other = 1
wake_grace_alarm = 1
maintenance = 100, 200
maintenance_window = 1
backoff = 1, 2
hooks_dir = $T/hooks
dry_run = yes
log_level = debug
EOF
mkdir -p "$T/hooks/post-resume.d"
cat >"$T/hooks/post-resume.d/10-log" <<EOF
#!/bin/sh
echo "resume class=\$T2_WAKE_CLASS sources=[\$T2_WAKE_SOURCES] spm=[\$T2_WAKE_SPM] slept=\$T2_SLEPT_MS" >>"$T/hooklog"
EOF
chmod +x "$T/hooks/post-resume.d/10-log"
cat >"$T/hooks/maintenance" <<EOF
#!/bin/sh
echo "maintenance level=\$T2_MAINTENANCE_LEVEL" >>"$T/hooklog"
EOF
chmod +x "$T/hooks/maintenance"

"$BIN" -c "$T/conf" -s "$SOCK" -r "$R" -d -f >"$T/log" 2>&1 &
PID=$!
for i in 1 2 3 4 5 6 7 8 9 10; do [ -S "$SOCK" ] && break; sleep 0.2; done
[ -S "$SOCK" ] || fail "no socket"
export T2_POWERD_SOCK="$SOCK"
ctl() { "$BIN" ctl "$@"; }

# 1. status while awake
out=$(ctl status)
case "$out" in *"state awake"*) ;; *) fail "status: $out";; esac

# 2. a held lease blocks the sleep: screen off, wait, nothing happens
(ctl inhibit test-lease because testing -- sleep 1234 2>/dev/null) &
LEASEPID=$!
sleep 0.5
echo Off >"$R/sys/class/drm/card1-DSI-1/dpms"
sleep 2.5
grep -q 'lease test-lease' "$T/log" || fail "lease not seen"
grep -q 'dry run' "$T/log" && fail "slept despite the lease"
pkill -f 'sleep 1234' 2>/dev/null || true
wait $LEASEPID 2>/dev/null || true

# 3. without the lease it sleeps (dry run), the wake is "other" (no source grew)
sleep 2.5
grep -q 'dry run: not writing' "$T/log" || fail "did not suspend after the lease was released"
grep -q 'other wake' "$T/log" || fail "wake class not 'other'"
grep -q 'resume class=other' "$T/hooklog" || fail "post-resume hook did not run: $(cat "$T/hooklog" 2>/dev/null)"
# the RTC alarm was armed for the first maintenance interval (100 s)
alarm=$(cat "$R/sys/class/rtc/rtc0/wakealarm")
case "$alarm" in 0) ;; *) fail "RTC alarm not disarmed after resume: $alarm";; esac
grep -q 'RTC alarm in 100 s for maintenance' "$T/log" || fail "maintenance alarm not armed"

# 4. a user wake: gpio-keys event_count grows while the (dry, 2 s) sleep lasts
echo On >"$R/sys/class/drm/card1-DSI-1/dpms"
sleep 0.5
echo Off >"$R/sys/class/drm/card1-DSI-1/dpms"
sleep 2
echo 5 >"$R/sys/class/wakeup/wakeup0/event_count"
sleep 2.5
grep -q 'user wake (sources: gpio-keys' "$T/log" || fail "user wake not classified: $(grep wake "$T/log" | tail -3)"

# 5. a modem wake, a few seconds later (user grace 2 s + delay 1 s)
sleep 2
echo 9 >"$R/sys/class/wakeup/wakeup1/event_count"
sleep 3
grep -q 'modem wake (sources: mddriver' "$T/log" || fail "modem wake not classified: $(grep wake "$T/log" | tail -3)"

# 6. a user alarm through the socket, in the past: fires at once
ctl alarm set wakeup +1 >/dev/null
sleep 2
grep -q 'alarm wakeup fired' "$T/log" || fail "alarm did not fire"

# 7. USB host blocks
echo configured >"$R/sys/class/udc/11201000.usb0/state"
sleep 4
grep -q 'USB host connected' "$T/log" || fail "USB host blocker not seen: $(tail -3 "$T/log")"
echo "not attached" >"$R/sys/class/udc/11201000.usb0/state"

kill $PID; wait $PID 2>/dev/null || true
echo "t2-powerd fake-sysfs test: OK"
