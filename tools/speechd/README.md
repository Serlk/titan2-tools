<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
<!-- Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com> -->
# t2-speechd

Voice-call audio for the Titan 2 (MediaTek MT6878) on mainline Linux. Voice samples never pass
through AP memory: the modem's speech engine talks to the audio front end directly over PCM
interface 1. Three things must be switched on for a call to be heard: the modem side of the AP
audio path (the PCM 1 interconnect switches and the hostless "Voice Call" PCM held open), the codec
(receiver/headphone/speaker and microphone routes and gains), and the modem's speech engine (the
"speech on" message over `/dev/ccci_aud`). `t2-speechd` does the first and the last, on command
from a UNIX socket, and answers the modem's speech notifications. The codec is set by the
**"Voice Call" UCM verb** ([tools/ucm/Titan2/VoiceCall.conf](../ucm/Titan2/VoiceCall.conf)), i.e.
by PipeWire/WirePlumber when they switch the card to a "Voice Call …" profile; see
[tools/ucm/README-voice-call.md](../ucm/README-voice-call.md) for who owns which control and how
the pieces are installed. With `-r 1` (`codec_routes = 1`) the daemon sets the codec itself, as it
did before 2026-10-29 — a fallback for when the Voice Call profile is not in place.

The daemon was written from a clean-room behavioural description of the modem's speech
protocol (facts only; no code of the factory firmware was used); the protocol is documented in
the source comments. The kernel side (control names, PCM device 4, the interface contract) is
in the kernel driver's documentation.

| file | what |
|---|---|
| `src/proto.c` | message framing, message classes, ack timing, speech info block (libc only) |
| `src/engine.c` | message queue and acks, notifications, modem loss and refresh, commands (libc only) |
| `src/shm.c` | the `/dev/ccci_raw_audio` rings: format, index reset, indirect modem data (libc only) |
| `src/audio.c` | AP path tables (control names, order) on top of a small backend interface; the codec tables only with `-r 1` |
| `src/audio_alsa.c` | alsa-lib backend: control API by name, Voice Call PCM holder |
| `src/main.c` | options, the port loop, control socket, client mode |
| `src/replay.c` | offline replay with a fake port, clock, modem state and ALSA |

## Build

C11. The speech part uses libc only; the ALSA part uses alsa-lib.

    make                    # host build: ./t2-speechd (needs alsa-lib headers)
    make test               # ASan/UBSan build + host test (python3 -I)

For the phone (Alpine aarch64, musl). The binary is linked **dynamically** against
`libasound.so.2`, so the phone needs `alsa-lib` installed (`apk add alsa-lib`; Alpine has no
`alsa-lib-static`, so unlike `t2-modemd` this one cannot be `-static`). The result is left as
`t2-speechd-aarch64` so the host binary is not overwritten:

    SR=$REPO/out/musl-aarch64/sysroot
    PATH=$REPO/toolchains/clang-r574158/bin:$PATH \
    make BIN=t2-speechd-aarch64 \
         CC="clang --target=aarch64-alpine-linux-musl --sysroot=$SR -fuse-ld=lld --gcc-toolchain=$SR/usr"

If the sysroot has no alsa-lib yet:

    docker run --rm -v $REPO/out/musl-aarch64:/sr alpine:3.24 sh -c \
      'apk --arch aarch64 --root /sr/sysroot --allow-untrusted \
       -X https://dl-cdn.alpinelinux.org/alpine/v3.24/main --no-scripts add alsa-lib-dev &&
       chown -R 1000:1000 /sr'

## Run

    t2-speechd [options] -d /dev/ccci_aud          # daemon
    t2-speechd [-s SOCKET] COMMAND [ARGS]          # client: sends one command, prints the reply
    t2-speechd [options] -R script -o out.txt      # offline replay

The OpenRC service `t2-speechd.initd` (install as `/etc/init.d/t2-speechd`, binary in
`/usr/local/bin`) needs `t2-modem` and runs the daemon as root. `/etc/conf.d/t2-speechd`
(`t2-speechd.conf`) has `T2_SPEECHD_CODEC_ROUTES=0|1` (passed as `-r`) and `T2_SPEECHD_ARGS`.

