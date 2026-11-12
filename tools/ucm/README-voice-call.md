<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
<!-- Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com> -->
# Voice-call audio and PipeWire on the Titan 2

How the call path (modem ⇄ AFE PCM 1 ⇄ MT6369 codec / FS199x amplifier, outside PipeWire) and
PipeWire's media path share one sound card without fighting over the codec. Written 2026-10-29
without the phone; the install steps below are for the next session. Files:

| file | what |
|---|---|
| `Titan2/Titan2.conf` | UCM master: verbs `HiFi` and `Voice Call`, BootSequence (everything off) |
| `Titan2/HiFi.conf` | media verb (PipeWire); reproduces the 2026-10-28 phone edits, **diff against the phone copy first** |
| `Titan2/VoiceCall.conf` | new call verb: codec routes/gains of a call + PCM 1 switches, devices Earpiece/Speaker/Headphones, Mic/Headset |
| `tools/ucmcheck.py`, `tools/ucmparse.c` | host-side checks of the UCM files |
| `../netroot/install.d/t2-ucm.sh`, `t2-speechd.sh` | put the UCM files / t2-speechd service into a rebuilt netroot image |
| `../speechd/` | `t2-speechd`: modem speech on/off, holds `hw:Titan2,4`, PCM 1 switches; `codec_routes = 0\|1` |

## 1. The problem

The call path is modem ⇄ PCM 1 ⇄ codec, directly; PipeWire only sees `hw:Titan2,0/1/2` (the HiFi
verb). Until now `t2-speechd` switched the codec itself (`RCV Mux`, `PGA_L/R_Mux`, `ADC_L/R_Mux`,
`Speaker Switch`, `Speaker Volume`, …) — the same controls the HiFi devices set and, in their
DisableSequence, reset (`RCV Mux` → `Open`, `ADC_*_Mux` → `Idle`, `PGA_*_Mux` → `None`). Any
device change by PipeWire/WirePlumber in a call (headset, user choice, WirePlumber restart) ran
those DisableSequences and killed the earpiece or the microphones of the call.

## 2. Design: who owns which control

| control group | owner | where |
|---|---|---|
| Codec: `RCV Mux`, `HPL/HPR Mux`, `Handset/Headset Volume`, `Speaker Volume`, pin switches `Earpiece/Speaker/Headphones/Main Mic/Second Mic/Headset Mic Switch`, `PGA_L/R_Mux`, `ADC_L/R_Mux`, `PGA1/PGA2 Volume`, `UL_SRC_MUX`, `MISO0/1_MUX` | **UCM verb**, i.e. PipeWire/WirePlumber. HiFi values in `HiFi.conf`, call values in `VoiceCall.conf` | the active card profile decides |
| Media routes `ADDA_DL_CH1/2 DL0_CH1/2`, `I2SOUT4_CH1/2 DL1_CH1/2`, `UL0_CH1/2 ADDA_UL_CH1/2` | UCM, verb level in **both** verbs (on while the verb is active, off in its DisableSequence) | so PipeWire can open PCM 0/1/2 at any time; DPCM returns EINVAL ("no backend DAIs enabled") for a front end without an enabled back end |
| Modem path `PCM_1_PB_CH1 ADDA_UL_CH1`, `PCM_1_PB_CH2 ADDA_UL_CH2` (mics → modem), `ADDA_DL_CH1/2 PCM_1_CAP_CH1` (modem → earpiece/headset), `I2SOUT4_CH1/2 PCM_1_CAP_CH1` (modem → speaker) | **both**: `t2-speechd` (always) and the Voice Call verb (uplink at verb level, downlink per output device). Same values, idempotent | t2-speechd needs them to open `hw:Titan2,4`; the verb needs them so that an output change in PipeWire moves the modem downlink too |
| `hw:Titan2,4` (Hostless Speech PCM, both directions), `/dev/ccci_aud` speech on/off, modem gains/mutes/routing messages | `t2-speechd` only | not a PipeWire PCM, not in any UCM device |
| `hw:Titan2,0/1/2` | PipeWire, in both verbs | in Voice Call the sink/source exist so the profile has Earpiece/Speaker/Headphones and Mic/Headset ports (callaudiod and WirePlumber match on them); media played in a call is summed into the codec path by the AFE interconnect, like notification sounds in a call on factory |

