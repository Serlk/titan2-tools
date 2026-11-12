# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com>
# Usage: python3 -I tools/ucm/tools/ucmcheck.py tools/ucm/Titan2
#!/usr/bin/python3 -I
# Check every cset in the Titan2 UCM files: control name exists in our kernel
# drivers (mt6369 codec, mt6878-titan2 machine driver, mt6878 AFE interconnect),
# enum values are valid, numeric values in range, ConflictingDevice names exist,
# every SectionUseCase file exists, and the HiFi/Voice Call verbs put back what
# they set (DisableSequence vs EnableSequence of each device/verb).
import re, sys, os

ROOT = sys.argv[1]
ENUMS = {
    "HPL Mux": ["Open", "Audio Playback"], "HPR Mux": ["Open", "Audio Playback"],
    "RCV Mux": ["Open", "Mute", "Voice Playback"], "LOL Mux": ["Open", "Playback"],
    "UL_SRC_MUX": ["AMIC", "DMIC"],
    "MISO0_MUX": ["UL1_CH1", "UL1_CH2", "UL2_CH1", "UL2_CH2"],
    "MISO1_MUX": ["UL1_CH1", "UL1_CH2", "UL2_CH1", "UL2_CH2"],
    "ADC_L_Mux": ["Idle", "AIN0", "Left Preamplifier", "Idle_1"],
    "ADC_R_Mux": ["Idle", "AIN0", "Right Preamplifier", "Idle_1"],
    "PGA_L_Mux": ["None", "AIN0", "AIN1", "AIN2"],
    "PGA_R_Mux": ["None", "AIN0", "AIN1", "AIN2"],
}
INT = {  # name: (channels, max)
    "Headset Volume": (2, 18), "Lineout Volume": (1, 18), "Handset Volume": (1, 18),
    "PGA1 Volume": (1, 4), "PGA2 Volume": (1, 4), "Speaker Volume": (1, 1023),
}
SWITCH = {"Speaker Switch", "Earpiece Switch", "Headphones Switch",
          "Main Mic Switch", "Second Mic Switch", "Headset Mic Switch"}
# AFE interconnect "<mixer widget> <input>" from mt6878-dai-adda.c / -i2s.c / -afe-pcm.c / -dai-pcm.c
MIX = set()
for w, ins in {
    "ADDA_DL_CH1": ["DL0_CH1", "DL1_CH1", "DL2_CH1", "DL3_CH1", "DL4_CH1", "DL5_CH1", "DL6_CH1",
                    "DL7_CH1", "DL8_CH1", "DL_24CH_CH1", "DL24_CH1", "PCM_1_CAP_CH1"],
    "ADDA_DL_CH2": ["DL0_CH1", "DL0_CH2", "DL1_CH2", "DL2_CH2", "DL3_CH2", "DL4_CH2", "DL5_CH2",
                    "DL6_CH2", "DL7_CH2", "DL8_CH2", "DL_24CH_CH2", "DL24_CH2", "PCM_1_CAP_CH1",
                    "PCM_1_CAP_CH2"],
    "I2SOUT4_CH1": ["DL0_CH1", "DL1_CH1", "DL2_CH1", "DL3_CH1", "DL4_CH1", "DL5_CH1", "DL6_CH1",
                    "DL7_CH1", "DL8_CH1", "DL_24CH_CH1", "DL24_CH1", "PCM_1_CAP_CH1"],
    "I2SOUT4_CH2": ["DL0_CH2", "DL1_CH2", "DL2_CH2", "DL3_CH2", "DL4_CH2", "DL5_CH2", "DL6_CH2",
                    "DL7_CH2", "DL8_CH2", "DL_24CH_CH2", "DL24_CH2", "PCM_1_CAP_CH1"],
    "UL0_CH1": ["ADDA_UL_CH1", "ADDA_UL_CH2", "I2SIN4_CH1", "I2SIN4_CH2"],
    "UL0_CH2": ["ADDA_UL_CH1", "ADDA_UL_CH2", "I2SIN4_CH1", "I2SIN4_CH2"],
    "PCM_1_PB_CH1": ["ADDA_UL_CH1", "ADDA_UL_CH2"],
    "PCM_1_PB_CH2": ["ADDA_UL_CH1", "ADDA_UL_CH2"],
}.items():
    for i in ins:
        MIX.add("%s %s" % (w, i))