| option | |
|---|---|
| `-d DEV` | speech control port. Opening is retried every 10 ms, 200 times, then every 2 s |
| `-m DEV` | raw-audio shared memory (`/dev/ccci_raw_audio`), `none` = not used. Only needed when the modem sends data indirectly through the ring |
| `-S FILE` | modem state file (`/sys/kernel/debug/d180000.mddriver/fsm`, first line `state: ready`); `none` = no state source |
| `-a` | with no readable state source: the modem counts as ready as soon as the port is open |
| `-s SOCK` | control socket (`/run/t2-speechd.sock`, mode 0660) |
| `-p FILE` | state kept across daemon restarts (`/run/t2-speechd.state`), `none` = off |
| `-I FILE` | "shared memory formatted this boot" marker (`/run/t2-speechd.shm-init`) |
| `-c CARD` | ALSA control device (`hw:Titan2`) |
| `-P PCM` | the Voice Call PCM (`hw:Titan2,4`) |
| `-f RATE` | call rate when `call-start` gives none (48000) |
| `-r 0\|1` | `codec_routes`: `0` (default) = only the PCM 1 switches and the Voice Call PCM, the codec is left to the "Voice Call" UCM verb (PipeWire); `1` = set the codec controls too (old behaviour, fallback) |
| `-H VAL` | `Handset Volume` at call start (`0` = −10 dB; up to 18 = +8 dB); `''` = leave it (`-r 1` only) |
| `-K VAL` | `Speaker Volume` at call start (512 of 1023); `''` = leave it (`-r 1` only) |
| `-A N` | info-block smart-PA byte (0x54), default 1 (one FS199x) |
| `-C` | info-block common parameters all 0 instead of the factory "Common" values |
| `-T MS` | how long the message queue waits for an ack (60000) |
| `-v` | log every packet |

### Commands

| command | what happens |
|---|---|
| `call-start [--rate 48000\|32000\|16000\|8000] [--device earpiece\|speaker]` | AP path on, Voice Call PCM opened at the rate, then **speech on** (0x2F20) with the same rate, then DL gain (0x2F00) and UL gain (0x2F01) |
| `call-stop` | UL mute (0x2F02 = 1), Voice Call PCM closed, AP path off, **speech off** (0x2F21), UL mute back to the user setting (0x2F02) |
| `device earpiece\|speaker` | in a call: DL mute (0x2F03 = 1), UL mute (0x2F02 = 1), output switched, routing (0x2F2B), DL unmute, UL back. Outside a call: remembered for the next one |
| `volume 0..6` | voice volume index; in a call 0x2F00 is sent when the value changes |
| `mute on\|off` | microphone mute; in a call 0x2F02, otherwise remembered (field 0x11 of the next speech on) |
| `status` | modem/call/queue state, last band and codec info, counters |

Each reply ends with a line `ok` (exit code 0) or `error: …` (exit code 1). Lines before it say
whether the modem acked: `acked`, `pending` (the caller's wait ran out, the ack may
still come) or `not acked` (dropped, write error or timeout: a local ack was synthesised).

Example (earpiece, wideband):

    t2-speechd call-start --rate 16000
    t2-speechd volume 5
    t2-speechd device speaker
    t2-speechd call-stop

### What call-start sets

Always (the modem side of the path): to the modem `PCM_1_PB_CH1 ADDA_UL_CH1`,
`PCM_1_PB_CH2 ADDA_UL_CH2`; from the modem `ADDA_DL_CH1/CH2 PCM_1_CAP_CH1` (earpiece) or
`I2SOUT4_CH1/CH2 PCM_1_CAP_CH1` (speaker). Then `hw:Titan2,4` is opened for playback and capture
(S16_LE, 2 channels, the call rate, period 1024 × 2), with `stop_threshold` = boundary, and
started. No data is written; the PCM stays open until `call-stop`. A failing PCM start is retried
once; if it fails again or a PCM 1 switch cannot be set, the path is undone and no speech on is
sent. DPCM needs an enabled back end to open the hostless front end, which is why these switches
stay in the daemon although the "Voice Call" UCM verb sets the same ones (same values, so the two
never disagree; a device change in PipeWire moves the downlink switches with the output).

