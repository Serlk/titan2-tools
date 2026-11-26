#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
#
# Build our small ATL test APKs -> out/atl-dl/test-apks
#   tools/atl/test-apps/build.sh [sensortest]   one app from tools/atl/test-apps/<name>, compiled
#                                               against its own compile-only stubs (<name>/stubs)
#                                               -> atltest-<name>.apk
#   tools/atl/test-apps/build.sh apptest        the package-catalogue test set from apptest/
#                                               -> out/atl-dl/test-apks/set/ (see the list at the end)
# Toolchain in docker (x86_64, local image titan2-atl-apkbuild:edge-2, created on first use):
# Alpine edge + openjdk8-jdk, android-build-tools (aapt2), art_standalone-dev (dx.jar, libcore), patch.
# The resources link against ATL's framework-res.apk (taken from the newest ATL package in
# out/atl/packages or out/atl/apks). apptest has no stubs: it compiles against the API classes of
# the ATL source the package was built from (the source archive kept by scripts/build-atl.sh build
# plus patches/android-translation-layer), so it can use everything ATL implements.
# Signed (v1, jarsigner) with throwaway keys out/atl-dl/test-apks/atltest.keystore (and
# atltest2.keystore, a second signer for the refusal tests).
set -eu
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
name=${1:-sensortest}
src=$ROOT/tools/atl/test-apps/$name
[ -f "$src/AndroidManifest.xml" ] || { echo "no $src/AndroidManifest.xml" >&2; exit 2; }
IMG=titan2-atl-apkbuild:edge-2
if ! docker image inspect "$IMG" >/dev/null 2>&1; then
	c=t2-apkbuild-$$
	docker run --name "$c" alpine:3.24 sh -c 'printf "%s\n" https://dl-cdn.alpinelinux.org/alpine/edge/main \
		https://dl-cdn.alpinelinux.org/alpine/edge/community https://dl-cdn.alpinelinux.org/alpine/edge/testing \
		>/etc/apk/repositories && apk upgrade --no-cache -a >/dev/null 2>&1; apk add --no-cache openjdk8-jdk android-build-tools art_standalone-dev zip unzip patch'
	docker commit "$c" "$IMG" >/dev/null
	docker rm "$c" >/dev/null
fi
pkg=$(ls -t "$ROOT"/out/atl/packages/aports/aarch64/android-translation-layer-0*.apk "$ROOT"/out/atl/apks/android-translation-layer-0*.apk 2>/dev/null | head -1)
[ -n "$pkg" ] || { echo "no ATL package (scripts/build-atl.sh rootfs|build)" >&2; exit 1; }
outdir=$ROOT/out/atl-dl/test-apks
mkdir -p "$outdir"

if [ "$name" != apptest ]; then
	docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$src:/app:ro" -v "$pkg:/atl.apk:ro" -v "$outdir:/out" "$IMG" sh -euc '
		w=$(mktemp -d)
		cd "$w"
		tar xzf /atl.apk usr/lib/java/dex/android_translation_layer/framework-res.apk 2>/dev/null
		mkdir cls
		/usr/lib/jvm/java-1.8-openjdk/bin/javac -nowarn -source 1.8 -target 1.8 -encoding UTF-8 -implicit:none \
			-bootclasspath /usr/lib/java/core-all_classes.jar -sourcepath /app/stubs -d cls $(find /app/src -name "*.java")
		java -jar /usr/lib/java/dx.jar --dex --output=classes.dex cls
		aapt2 link --manifest /app/AndroidManifest.xml -I usr/lib/java/dex/android_translation_layer/framework-res.apk -o app.apk
		zip -qj app.apk classes.dex
		ks=/out/atltest.keystore
		[ -f $ks ] || keytool -genkeypair -keystore $ks -storepass atltest -keypass atltest -alias atltest \
			-keyalg RSA -keysize 2048 -validity 10000 -dname CN=atltest >/dev/null 2>&1
		jarsigner -keystore $ks -storepass atltest -sigalg SHA256withRSA -digestalg SHA-256 app.apk atltest >/dev/null
		cp app.apk /out/atltest-'"$name"'.apk
		unzip -l /out/atltest-'"$name"'.apk'
	echo "-> $outdir/atltest-$name.apk"
	exit 0
