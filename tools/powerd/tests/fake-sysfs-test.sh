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
# every request with a limit: a hung answer fails the test instead of stopping it for good
ctl() { timeout 20 "$BIN" ctl "$@" || { r=$?; [ $r = 124 ] && fail "ctl $* gave no complete answer within 20 s"; return $r; }; }
# the expected events are waited for (up to WAIT seconds, default 20: a slow or loaded host,
# sanitizers) instead of a fixed sleep; fixed sleeps remain only where something must NOT happen
WAIT=${WAIT:-20}
# count <file> <pattern>: lines that match
count() { grep -c -- "$2" "$1" 2>/dev/null || true; }
# wait_for <file> <pattern> [<n>]: until at least n (default 1) lines match, at most WAIT s
wait_for() {
	_t=$((WAIT * 10))
	while [ "$(count "$1" "$2")" -lt "${3:-1}" ]; do
		_t=$((_t - 1))
		[ $_t -gt 0 ] || return 1
		sleep 0.1
	done
}
# the next (dry) sleep has begun: the daemon logs it, then sleeps 2 s
next_sleep() {
	wait_for "$T/log" 'dry run: not writing' $(($(count "$T/log" 'dry run: not writing') + 1))
}

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
# a battery for the guard: full and charging until test 8
mkdir -p "$R/sys/class/power_supply/ac" "$R/sys/class/power_supply/bat0"
echo Mains >"$R/sys/class/power_supply/ac/type"
echo Battery >"$R/sys/class/power_supply/bat0/type"
echo 80 >"$R/sys/class/power_supply/bat0/capacity"
echo Charging >"$R/sys/class/power_supply/bat0/status"

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
battery_low = 20
battery_critical = 10
battery_shutdown = 5
battery_poll = 1
battery_shutdown_cmd = $T/hooks/fake-poweroff
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
mkdir -p "$T/hooks/battery-low.d"
cat >"$T/hooks/battery-low.d/10-log" <<EOF
#!/bin/sh
echo "battery-low cap=\$T2_BATTERY_CAPACITY status=\$T2_BATTERY_STATUS level=\$T2_BATTERY_LEVEL" >>"$T/hooklog"
EOF
for h in battery-critical battery-shutdown battery-ok; do
	cat >"$T/hooks/$h" <<EOF
#!/bin/sh
echo "$h cap=\$T2_BATTERY_CAPACITY level=\$T2_BATTERY_LEVEL" >>"$T/hooklog"
EOF
done
cat >"$T/hooks/fake-poweroff" <<EOF
#!/bin/sh
echo "poweroff" >>"$T/hooklog"
EOF
chmod +x "$T/hooks/battery-low.d/10-log" "$T/hooks/battery-critical" "$T/hooks/battery-shutdown" \
	"$T/hooks/battery-ok" "$T/hooks/fake-poweroff"

"$BIN" -c "$T/conf" -s "$SOCK" -r "$R" -d -f >"$T/log" 2>&1 &
PID=$!
for i in 1 2 3 4 5 6 7 8 9 10; do [ -S "$SOCK" ] && break; sleep 0.2; done
[ -S "$SOCK" ] || fail "no socket"
export T2_POWERD_SOCK="$SOCK"
# every request with a limit: a hung answer fails the test instead of stopping it for good
ctl() { timeout 20 "$BIN" ctl "$@" || { r=$?; [ $r = 124 ] && fail "ctl $* gave no complete answer within 20 s"; return $r; }; }

# 1. status while awake
out=$(ctl status)
case "$out" in *"state awake"*) ;; *) fail "status: $out";; esac

# 2. a held lease blocks the sleep: screen off, wait, nothing happens
("$BIN" ctl inhibit test-lease because testing -- sleep 1234 2>/dev/null) &	# holds: no limit
LEASEPID=$!
sleep 0.5
echo Off >"$R/sys/class/drm/card1-DSI-1/dpms"
sleep 2.5
grep -q 'lease test-lease' "$T/log" || fail "lease not seen"
grep -q 'dry run' "$T/log" && fail "slept despite the lease"
pkill -f 'sleep 1234' 2>/dev/null || true
wait $LEASEPID 2>/dev/null || true

# 3. without the lease it sleeps (dry run), the wake is "other" (no source grew)
wait_for "$T/log" 'dry run: not writing' || fail "did not suspend after the lease was released"
wait_for "$T/log" 'other wake' || fail "wake class not 'other'"
wait_for "$T/hooklog" 'resume class=other' || fail "post-resume hook did not run: $(cat "$T/hooklog" 2>/dev/null)"
# the RTC alarm was armed for the first maintenance interval (100 s)
alarm=$(cat "$R/sys/class/rtc/rtc0/wakealarm")
case "$alarm" in 0) ;; *) fail "RTC alarm not disarmed after resume: $alarm";; esac
grep -q 'RTC alarm in 100 s for maintenance' "$T/log" || fail "maintenance alarm not armed"

