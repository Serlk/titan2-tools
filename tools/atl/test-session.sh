#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
#
# ATL test in the aarch64 chroot (scripts/build-atl.sh test): cage on the wlroots headless backend
# (pixman, no GPU), a 1440x1440 screen like the Titan 2's, Mesa llvmpipe for the app's EGL/GLES.
#   test-session.sh <name.apk> [android-translation-layer arguments...]
# Screenshots (grim) go to /root/out/<name>-NNN.png every SHOTS seconds, the log to /root/out/<name>.log.
set -u
APK=${1:?apk}
shift
NAME=$(basename "$APK" .apk)
OUT=/root/out
export HOME=/root XDG_RUNTIME_DIR=/run/user/0 XDG_SESSION_TYPE=wayland
mkdir -p -m 700 $XDG_RUNTIME_DIR
mkdir -p $HOME/.local/share/android_translation_layer $HOME/.cache
export WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1 WLR_HEADLESS_OUTPUTS=1
export LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe
export GDK_BACKEND=wayland
[ -z "${GSK:-}" ] || export GSK_RENDERER=$GSK
export ANDROID_LOG_TAGS="${ATL_LOG:-*:i}"

LOG=$OUT/$NAME.log
: >"$LOG"
echo "== $(date -u +%FT%TZ) $APK $* (uname -m: $(uname -m))" >>"$LOG"
# session bus: GtkApplication and libportal expect one
eval "$(dbus-launch --sh-syntax)"
export DBUS_SESSION_BUS_ADDRESS

cat >/tmp/client.sh <<EOF
#!/bin/sh
wlr-randr --output HEADLESS-1 --custom-mode 1440x1440 >>"$LOG" 2>&1
ATL_LOGFILE=0 exec atl-run "/root/apks/$APK" $* >>"$LOG" 2>&1
EOF
chmod 755 /tmp/client.sh
cage -d -- /tmp/client.sh >"$OUT/$NAME.cage.log" 2>&1 &
CAGE=$!

t=0
n=0
while [ $t -lt "$TIMEOUT" ]; do
	sleep "$SHOTS"
	t=$((t + SHOTS))
	kill -0 $CAGE 2>/dev/null || { echo "== cage exited at ${t} s" >>"$LOG"; break; }
	sock=$(ls $XDG_RUNTIME_DIR | grep -E '^wayland-[0-9]+$' | head -1)
	[ -n "$sock" ] || continue
	n=$((n + 1))
	WAYLAND_DISPLAY=$sock grim "$OUT/$NAME-$(printf %03d $n).png" 2>>"$LOG" &&
		echo "== screenshot $n at ${t} s" >>"$LOG"
done
kill $CAGE 2>/dev/null
sleep 2
kill -9 $CAGE 2>/dev/null
kill "$DBUS_SESSION_BUS_PID" 2>/dev/null
echo "== end ($t s)" >>"$LOG"