fi

# --- apptest: one source, several packages/versions/signers/splits ---
tarball=$(ls -t "$ROOT"/out/atl/buildroot/var/cache/distfiles/android_translation_layer-*.tar.gz 2>/dev/null | head -1)
[ -n "$tarball" ] || { echo "no ATL source archive in out/atl/buildroot/var/cache/distfiles (scripts/build-atl.sh build)" >&2; exit 1; }
mkdir -p "$outdir/set" "$outdir/.api"
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$src:/app:ro" -v "$pkg:/atl.apk:ro" -v "$outdir:/out" \
	-v "$tarball:/atl-src.tar.gz:ro" -v "$ROOT/patches/android-translation-layer:/patches:ro" "$IMG" sh -euc '
	JAVAC="/usr/lib/jvm/java-1.8-openjdk/bin/javac -nowarn -source 1.8 -target 1.8 -encoding UTF-8"
	w=$(mktemp -d)
	cd "$w"
	tar xzf /atl.apk usr/lib/java/dex/android_translation_layer/framework-res.apk 2>/dev/null
	FW=$w/usr/lib/java/dex/android_translation_layer/framework-res.apk

	# the API classes of the patched ATL source; rebuilt when the source or a patch changes
	stamp=$(cat /atl-src.tar.gz /patches/*.patch | sha256sum | cut -c1-16)
	api=/out/.api/$stamp
	if [ ! -f "$api/.done" ]; then
		rm -rf /out/.api/* && mkdir -p "$api" atl
		tar xzf /atl-src.tar.gz -C atl --strip-components=1
		for p in /patches/*.patch; do patch -s -p1 -d atl <"$p"; done
		(cd atl/src/api-impl && grep -o "^[[:space:]]*\x27[^\x27]*\.java\x27" meson.build | tr -d "\x27 \t" | sort -u >"$w/srcs" &&
			$JAVAC -proc:none -bootclasspath /usr/lib/java/core-all_classes.jar -d "$api" @"$w/srcs" 2>&1 | grep -v "^Note:" || :)
		[ -f "$api/android/app/Activity.class" ] || { echo "API classes did not compile" >&2; exit 1; }
		touch "$api/.done"
	fi

	mkdir cls
	$JAVAC -implicit:none -bootclasspath /usr/lib/java/core-all_classes.jar -classpath "$api" -d cls $(find /app/src -name "*.java")
	java -jar /usr/lib/java/dx.jar --dex --output=classes.dex cls

	key() { # keystore file, created on first use
		[ -f "$1" ] || keytool -genkeypair -keystore "$1" -storepass atltest -keypass atltest -alias atltest \
			-keyalg RSA -keysize 2048 -validity 10000 -dname "CN=$(basename "$1" .keystore)" >/dev/null 2>&1
	}
	key /out/atltest.keystore
	key /out/atltest2.keystore

	# variant OUT PACKAGE VERSIONCODE KEYSTORE [manifest attributes] : a base APK
	variant() {
		out=$1 p=$2 vc=$3 ks=$4 attrs=${5:-}
		sed "s|@PKG@|$p|g; s|@VC@|$vc|g; s|@MANIFEST_ATTRS@|$attrs|" /app/AndroidManifest.xml >m.xml
		rm -f v.apk
		aapt2 link --manifest m.xml -I "$FW" -o v.apk
		zip -qj v.apk classes.dex
		[ -z "${EXTRA_ZIP_DIR:-}" ] || (cd "$EXTRA_ZIP_DIR" && zip -qr "$w/v.apk" .)
		jarsigner -keystore "$ks" -storepass atltest -sigalg SHA256withRSA -digestalg SHA-256 v.apk atltest >/dev/null
		mkdir -p "$(dirname "/out/set/$out")"
		cp v.apk "/out/set/$out"
	}
	# split OUT PACKAGE VERSIONCODE KEYSTORE SPLITNAME : a split APK without code
	split() {
		out=$1 p=$2 vc=$3 ks=$4 s=$5
		printf "%s\n" "<?xml version=\"1.0\" encoding=\"utf-8\"?>" \
			"<manifest xmlns:android=\"http://schemas.android.com/apk/res/android\" package=\"$p\" split=\"$s\" android:versionCode=\"$vc\">" \
			"<application android:hasCode=\"false\" /></manifest>" >s.xml
		rm -f s.apk
		aapt2 link --manifest s.xml -I "$FW" -o s.apk
		mkdir -p sx/assets && echo "payload of split $s" >sx/assets/split-$s.txt && (cd sx && zip -qr ../s.apk assets) && rm -rf sx
		jarsigner -keystore "$ks" -storepass atltest -sigalg SHA256withRSA -digestalg SHA-256 s.apk atltest >/dev/null
		mkdir -p "$(dirname "/out/set/$out")"
		cp s.apk "/out/set/$out"
	}
	K1=/out/atltest.keystore K2=/out/atltest2.keystore
	rm -rf /out/set/*
	variant a/base.apk org.titan2.atltest.app 1 $K1
	variant b/base.apk org.titan2.atltest.other 1 $K2
	variant app-v2-renamed.apk org.titan2.atltest.app 2 $K1
	variant app-v3-other-signer.apk org.titan2.atltest.app 3 $K2
	variant other-same-signer-as-app.apk org.titan2.atltest.same 1 $K1
	variant split/base.apk org.titan2.atltest.split 1 $K1 " android:isSplitRequired=\"true\""
	split split/split_config.extra.apk org.titan2.atltest.split 1 $K1 config.extra
	split split/wrong-version.apk org.titan2.atltest.split 2 $K1 config.other
	split split/wrong-signer.apk org.titan2.atltest.split 1 $K2 config.signer
	mkdir -p x32/lib/armeabi-v7a && echo "not a real library" >x32/lib/armeabi-v7a/libdummy.so
	EXTRA_ZIP_DIR=$w/x32 variant arm32-only.apk org.titan2.atltest.arm32 1 $K1
	mkdir -p x64/lib/arm64-v8a x64/lib/armeabi-v7a && echo "64" >x64/lib/arm64-v8a/libdummy.so && echo "32" >x64/lib/armeabi-v7a/libdummy.so
	EXTRA_ZIP_DIR=$w/x64 variant with-arm64-lib.apk org.titan2.atltest.arm64 1 $K1
	unset EXTRA_ZIP_DIR
	# a signed file changed afterwards: the first byte of classes.dex data region is flipped
	cp /out/set/a/base.apk /out/set/tampered.apk
	off=$(( $(stat -c %s /out/set/tampered.apk) / 2 ))
	printf "\\377" | dd of=/out/set/tampered.apk bs=1 seek=$off conv=notrunc 2>/dev/null
	(cd /out/set && find . -type f | sort | xargs sha256sum >SHA256SUMS && cat SHA256SUMS)'
cat <<'LIST'
-> out/atl-dl/test-apks/set/
   a/base.apk                    org.titan2.atltest.app   version 1, signer 1
   b/base.apk                    org.titan2.atltest.other version 1, signer 2 (same file name, another package)
   app-v2-renamed.apk            org.titan2.atltest.app   version 2, signer 1 (update from a differently named file)
   app-v3-other-signer.apk       org.titan2.atltest.app   version 3, signer 2 (must be refused as an update)
   other-same-signer-as-app.apk  org.titan2.atltest.same  version 1, signer 1 (holds the signature permission)
   split/base.apk + split/split_config.extra.apk   org.titan2.atltest.split, base requires its split
   split/wrong-version.apk, split/wrong-signer.apk splits that do not belong to that base
   arm32-only.apk                only lib/armeabi-v7a (must be refused on arm64)
   with-arm64-lib.apk            lib/arm64-v8a + lib/armeabi-v7a
   tampered.apk                  a/base.apk with one byte changed after signing (must be refused)
LIST