Only with `-r 1` (`codec_routes = 1`), before the switches above — earpiece: `RCV Mux`=`Voice
Playback`, `Handset Volume`, `Earpiece Switch`=on; speaker: `Speaker Volume`, `Speaker Switch`=on;
microphones (both devices): `UL_SRC_MUX`=`AMIC`, `MISO0_MUX`=`UL1_CH1`, `MISO1_MUX`=`UL1_CH2`,
`PGA_L_Mux`=`AIN0`, `ADC_L_Mux`=`Left Preamplifier`, `PGA_R_Mux`=`AIN2`, `ADC_R_Mux`=`Right
Preamplifier`, `PGA1/PGA2 Volume`=2, `Main Mic Switch` and `Second Mic Switch`=on. `RCV Mux` is
required, the others optional (logged). These are the values the "Voice Call" verb uses too.

`call-stop` sets the switches back to 0 in reverse order (and with `-r 1` `RCV Mux`=`Open`, the
pin switches off; muxes and volumes are left alone). `device` in a call moves only the downlink
switches (`-r 0`) or the downlink switches and the output codec part (`-r 1`).

## Modem readiness

Our driver has no GET_MD_STATE ioctl. The modem counts as ready when `/dev/ccci_aud` is open and
the state file (`-S`) reads `state: ready`. Without a readable state file (`-S none`, no debugfs),
it counts as ready after the modem's alive notification (0xAFA0), or right away with `-a`. The
kernel itself refuses writes with ENODEV while the modem is not running; that is handled as a
modem loss.

During a call the state is checked every 200 ms. A modem that stops being ready (or an ENODEV
write, or EPOF 0xAF78) marks the modem lost: the outstanding ack is synthesised, the Voice Call
PCM is closed, and every message except the 0x2FA0 answer is dropped. When the modem sends 0xAFA0
again, the daemon answers it and, if the call is still on, re-runs it: PCM open, speech on with
the current state, gains, mute. If the state file says ready for 10 s and no 0xAFA0 came, the
daemon resumes anyway.

## Offline replay and tests

`-R script -o out` replaces the port, the clock, the modem state and ALSA by a script. The format
is described at the top of `src/replay.c`. `tests/test_replay.py` uses it to check every message
byte by byte (speech on for earpiece/speaker at 48/32/16/8 kHz, gains, mute, speech off, device
switch, notification answers, acks in and out of pair, the 50/300/500 ms caller waits and the
60 s queue timeout, modem loss and refresh, start-up recovery, the shared-memory format and the
indirect ring path, malformed packets). It also runs the real daemon on `/dev/null` and talks to it
through the socket.

## Safety notes

- A call with `-r 1` was verified on the phone (2026-10-28, earpiece and microphones). `-r 0`
  with the Voice Call UCM verb is not yet: start with low volumes (`Handset Volume` 0 in the verb,
  volume index ≤ 4) and do not hold the earpiece to the ear on the first tries.
- Everything the modem sends is untrusted: lengths, sync words, data sizes, fragment fields and
  ring indices are checked; bad packets are logged and dropped. The ring is read with wrap-around
  inside the md_data region only.
- The daemon writes only to `/dev/ccci_aud`, the raw-audio shared memory (format once per boot,
  index reset after speech off, read index), ALSA controls of the configured card (with `-r 0`
  only the six PCM 1 interconnect switches), and its files in `/run`. No partition or file of the
  modem is touched.
- `/dev/usip` is never opened: speech on always says "no parameters, use the modem's built-in
  defaults" (parameter status 0). Passing captured parameter blobs is a possible future feature.
- One opener per CCCI port: a second daemon (or the factory audio HAL in a container) cannot share
  `/dev/ccci_aud`.
- On SIGTERM an active call is ended (`call-stop`) before the daemon exits.

## Choices where the protocol description leaves room