Values in `VoiceCall.conf` are the ones a working call used (t2-speechd `-r 1`, 2026-10-28):
`RCV Mux` = `Voice Playback`, `Handset Volume` 0 (−10 dB), `Speaker Volume` 512/1023,
`PGA1/PGA2 Volume` 2 (+12 dB), `PGA_L_Mux` `AIN0` + `ADC_L_Mux` `Left Preamplifier` (main mic →
modem CH1), `PGA_R_Mux` `AIN2` + `ADC_R_Mux` `Right Preamplifier` (second mic → CH2),
`UL_SRC_MUX` `AMIC`, `MISO0_MUX` `UL1_CH1`, `MISO1_MUX` `UL1_CH2`. Headset mic: `AIN1` on the left
ADC and `MISO1_MUX` = `UL1_CH1` so the modem gets it on both channels (factory single-mic path).
Headphones: `HPL/HPR Mux` = `Audio Playback`, `Headset Volume` 0,0, modem downlink into ADDA like
the earpiece. All control names were checked against `mt6369.c`, `mt6878-titan2.c` and the AFE
interconnect tables (`mt6878-dai-adda.c`, `-dai-i2s.c`, `-afe-pcm.c`, `-dai-pcm.c`).

### What PipeWire makes of it

PipeWire 1.6 (`spa/plugins/alsa/acp/alsa-ucm.c`) creates **one sink or source per UCM device**,
one port per sink, and **one card profile per set of devices usable at the same time** — two
devices with the same PCM never share a profile. So the card has profiles

    HiFi (Earpiece, Mic)   HiFi (Mic, Speaker)   HiFi (Headphones, Mic)   (+ 3 with Headset)
    Voice Call (Earpiece, Mic)   Voice Call (Mic, Speaker)   Voice Call (Headphones, Mic)   (+ 3 with Headset)

and **changing the output is a profile change**, not a sink-port change. Profile priority =
verb (HiFi 8000, Voice Call 4000) + device priorities; in Voice Call Earpiece (100) > Speaker
(50) > Headphones (40, no USB-C jack detection yet) and Mic (100) > Headset (50), so
`Voice Call (Earpiece, Mic)` is the call default. A profile change runs the DisableSequence of
the old device(s) and the EnableSequence of the new, and a verb change the verb sequences in
between (`alsa-lib` `set_verb_user`: disable devices, verb DisableSequence, new verb
EnableSequence, device EnableSequence). UCM profiles are not probed by opening their PCMs
(`ucm_create_profile` marks them supported), so `hw:Titan2,4` being held by t2-speechd is no
problem for PipeWire.

### Who switches the profile

- **WirePlumber ≥ 0.5.11** (Alpine 3.24 ships 0.5.15): `libwireplumber-module-modem-manager`
  watches ModemManager (`org.freedesktop.ModemManager1.Modem.Voice`, call states
  dialing/ringing-out/active) on the **system bus** and `device/find-voice-call-profile.lua`
  then selects the highest-priority available profile whose name starts with `Voice Call`
  (profiles with availability `unknown`, ours, are accepted since 0.5.15). On call end it goes
  back to the stored/best profile (HiFi). The module requires `support.system-dbus`
  (`libwireplumber-module-dbus-connection` in the WirePlumber config). Our MM bridge
  (`tools/mm-titan2`) reports the call on D-Bus, so this works without any dialer code.
- **By hand**: `pactl set-card-profile <card> "Voice Call (Earpiece, Mic)"` (and `"HiFi (…)"`
  back). In a call, a manual profile change sticks until the call ends (the hook runs on
  call start/stop and device add only).
- **callaudiod** `SelectMode(1)`: does nothing on this stack — callaudiod disables its own routing
  when it finds WirePlumber > 0.5.10 (`init_server_info` → `cad_manager_disable_routing`),
  because WirePlumber does it.

Speaker in a call = `pactl set-card-profile <card> "Voice Call (Mic, Speaker)"`; callaudiod's
`EnableSpeaker` cannot do it here (it switches a *port* of one sink, and the Speaker is a
different sink/profile on PipeWire). A GNOME Calls speaker button therefore needs a small helper
(a `wpctl`/`pactl` profile switch, or a WirePlumber script) — open item.

### t2-speechd: `codec_routes`