# 4. a user wake: gpio-keys event_count grows while the (dry, 2 s) sleep lasts
echo On >"$R/sys/class/drm/card1-DSI-1/dpms"
sleep 0.5
echo Off >"$R/sys/class/drm/card1-DSI-1/dpms"
next_sleep || fail "no sleep after the screen went off: $(tail -3 "$T/log")"
echo 5 >"$R/sys/class/wakeup/wakeup0/event_count"
wait_for "$T/log" 'user wake (sources: gpio-keys' || fail "user wake not classified: $(grep wake "$T/log" | tail -3)"

# 5. a modem wake, at the next sleep (user grace 2 s + delay 1 s)
next_sleep || fail "no sleep after the user wake: $(tail -3 "$T/log")"
echo 9 >"$R/sys/class/wakeup/wakeup1/event_count"
wait_for "$T/log" 'modem wake (sources: mddriver' || fail "modem wake not classified: $(grep wake "$T/log" | tail -3)"

# 6. a user alarm through the socket, in the past: fires at once
ctl alarm set wakeup +1 >/dev/null
wait_for "$T/log" 'alarm wakeup fired' || fail "alarm did not fire"

# 7. USB host blocks (a dry sleep of 2 s plus the 2 s "other" grace may be in progress: it is
#    seen at the end of that cycle)
echo configured >"$R/sys/class/udc/11201000.usb0/state"
wait_for "$T/log" 'USB host connected' || fail "USB host blocker not seen: $(tail -3 "$T/log")"
echo "not attached" >"$R/sys/class/udc/11201000.usb0/state"

# 8. the battery guard: the first "Battery" supply is found, levels fire once each on the
#    way down, the shutdown needs two readings in a row, a charger resets everything
# the screen on: no (dry) sleeps in between, the readings come every second
echo On >"$R/sys/class/drm/card1-DSI-1/dpms"
wait_for "$T/log" 'battery: /sys/class/power_supply/bat0' || fail "battery not found: $(grep -i battery "$T/log" | head -3)"
grep -q 'battery-' "$T/hooklog" && fail "a battery hook ran while charging: $(grep battery "$T/hooklog")"
echo Discharging >"$R/sys/class/power_supply/bat0/status"
echo 18 >"$R/sys/class/power_supply/bat0/capacity"
wait_for "$T/hooklog" 'battery-low cap=18 status=Discharging level=low' || fail "battery-low hook: $(grep battery "$T/hooklog")"
sleep 1.5	# one more reading: the hook must not run again
[ "$(grep -c 'battery-low' "$T/hooklog")" = 1 ] || fail "battery-low ran more than once"
echo 4 >"$R/sys/class/power_supply/bat0/capacity"
# one reading at the shutdown level: critical fired, the shutdown waits for a second one
wait_for "$T/log" 'confirming at the next reading' || fail "shutdown not deferred: $(grep -i battery "$T/log" | tail -3)"
grep -q 'battery-critical cap=4 level=critical' "$T/hooklog" || fail "battery-critical hook: $(grep battery "$T/hooklog")"
# a glitch: back up right after that reading, before the second one (a second later)
echo 30 >"$R/sys/class/power_supply/bat0/capacity"
wait_for "$T/hooklog" 'battery-ok cap=30' || fail "battery-ok after the glitch: $(grep battery "$T/hooklog")"
grep -q 'poweroff' "$T/hooklog" && fail "shut down on one odd reading"
# and for real: two readings
echo 3 >"$R/sys/class/power_supply/bat0/capacity"
wait_for "$T/hooklog" 'poweroff' || fail "no shutdown after two readings: $(grep -i battery "$T/log" | tail -4)"
grep -q 'battery-shutdown cap=3 level=shutdown' "$T/hooklog" || fail "battery-shutdown hook: $(grep battery "$T/hooklog")"
sleep 1.5	# one more reading: no second poweroff
[ "$(grep -c 'poweroff' "$T/hooklog")" = 1 ] || fail "poweroff ran more than once"
out=$(ctl status)
case "$out" in *"battery 3% Discharging level=shutdown"*) ;; *) fail "status battery line: $out";; esac
echo Charging >"$R/sys/class/power_supply/bat0/status"
wait_for "$T/hooklog" 'battery-ok' 2 || fail "battery-ok on the charger: $(grep battery "$T/hooklog")"

kill $PID; wait $PID 2>/dev/null || true