- **Codec ownership** (`-r`): the factory audio HAL sets everything itself; here the codec routes of a
  call are a UCM verb so that PipeWire/WirePlumber, which run the HiFi devices' DisableSequences
  on every device change, cannot kill the earpiece or the microphones of a call they do not know
  about, and so that a device change in a call is one profile change for everyone. The modem
  path stays in the daemon (see "What call-start sets").
- **Readiness**: see above (state file, else alive, else `-a`).
- **Call-stop order**: the protocol description order: UL mute, PCM closed, paths off, then
  0x2F21, then UL mute restored. The kernel contract (recon) suggested speech off first;
  the protocol description, taken from the factory audio HAL, wins.
- **Speech-on fields**: application 0, BT info 0, AP-DSP 0, parameter path 0, status 0, size and
  index 0, external device info 0 for **both** earpiece and speaker (no smart-PA echo reference:
  our kernel has no hostless I2SIN4 → PCM_1_PB_CH4/5 path yet), enhancement control 0x0003,
  common parameters = the factory "Common" values (`-C` for zeros), debug info 0, smart-PA byte 1
  (`-A`).
- **Volume**: the receiver table (DL attenuation 22,19,16,13,10,7,4 dB, UL 23 dB) is used for
  the speaker too. Default index 4. 0x2F00/0x2F01 are sent after every speech on; 0x2F00 again
  only when the value changes; 0x2F01 never changes. 0x2F7B is not sent on a volume change
  (without a parameter blob it would carry nothing new).
- **Device switch**: DL mute, UL mute (each acked), output switch, 0x2F2B with the mute bits set
  as they are at that moment (0x03), DL unmute, UL back to the user setting (question 11).
- **Queue**: A0 messages (gains) go through the same FIFO as A1 messages, behind an outstanding
  A1. Answers (A2) bypass it. A missing ack therefore holds later messages for up to 60 s, as in
  the protocol description; `-T` shortens it.
- **Refresh after 0xAFA0** (question 10): Voice Call PCM reopened, speech on with the current
  state, DL and UL gain, and 0x2F02 = 1 if the microphone is muted. On 0xAFA0 the modem-side state
  bits are cleared (its speech task restarted).
- **Recovery after a daemon crash** (question 8): the pending ack id, the modem-side state
  bits and whether a call was up are kept in `/run/t2-speechd.state`. At start the daemon waits up
  to 1 s for a pending ack, then sends the "off" message for every bit still set (speech off when
  a call was up) and switches the old AP path off. `/run` is empty after boot, so no unpaired
  0x2F21 is sent at a normal start.
- **Shared memory**: formatted once per boot (marker file in `/run` instead of the
  `Speech_SHM_Init` control), when the modem is first ready; ring data zeroed. If the modem
  reading flag is set at index reset, the reset is skipped and AP flag bit 1 cleared again.
- **0xAF90 with an unexpected type or size** is still answered with 0x2F90 (only logging depends
  on its content). 0xAFA0 of the wrong type, size or fragment is not answered.
- **0xAF75** is logged and shown in `status`; the gains do not depend on the band here, so nothing
  is resent.
- **Data messages 0xAF60/62/64/66** (features never enabled) are answered with 0x2F60/62/64/66,
  their ring record consumed; 0xAFB5 is answered 0x2FB5. 0xAF70 and 0xAFE4 are ignored.
- **Messages from the modem while it is lost or in EPOF** are ignored, except 0xAFA0.
- **call-start while the modem is not ready**: after the 480 ms wait the call is set up anyway
; speech on is dropped with a local ack and the reply warns. The status monitor then
  treats the modem as lost and redoes speech on once it is back.
- **call-stop without a call** still runs the full sequence and switches both outputs
  off.
- **Answers to modem requests** (0x2FA0, 0x2F78, 0x2F90…) are written without the state-file
  check (the modem just talked to us); the kernel refuses them if the modem is not running.

## Open questions

The ones that matter first for this daemon: 1 (a real message trace), 3 (do the built-in default
parameters give audio), 5 (PCM rate: 48 kHz assumed), 6 (exact earpiece route, mic path,
sidetone), 7 (band encoding), 8 (unpaired 0x2F21), 10 (refresh content), 11 (mutes around
0x2F2B), 13 (split of modem vs codec gain).
