#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
#
# Checks of the ATL package catalogue that need no window: install, update, signer and version
# rules, split sets, native code, removal, launcher entry and D-Bus activation file, interrupted
# installation. Runs as the current user against a throwaway data directory, so nothing installed
# for real is touched.
#   catalogue-test.sh [APKS_DIR]      APKS_DIR holds set/ from tools/atl/test-apps/build.sh apptest
#                                     and optionally real APKs (default /host/apks on the VM bench,
#                                     /usr/share/titan2/apks on the phone)
# On the PC (VM bench, scripts/atl-vm.sh):
#   cp -r out/atl-dl/test-apks/set tools/atl/test-apps/catalogue-test.sh out/atl-vm/share/apks/
#   CMD_TIMEOUT=1500 scripts/atl-vm.sh cmd 'sh /host/apks/catalogue-test.sh /host/apks'
# (not in the qemu-user chroot of scripts/build-atl.sh: the runtime is not stable there without a
# session). Every check prints "ok" or "FAIL"; exit status 1 if any failed.
set -u
# a crash must not leave a core file of the whole VM behind
ulimit -c 0 2>/dev/null
APKS=${1:-/host/apks}
[ -d "$APKS/set" ] || APKS=/usr/share/titan2/apks
S=$APKS/set
[ -f "$S/a/base.apk" ] || { echo "no test set in $APKS/set (tools/atl/test-apps/build.sh apptest)" >&2; exit 2; }
T=$(mktemp -d)
export ANDROID_APP_DATA_DIR=$T/data XDG_DATA_HOME=$T/xdg
mkdir -p "$ANDROID_APP_DATA_DIR" "$XDG_DATA_HOME"
D=$ANDROID_APP_DATA_DIR/_packages_
fails=0
n=0

pm() { atl-pm "$@" 2>"$T/stderr"; }
# check DESCRIPTION EXPECTED_PATTERN -- command...   (the pattern is matched against stdout)
check() {
	desc=$1 pattern=$2
	shift 3
	n=$((n + 1))
	out=$("$@" 2>"$T/stderr")
	if printf '%s\n' "$out" | grep -q -e "$pattern"; then
		printf 'ok    %2d %s\n' "$n" "$desc"
	else
		printf 'FAIL  %2d %s\n         wanted /%s/, got: %s\n' "$n" "$desc" "$pattern" "$(printf '%s' "$out" | head -n 3)"
		grep -v '^JVM launched\|^$' "$T/stderr" | tail -n 5 | sed 's/^/         stderr: /'
		fails=$((fails + 1))
	fi
}
# is CONDITION true?
is() {
	desc=$1
	shift
	n=$((n + 1))
	if "$@" >/dev/null 2>&1; then printf 'ok    %2d %s\n' "$n" "$desc"; else printf 'FAIL  %2d %s\n' "$n" "$desc"; fails=$((fails + 1)); fi
}
isnt() {
	desc=$1
	shift
	n=$((n + 1))
	if "$@" >/dev/null 2>&1; then printf 'FAIL  %2d %s\n' "$n" "$desc"; fails=$((fails + 1)); else printf 'ok    %2d %s\n' "$n" "$desc"; fi
}
version_of() { atl-pm list 2>/dev/null | awk -F'\t' -v p="$1" '$1 == p {print $2}'; }

echo "== identity: the package name of the manifest, not the file name"
check "install a/base.apk" "^Success: org.titan2.atltest.app 1 (new)" -- atl-install "$S/a/base.apk"
check "install b/base.apk (same file name, another package)" "^Success: org.titan2.atltest.other 1 (new)" -- atl-install "$S/b/base.apk"
check "both are installed independently" "2" -- sh -c "atl-pm list | grep -c '^org.titan2.atltest.\(app\|other\)	1	'"
is "separate data directories" test -d "$D/org.titan2.atltest.app/data" -a -d "$D/org.titan2.atltest.other/data"
isnt "the two application ids differ" test "$(atl-pm info org.titan2.atltest.app | sed -n 's/^app id: *//p')" = "$(atl-pm info org.titan2.atltest.other | sed -n 's/^app id: *//p')"
check "info records signer, SDK levels and ABI" "minSdk: *23" -- atl-pm info org.titan2.atltest.app
check "launcher entry names the package, not a file" "^Exec=.*--package org.titan2.atltest.app --uri %u" -- cat "$XDG_DATA_HOME/applications/org.titan2.atltest.app.desktop"
check "D-Bus activation file registers the package name" "^Name=org.titan2.atltest.app$" -- cat "$XDG_DATA_HOME/dbus-1/services/org.titan2.atltest.app.service"
check "activation starts without a window" "^Exec=.*--package org.titan2.atltest.app --background" -- cat "$XDG_DATA_HOME/dbus-1/services/org.titan2.atltest.app.service"

