# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
#
# The ALSA UCM2 profile of the Titan2 sound card (tools/ucm/Titan2: verbs HiFi and Voice Call,
# tools/ucm/README-voice-call.md) in the rootfs; sourced from scripts/netroot-serve.sh build
# ($R = image root). The conf.d directory is named after the card driver (mt6878-titan2).
mkdir -p $R/usr/share/alsa/ucm2/Titan2 $R/usr/share/alsa/ucm2/conf.d/mt6878-titan2
install -m 644 /work/tools/ucm/Titan2/Titan2.conf /work/tools/ucm/Titan2/HiFi.conf \
	/work/tools/ucm/Titan2/VoiceCall.conf $R/usr/share/alsa/ucm2/Titan2/
ln -sf ../../Titan2/Titan2.conf $R/usr/share/alsa/ucm2/conf.d/mt6878-titan2/Titan2.conf