`-r 0` (default, `/etc/conf.d/t2-speechd`: `T2_SPEECHD_CODEC_ROUTES=0`): the daemon writes only
the six PCM 1 switches above, opens `hw:Titan2,4` and talks to the modem. `-r 1`: it also sets
the codec (the old behaviour) — the fallback when the Voice Call profile is not active during a
call. The socket protocol (`call-start`, `call-stop`, `device`, `volume`, `mute`, `status`) is
unchanged; `status` shows `codec_routes=`. With `-r 0`, `device earpiece|speaker` only moves the
downlink switches; the output itself is chosen by the card profile, so the two should agree
(the MM bridge only sends `call-start --rate 48000`/`call-stop`, i.e. earpiece downlink).

## 3. callaudiod on Alpine 3.24

Alpine 3.24 **ships it**: `community/callaudiod 0.1.99-r0` (aarch64), with `callaudiocli`,
`callaudiod-libs`, `callaudiod-dev` ([pkgs.alpinelinux.org](https://pkgs.alpinelinux.org/packages?name=callaudio*&branch=v3.24&arch=aarch64)).
Upstream (gitlab.com/mobian1/callaudiod, cloned in `src/callaudiod`) calls it **deprecated**:
profile switching is WirePlumber's job now; mute is plain PulseAudio API; speaker switching
"may come to PipeWire".

    apk add callaudiod callaudiocli

callaudiod is a **session-bus** D-Bus service (`org.mobian_project.CallAudio`, activated by
`/usr/share/dbus-1/services/org.mobian_project.CallAudio.service`, or started as a user service /
by hand with `callaudiod`), linked to `libpulse` → it talks to `pipewire-pulse` of the same user
session. Needs on the phone: a D-Bus **session** bus for the user that runs PipeWire (our niri
session: check `echo $DBUS_SESSION_BUS_ADDRESS`; if there is none, `dbus-run-session` around the
session or `dbus-daemon --session --address=unix:path=$XDG_RUNTIME_DIR/bus` with
`DBUS_SESSION_BUS_ADDRESS=unix:path=$XDG_RUNTIME_DIR/bus` exported), `pipewire-pulse` running, and
`PULSE_SERVER`/`XDG_RUNTIME_DIR` so libpulse finds it. At start it looks for a card that has ports
of type speaker **and** earpiece/handset/headphones (`alsa.card_name` not starting with
"Modem"), and a profile starting with `Voice Call` — our card qualifies once the UCM profile is installed.

D-Bus API (`org.mobian_project.CallAudio` on `/org/mobian_project/CallAudio`):

| method / property | |
|---|---|
| `SelectMode(u mode) → b` | 0 default, 1 voice call. **No-op with WirePlumber > 0.5.10** (returns true) |
| `EnableSpeaker(b) → b` | sink port Speaker / highest-priority non-speaker port. Does not work with per-device sinks (see above) |
| `MuteMic(b) → b` | mutes the default source (works) |
| `AudioMode`, `SpeakerState`, `MicState` (u) | 0/1, 255 unknown |

GNOME Calls uses libcallaudio: `call_audio_select_mode(CALL_AUDIO_MODE_CALL)` when a call starts,
`…_DEFAULT` at the end, `call_audio_enable_speaker()` for its speaker button, `call_audio_mute_mic()`.
Our MM bridge does not talk to callaudiod at all — the profile comes from WirePlumber's
ModemManager module, the modem path from `t2-speechd` (triggered by the MM plugin / `t2-mm-speech-trigger`).

CLI test (`callaudiocli -h` for the options):

    callaudiocli -m 1          # SelectMode(call): returns true, no effect with WP 0.5.15
    callaudiocli -s 1          # EnableSpeaker(true)
    callaudiocli -s 0
    callaudiocli -u 1          # MuteMic(true)
    callaudiocli -u 0
    callaudiocli -m 0
    gdbus introspect --session -d org.mobian_project.CallAudio -o /org/mobian_project/CallAudio

If Alpine's package were missing: `meson setup build && ninja -C build` needs `alsa-lib-dev
glib-dev pulseaudio-dev appstream-dev` (`dependency('alsa')`, `gobject-2.0`, `gio-unix-2.0`,
`libpulse`, `libpulse-mainloop-glib`, `appstream`); not needed on 3.24.

## 4. Install (next session, phone on)

Before anything: **do not touch the phone until the user says it is test time**
(memory: phone-hands-off). Then, on the phone:

    # 0. what is there now
    cd /usr/share/alsa/ucm2/Titan2 && ls -la
    diff -u HiFi.conf /path/to/repo/tools/ucm/Titan2/HiFi.conf     # the hand edits of 2026-10-28:
                                                                   # (a) route switches at verb level, (b) Headphones 40
    # keep the phone's version of anything the diff shows that this README does not describe
    cp HiFi.conf HiFi.conf.bak-2026-10-29
    wireplumber --version; pipewire --version; cat /proc/asound/cards

    # 1. UCM
    cp tools/ucm/Titan2/{Titan2.conf,HiFi.conf,VoiceCall.conf} /usr/share/alsa/ucm2/Titan2/
    ls -l /usr/share/alsa/ucm2/conf.d/mt6878-titan2/Titan2.conf   # -> ../../Titan2/Titan2.conf
    alsaucm listcards; alsaucm -c Titan2 list _verbs                # HiFi, Voice Call
    alsaucm -c Titan2 list "_devices/Voice Call"

    # 2. PipeWire picks it up on a WirePlumber restart (as the session user)
    rc-service wireplumber restart   # or: systemctl --user restart wireplumber / kill + respawn
    pactl list cards | grep -A40 'Name: alsa_card'    # profiles "Voice Call (Earpiece, Mic)" ...,
                                                      # Ports: [Out] Earpiece/Speaker/Headphones, [In] Mic/Headset
    wpctl status

    # 3. manual profile test, no call: the codec must follow
    CARD=$(pactl list cards short | awk '/Titan2|platform-sound/{print $2}')
    pactl set-card-profile $CARD "Voice Call (Earpiece, Mic)"
    amixer -c Titan2 cget name='RCV Mux'                      # Voice Playback
    amixer -c Titan2 cget name='PCM_1_PB_CH1 ADDA_UL_CH1'     # 1
    amixer -c Titan2 cget name='ADDA_DL_CH1 PCM_1_CAP_CH1'    # 1
    pactl set-card-profile $CARD "Voice Call (Mic, Speaker)"
    amixer -c Titan2 cget name='I2SOUT4_CH1 PCM_1_CAP_CH1'    # 1; RCV Mux back to Open
    pactl set-card-profile $CARD "HiFi (Mic, Speaker)"        # all PCM_1 switches 0, RCV Mux Open

    # 4. t2-speechd (codec_routes=0)
    install -m 755 tools/speechd/t2-speechd-aarch64 /usr/local/bin/t2-speechd
    install -m 644 tools/speechd/t2-speechd.conf /etc/conf.d/t2-speechd   # T2_SPEECHD_CODEC_ROUTES=0
    install -m 755 tools/speechd/t2-speechd.initd /etc/init.d/t2-speechd
    rc-service t2-speechd restart; t2-speechd status                     # codec_routes=0

    # 5. a call, profile by hand first (WirePlumber's own switch is step 6)
    pactl set-card-profile $CARD "Voice Call (Earpiece, Mic)"
    mmcli -m any --voice-create-call "number=<n>" && mmcli -m any -o 0 --start
    #   the MM plugin sends t2-speechd "call-start --rate 48000"; check: t2-speechd status,
    #   amixer: RCV Mux = Voice Playback, PCM_1 switches 1, speech-on acked in the log
    pactl set-card-profile $CARD "Voice Call (Mic, Speaker)"   # speaker in the call
    pactl set-card-profile $CARD "Voice Call (Earpiece, Mic)"
    mmcli -m any -o 0 --hangup
    pactl set-card-profile $CARD "HiFi (Mic, Speaker)"

    # 6. WirePlumber does the switch (needs its modem-manager module and the system bus)
    grep -rn "modem-manager\|find-voice-call" /usr/share/wireplumber/wireplumber.conf \
        /usr/share/wireplumber/wireplumber.conf.d/ /etc/wireplumber/ 2>/dev/null
    ls /usr/lib/wireplumber-0.5/libwireplumber-module-modem-manager.so
    pactl set-card-profile $CARD "HiFi (Mic, Speaker)"
    mmcli -m any --voice-create-call "number=<n>" && mmcli -m any -o 0 --start
    pactl list cards | grep "Active Profile"    # -> Voice Call (Earpiece, Mic) during the call, HiFi after
    WIREPLUMBER_DEBUG=3 (or journal/log of wireplumber): "Found calling profile 'Voice Call ...'"

    # 7. callaudiod (optional on this stack)
    apk add callaudiod callaudiocli && callaudiod &    # in the user session with a session bus
    callaudiocli -m 1; callaudiocli -u 1; callaudiocli -u 0; callaudiocli -m 0

Expected in `pactl list cards`: `alsa.card_name = "Titan2"`, profiles `HiFi (…)` ×6 and
`Voice Call (…)` ×6 plus `pro-audio` and `off`; ports `[Out] Earpiece`, `[Out] Speaker`,
`[Out] Headphones`, `[In] Mic`, `[In] Headset` with types earpiece/speaker/headphones/mic/headset.

## 5. Rollback

- UCM: `cp /usr/share/alsa/ucm2/Titan2/HiFi.conf.bak-titan /usr/share/alsa/ucm2/Titan2/HiFi.conf`
  (the pre-2026-10-28 file; or the `HiFi.conf.bak-2026-10-29` copy from step 0 for the
  hand-edited one), remove the `SectionUseCase."Voice Call"` block from `Titan2.conf` (or restore
  the repo copy from before this change: `git show HEAD:tools/ucm/Titan2/Titan2.conf`), delete
  `VoiceCall.conf`, restart WirePlumber.
- t2-speechd: `T2_SPEECHD_CODEC_ROUTES=1` in `/etc/conf.d/t2-speechd`, `rc-service t2-speechd
  restart` → the daemon sets the codec itself again (the verified 2026-10-28 behaviour), with the
  known conflict with PipeWire device changes. The old binary is not kept in the repo (the
  socket protocol is unchanged; `-r 1` *is* the old behaviour).

## 6. Validation done on the host (no phone)

- `alsaucm`/`aplay` are not installed on the host; `snd-dummy` needs root. Instead the UCM tree was
  parsed with the host's alsa-lib 1.2.16 (`snd_use_case_mgr_open("Titan2")` on a temporary
  `ALSA_CONFIG_UCM2` tree, no card — `${CardId}` replaced by a literal for the parse; without a
  card alsa-lib fails on `${CardId}` with "variable not defined", which is expected): both verbs,
  all 10 devices and their Values parse, `Voice Call` lists Earpiece/Speaker/Headphones/Mic/Headset.
- `tools/ucmcheck.py` (`python3 -I tools/ucm/tools/ucmcheck.py tools/ucm/Titan2`; the parse
  program is `tools/ucmparse.c`) verified every `cset`: control name exists in our drivers, enum values are the
  driver's strings, numeric ranges (`Handset/Headset Volume` 0..18, `PGA` 0..4, `Speaker Volume`
  0..1023), `ConflictingDevice` names exist and are symmetric, and everything switched on in an
  EnableSequence is switched off in the DisableSequence of the same device/verb.
- t2-speechd: `make test` (ASan/UBSan replay tests, 229 checks, including the new
  `test_codec_routes_off`) passes; host and aarch64 binaries built.

## 7. Open questions for the next session

1. The diff between the phone's `HiFi.conf` and the repo copy (step 0): the Earpiece route switch
   and the Headset mic UL0 handling were reconstructed from the description, not copied.
2. Does the Voice Call profile appear with the expected names, and does WirePlumber's
   `find-voice-call-profile` fire (module built into Alpine's wireplumber? system bus reachable
   from the session that runs WirePlumber? our MM plugin exposes `Modem.Voice` calls with the
   states the module expects: dialing/ringing-out/active)?
3. Call audio with `codec_routes=0` + verb: same quality as `-r 1`? The verb sets the codec
   possibly *after* `call-start` (order of the two triggers) — audio should start when the
   profile lands; check there is no DAPM glitch when `hw:Titan2,4` is already running.
4. `Voice Call (Mic, Speaker)` in a call: downlink moves to I2SOUT4, amplifier on, PCM rate 48 kHz
   OK; t2-speechd's own `device speaker` is then redundant (modem routing message 0x2F2B is still
   only sent by `device`): decide whether the MM bridge should mirror the profile into `device`.
5. Media (notification) playback during a call via the Voice Call verb's `hw:Titan2,0`: works,
   or does the second front end on the running ADDA back end disturb the call?
6. Headset mic path (`MISO1_MUX` = `UL1_CH1`) and Headphones: untestable until the ASW5480 USB-C
   audio switch works on mainline.
7. callaudiod: install at all? Only `MuteMic` is useful here; GNOME Calls' speaker button needs a
   profile-switch helper instead of `EnableSpeaker`.
8. WirePlumber 0.5.11–0.5.14 skip profiles with availability `unknown`; Alpine 3.24 has 0.5.15
   (fixed), check the installed version.