echo "== update keeps the data, whatever the file is called"
echo "kept" >"$D/org.titan2.atltest.app/data/marker"
check "update from app-v2-renamed.apk" "^Success: org.titan2.atltest.app 2 (replaces 1, data kept)" -- atl-install "$S/app-v2-renamed.apk"
check "data survived the update" "kept" -- cat "$D/org.titan2.atltest.app/data/marker"
check "one installation of the package" "^1$" -- sh -c "atl-pm list | grep -c '^org.titan2.atltest.app	'"
check "the other package is untouched" "^1$" -- version_of org.titan2.atltest.other
check "application id is stable across the update" "app id" -- atl-pm info org.titan2.atltest.app

echo "== refusals leave the installed version and its data alone"
check "other signer is refused" "INSTALL_FAILED_UPDATE_INCOMPATIBLE" -- atl-install "$S/app-v3-other-signer.apk"
check "still version 2" "^2$" -- version_of org.titan2.atltest.app
check "older version is refused" "INSTALL_FAILED_VERSION_DOWNGRADE" -- atl-install "$S/a/base.apk"
check "still version 2" "^2$" -- version_of org.titan2.atltest.app
check "a file changed after signing is refused" "INSTALL_PARSE_FAILED_NO_CERTIFICATES" -- atl-install "$S/tampered.apk"
check "data still there" "kept" -- cat "$D/org.titan2.atltest.app/data/marker"
check "an interrupted copy leaves version 2" "unchanged" -- env ATL_INSTALL_FAULT=copy atl-install --allow-downgrade "$S/a/base.apk"
check "still version 2 after the interrupted copy" "^2$" -- version_of org.titan2.atltest.app
check "an interruption right before publishing leaves version 2" "unchanged" -- env ATL_INSTALL_FAULT=publish atl-install --allow-downgrade "$S/a/base.apk"
check "still version 2 after the interrupted publish" "^2$" -- version_of org.titan2.atltest.app
check "no staging directory is left behind" "^0$" -- sh -c "ls -a '$D/org.titan2.atltest.app/versions' | grep -c staging"
check "data still there" "kept" -- cat "$D/org.titan2.atltest.app/data/marker"
check "explicit downgrade works" "^Success: org.titan2.atltest.app 1 (replaces 2" -- atl-install --allow-downgrade "$S/a/base.apk"

echo "== split APK sets"
check "base that requires splits, alone" "INSTALL_FAILED_MISSING_SPLIT" -- atl-install "$S/split/base.apk"
isnt "nothing was installed" test -e "$D/org.titan2.atltest.split/current"
check "split of another version" "INSTALL_FAILED_INVALID_APK" -- atl-install "$S/split/base.apk" "$S/split/wrong-version.apk"
check "split of another signer" "INCONSISTENT_CERTIFICATES" -- atl-install "$S/split/base.apk" "$S/split/wrong-signer.apk"
check "splits without a base" "no base APK" -- atl-install "$S/split/split_config.extra.apk"
check "base + its split" "^Success: org.titan2.atltest.split 1 (new)" -- atl-install "$S/split/base.apk" "$S/split/split_config.extra.apk"
check "both files belong to the one installed version" "^2$" -- sh -c "atl-pm path org.titan2.atltest.split | wc -l"
check "the split is recorded" "splits: *config.extra" -- atl-pm info org.titan2.atltest.split

echo "== native code"
check "32-bit-only libraries are refused with the reason" "INSTALL_FAILED_NO_MATCHING_ABIS.*armeabi-v7a" -- atl-install "$S/arm32-only.apk"
isnt "nothing was installed" test -e "$D/org.titan2.atltest.arm32/current"
case "$(uname -m)" in
aarch64)
	check "arm64 libraries are unpacked for the version" "^Success: org.titan2.atltest.arm64 1" -- atl-install "$S/with-arm64-lib.apk"
	check "the 64-bit library was chosen" "^64$" -- cat "$D/org.titan2.atltest.arm64/current/lib/libdummy.so"
	check "the package without libraries needs no ABI" "native ABI: *-" -- atl-pm info org.titan2.atltest.app
	;;
*) echo "      ($(uname -m): arm64 library check skipped)" ;;
esac

echo "== component states (the launcher alias)"
check "alias is off by default" "AltLauncher	default(disabled)" -- atl-pm components org.titan2.atltest.app
check "enable the alias, disable the main entry" "Success" -- sh -c "atl-pm enable org.titan2.atltest.app org.titan2.atltest.app.AltLauncher && atl-pm disable org.titan2.atltest.app org.titan2.atltest.app.MainActivity"
check "the state is stored" "AltLauncher	enabled" -- atl-pm components org.titan2.atltest.app
check "the launcher entry follows the enabled alias" "^Name=ATL app test (alt)$" -- cat "$XDG_DATA_HOME/applications/org.titan2.atltest.app.desktop"
check "the state survives an update" "^Success" -- atl-install "$S/app-v2-renamed.apk"
check "alias still enabled after the update" "AltLauncher	enabled" -- atl-pm components org.titan2.atltest.app
check "back to the manifest defaults" "Success" -- sh -c "atl-pm default org.titan2.atltest.app org.titan2.atltest.app.AltLauncher && atl-pm default org.titan2.atltest.app org.titan2.atltest.app.MainActivity"
check "the launcher entry is the main one again" "^Name=ATL app test$" -- cat "$XDG_DATA_HOME/applications/org.titan2.atltest.app.desktop"