CSET = re.compile(r"""cset\s+"name='([^']+)'\s+(.*)"\s*$""")
bad = 0
def err(msg):
    global bad
    bad += 1
    print("ERROR:", msg)

def check_cset(where, name, val):
    val = val.strip()
    if name in ENUMS:
        v = val.strip("'")
        if v not in ENUMS[name]:
            err("%s: '%s' = %r not in %r" % (where, name, v, ENUMS[name]))
    elif name in INT:
        ch, mx = INT[name]
        parts = val.split(",")
        if len(parts) != ch or not all(p.isdigit() and int(p) <= mx for p in parts):
            err("%s: '%s' = %r (want %d value(s) 0..%d)" % (where, name, val, ch, mx))
    elif name in SWITCH:
        if val not in ("on", "off", "1", "0"):
            err("%s: '%s' = %r" % (where, name, val))
    elif name in MIX:
        if val not in ("0", "1"):
            err("%s: '%s' = %r" % (where, name, val))
    else:
        err("%s: unknown control '%s'" % (where, name))

def parse(path):
    """returns {section: {seq: [(name, val)]}}, sections 'Verb', 'Device.X'"""
    out, sec, seq, files, conflicts = {}, None, None, [], {}
    for n, line in enumerate(open(path), 1):
        s = line.split("#", 1)[0].strip()
        if not s:
            continue
        m = re.match(r'SectionUseCase\."([^"]+)"', s)
        if m:
            sec = "UseCase." + m.group(1); out.setdefault(sec, {}); continue
        m = re.match(r'File\s+"([^"]+)"', s)
        if m and sec and sec.startswith("UseCase."):
            files.append(m.group(1)); continue
        if s.startswith("SectionVerb"):
            sec = "Verb"; out.setdefault(sec, {}); continue
        m = re.match(r'SectionDevice\."([^"]+)"', s)
        if m:
            sec = "Device." + m.group(1); out.setdefault(sec, {}); conflicts[m.group(1)] = []; continue
        if s.startswith("BootSequence"):
            sec = "Boot"; out.setdefault(sec, {}); seq = "Boot"; out[sec].setdefault(seq, []); continue
        m = re.match(r'(EnableSequence|DisableSequence|TransitionSequence\."[^"]+")\s*\[', s)
        if m:
            seq = m.group(1); out[sec].setdefault(seq, []); continue
        if s.startswith("ConflictingDevice") or s.startswith("SupportedDevice"):
            seq = "Conflicting"; continue
        if s == "]":
            seq = None; continue
        if seq == "Conflicting":
            conflicts[sec.split(".", 1)[1]].append(s.strip('"')); continue
        m = CSET.match(s)
        if m:
            if seq is None or sec is None:
                err("%s:%d cset outside a sequence" % (path, n))
            else:
                out[sec][seq].append((m.group(1), m.group(2)))
                check_cset("%s:%d" % (os.path.basename(path), n), m.group(1), m.group(2))
        elif s.startswith("cset"):
            err("%s:%d unparsed cset: %s" % (path, n, s))
    return out, files, conflicts

master, files, _ = parse(os.path.join(ROOT, "Titan2.conf"))
for f in files:
    p = os.path.join(ROOT, f.split("/Titan2/", 1)[1])
    if not os.path.exists(p):
        err("missing verb file %s" % p); continue
    secs, _, conflicts = parse(p)
    devs = set(conflicts)
    for d, cl in conflicts.items():
        for c in cl:
            if c not in devs:
                err("%s: device %s conflicts with unknown %s" % (f, d, c))
            elif d not in conflicts[c]:
                err("%s: %s conflicts with %s but not the reverse" % (f, d, c))
    # every switch set to 1/on in an EnableSequence is set 0/off/Open in the DisableSequence
    for sec, seqs in secs.items():
        en = {n: v for n, v in seqs.get("EnableSequence", [])}
        dis = {n: v for n, v in seqs.get("DisableSequence", [])}
        for n, v in en.items():
            if (n in MIX and v == "1") or (n in SWITCH and v == "on") or \
               (n in ("RCV Mux", "HPL Mux", "HPR Mux") and v != "'Open'"):
                if n not in dis:
                    err("%s %s: '%s' enabled but not disabled" % (f, sec, n))
        for n in dis:
            if n not in en:
                print("note: %s %s: '%s' in DisableSequence only" % (f, sec, n))
    print("%s: %d sections ok" % (f, len(secs)))
print("%d errors" % bad)
sys.exit(1 if bad else 0)