# 9. the time asleep spans the suspend: a dry sleep that counts 183 s as suspended (the
#    phone's 18:13:35 -> 18:16:38 case, logged as "resumed after 1.0 s" before) is logged as
#    183 s asleep, summed in the status, and an unexplained "other" wake after it is not a
#    short sleep (no back-off: on MONOTONIC every sleep looked short)
sed -i 's/^dry_run = yes$/dry_run = yes\ndry_run_asleep = 183/; s/^battery_poll = 1$/battery_poll = 1000/' "$T/conf"
echo On >"$R/sys/class/drm/card1-DSI-1/dpms"
: >"$T/log"
"$BIN" -c "$T/conf" -s "$SOCK" -r "$R" -d -f >"$T/log" 2>&1 &
PID=$!
for i in 1 2 3 4 5 6 7 8 9 10; do [ -S "$SOCK" ] && break; sleep 0.2; done
echo Off >"$R/sys/class/drm/card1-DSI-1/dpms"
wait_for "$T/log" 'resumed after' || :
grep -Eq 'resumed after 183\.[0-9] s asleep \(2\.[0-9] s in transitions\): other wake' "$T/log" ||
	fail "time asleep: $(grep 'resumed after' "$T/log")"
out=$(ctl status)
case "$out" in *"asleep 183 s"*) ;; *) fail "status asleep line: $out";; esac
case "$out" in *"backoff 0"*) ;; *) fail "back-off after a long sleep: $out";; esac
kill $PID; wait $PID 2>/dev/null || true

# 9b. a status that holds "ok" inside a line (battery level=ok) and arrives in one read: the
#     client stops at the "ok" line, not before and not never (it used to hang)
"$BIN" -c "$T/conf" -s "$SOCK" -r "$R" -d -f >"$T/log" 2>&1 &
PID=$!
for i in 1 2 3 4 5 6 7 8 9 10; do [ -S "$SOCK" ] && break; sleep 0.2; done
sleep 1.2
for i in 1 2 3 4 5 6 7 8 9 10; do
	out=$(ctl status)
	case "$out" in *"level=ok"*"ok") ;; *) fail "status $i: no battery level=ok line or no final ok: $out";; esac
done
kill $PID; wait $PID 2>/dev/null || true

# 9c. the same answer in one write, from a stand-in server that keeps the connection open (as
#     t2-powerd does): the old client read past its own end and waited for ever
if command -v python3 >/dev/null 2>&1; then
	python3 -c '
import os, socket, sys, time
s = socket.socket(socket.AF_UNIX); s.bind(sys.argv[1]); s.listen(1)
c, _ = s.accept(); c.recv(100)
c.sendall(b"state awake\nbattery 80% Charging level=ok\nmaintenance level=0\nok\n")
time.sleep(30)' "$T/one.sock" &
	SRV=$!
	for i in 1 2 3 4 5 6 7 8 9 10; do [ -S "$T/one.sock" ] && break; sleep 0.1; done
	out=$(T2_POWERD_SOCK="$T/one.sock" timeout 5 "$BIN" ctl status) ||
		fail "an answer in one read with \"level=ok\" inside: no end found ($out)"
	case "$out" in *"level=ok"*"ok") ;; *) fail "one-read answer: $out";; esac
	kill $SRV 2>/dev/null || true
fi

# 10. wake-screen (t2-fpd after a fingerprint match with the panel off): the screen-on hook runs,
#     subscribers get "screen on" at once, the phone stays up
cat >"$T/hooks/screen-on" <<EOF
#!/bin/sh
echo "screen-on hook" >>"$T/hooklog"
EOF
chmod +x "$T/hooks/screen-on"
: >"$T/log"
"$BIN" -c "$T/conf" -s "$SOCK" -r "$R" -d -f >"$T/log" 2>&1 &
PID=$!
for i in 1 2 3 4 5 6 7 8 9 10; do [ -S "$SOCK" ] && break; sleep 0.2; done
ctl screen off >/dev/null
( exec "$BIN" ctl subscribe >"$T/sub" 2>&1 ) &	# the client itself, so that kill reaches it
echo $! >"$T/subpid"
sleep 0.5
out=$(ctl wake-screen fingerprint)
[ "$out" = ok ] || fail "wake-screen reply: $out"
wait_for "$T/hooklog" 'screen-on hook' || fail "wake-screen: no screen-on hook"
wait_for "$T/sub" 'event screen on' || fail "wake-screen: no screen on for subscribers: $(cat "$T/sub")"
wait_for "$T/log" 'screen on requested (fingerprint)' || fail "wake-screen: not logged"
kill "$(cat "$T/subpid")" 2>/dev/null || true
kill $PID; wait $PID 2>/dev/null || true
echo "t2-powerd fake-sysfs test: OK"