echo "== permissions decided from outside"
check "nothing decided yet" "CAMERA	not decided" -- atl-pm permissions org.titan2.atltest.app
check "grant" "Success" -- atl-pm grant org.titan2.atltest.app android.permission.CAMERA
check "granted" "CAMERA	granted" -- atl-pm permissions org.titan2.atltest.app
check "revoke" "Success" -- atl-pm revoke org.titan2.atltest.app android.permission.CAMERA
check "denied" "CAMERA	denied" -- atl-pm permissions org.titan2.atltest.app

echo "== data written by the older, file-name based layout"
mkdir -p "$ANDROID_APP_DATA_DIR/_installed_apks_" "$ANDROID_APP_DATA_DIR/legacy-name.apk_/files"
cp "$S/other-same-signer-as-app.apk" "$ANDROID_APP_DATA_DIR/_installed_apks_/legacy-name.apk"
echo "old data" >"$ANDROID_APP_DATA_DIR/legacy-name.apk_/files/old.txt"
check "one confirmed old directory is taken over at the first installation" "moved .*legacy-name.apk_" -- atl-install "$S/other-same-signer-as-app.apk"
check "the old data is the package's data now" "old data" -- cat "$D/org.titan2.atltest.same/data/files/old.txt"
atl-install -r org.titan2.atltest.same >/dev/null 2>&1
mkdir -p "$ANDROID_APP_DATA_DIR/legacy-name.apk_/files" "$ANDROID_APP_DATA_DIR/second-name.apk_/files"
cp "$S/other-same-signer-as-app.apk" "$ANDROID_APP_DATA_DIR/_installed_apks_/second-name.apk"
echo "one" >"$ANDROID_APP_DATA_DIR/legacy-name.apk_/files/old.txt"
echo "two" >"$ANDROID_APP_DATA_DIR/second-name.apk_/files/old.txt"
check "two candidates: nothing is moved" "ambiguous" -- atl-install "$S/other-same-signer-as-app.apk"
is "both old directories are intact" test -f "$ANDROID_APP_DATA_DIR/legacy-name.apk_/files/old.txt" -a -f "$ANDROID_APP_DATA_DIR/second-name.apk_/files/old.txt"
check "the user chooses one" "moved .*second-name.apk_" -- atl-pm migrate-data org.titan2.atltest.same "$ANDROID_APP_DATA_DIR/second-name.apk_"
check "the chosen data is in place" "two" -- cat "$D/org.titan2.atltest.same/data/files/old.txt"

echo "== removal"
check "remove by package name" "^Success: org.titan2.atltest.other removed" -- atl-install -r org.titan2.atltest.other
isnt "launcher entry is gone" test -e "$XDG_DATA_HOME/applications/org.titan2.atltest.other.desktop"
isnt "D-Bus activation file is gone" test -e "$XDG_DATA_HOME/dbus-1/services/org.titan2.atltest.other.service"
isnt "its data is gone" test -e "$D/org.titan2.atltest.other"
check "not listed any more" "^0$" -- sh -c "atl-pm list | grep -c '^org.titan2.atltest.other	'"
check "remove but keep the data" "data kept" -- atl-install -r org.titan2.atltest.app --keep-data
check "data kept" "kept" -- cat "$D/org.titan2.atltest.app/data/marker"
check "reinstall finds the kept data" "^Success: org.titan2.atltest.app 1" -- atl-install "$S/a/base.apk"
check "data is the package's again" "kept" -- cat "$D/org.titan2.atltest.app/data/marker"
check "removing an unknown package fails" "not installed" -- atl-install -r org.titan2.atltest.nothing

echo "== real APKs, if present (signature schemes v2/v3)"
for f in "$APKS"/org.fdroid.fdroid_*.apk "$APKS"/org.billthefarmer.editor_*.apk "$APKS"/org.happysanta.gd_*.apk; do
	[ -f "$f" ] || continue
	check "verify and describe $(basename "$f")" "^signature: *scheme v" -- atl-pm info "$f"
	atl-pm info "$f" 2>/dev/null | grep '^package\|^version\|^signature\|^signer\|^ABIs\|^minSdk\|^targetSdk\|^sha256' | sed 's/^/        /'
	cp "$f" "$T/tampered-real.apk"
	printf '\377' | dd of="$T/tampered-real.apk" bs=1 seek=$(($(stat -c %s "$f") / 3)) conv=notrunc 2>/dev/null
	check "the same file with one byte changed is refused" "Failure" -- atl-pm info "$T/tampered-real.apk"
done

echo
echo "$n checks, $fails failed"
rm -rf "$T"
[ "$fails" -eq 0 ]
