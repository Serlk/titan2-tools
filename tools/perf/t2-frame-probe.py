#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
"""Titan 2: a GTK4 window that animates for N seconds and reports its frame pacing.

The window draws a moving bar through GSK (no Cairo, no shaders of its own) on a
translucent (default) or opaque background, so the compositor composites it the way it
composites a real app. Every GTK frame-clock tick is logged; completed GdkFrameTimings
give the presentation times the compositor reported (wp_presentation). The summary is
what the measurement plan compares between renderers, Mesa versions and effects:

  fps              ticks per second over the run (first 2 s dropped)
  tick_p50/p95_ms  intervals between ticks (16.7 = every TE, 33.5 = every other one)
  long_ticks       intervals > 1.5 refresh periods: frames the user sees as a stutter
  present_p50_ms   intervals between consecutive presentation timestamps
  present_jumps    presentation intervals > 1.5 refresh periods
  first_frame_ms   process start -> first tick (app launch latency of a GTK app)

  t2-frame-probe [SECONDS] [--opaque] [--fullscreen] [--launch] [--json FILE]

--launch quits after the first frame and prints first_frame_ms only (repeat it for a
launch-latency sample). The renderer is GTK's choice unless GSK_RENDERER is set
(gl = panfrost, vulkan = panvk); the probe prints which one GTK used.
"""
import json
import os
import sys
import time

PROC_START = time.monotonic()
try:
    with open('/proc/self/stat') as f:
        # field 22: start time in clock ticks since boot
        _start_ticks = int(f.read().split(')')[1].split()[19])
    _hz = os.sysconf('SC_CLK_TCK')
    with open('/proc/uptime') as f:
        _uptime = float(f.read().split()[0])
    # how long ago the process started (uptime is CLOCK_BOOTTIME-like; fine for ms)
    PROC_AGE_AT_IMPORT = _uptime - _start_ticks / _hz
except (OSError, ValueError, IndexError):
    PROC_AGE_AT_IMPORT = 0.0

import gi  # noqa: E402
gi.require_version('Gtk', '4.0')
gi.require_version('Graphene', '1.0')
from gi.repository import Gtk, Gdk, Graphene, GLib  # noqa: E402

args = sys.argv[1:]
duration = 30.0
opaque = '--opaque' in args
fullscreen = '--fullscreen' in args
launch = '--launch' in args
json_path = None
for i, a in enumerate(args):
    if a == '--json' and i + 1 < len(args):
        json_path = args[i + 1]
    elif a[:1].isdigit():
        duration = float(a)
if launch:
    duration = 5.0

frames = []          # [t_since_start, frame_time_us, counter]
presentations = {}   # counter -> [frame_time, presentation_time, refresh_interval]
first_frame_ms = None
renderer_name = None


class Probe(Gtk.Widget):
    def __init__(self):
        super().__init__()
        self.started = time.monotonic()
        self.add_tick_callback(self.tick)

    def tick(self, widget, clock):
        global first_frame_ms, renderer_name
        now = time.monotonic()
        if first_frame_ms is None:
            first_frame_ms = (now - PROC_START + PROC_AGE_AT_IMPORT) * 1000
            r = self.get_native().get_renderer() if self.get_native() else None
            renderer_name = type(r).__name__ if r else None
            if launch:
                GLib.idle_add(lambda: (app.quit(), False)[1])
        counter = clock.get_frame_counter()
        frames.append([round(now - self.started, 6), clock.get_frame_time(), counter])
        for index in range(max(0, counter - 8), counter):
            if index in presentations:
                continue
            timing = clock.get_timings(index)
            if timing and timing.get_complete():
                presentations[index] = [timing.get_frame_time(),
                                        timing.get_presentation_time(),
                                        timing.get_refresh_interval()]
        self.queue_draw()
        return True

    def do_snapshot(self, snapshot):
        width, height = self.get_width(), self.get_height()
        bg, bar = Gdk.RGBA(), Gdk.RGBA()
        bg.parse('rgb(33,50,71)' if opaque else 'rgba(33,50,71,0.80)')
        bar.parse('#82c7a5')
        rect = Graphene.Rect()
        rect.init(0, 0, width, height)
        snapshot.append_color(bg, rect)
        x = (time.monotonic() - self.started) * 150 % max(1, width)
        rect.init(x, 0, min(40, width - x), height)
        snapshot.append_color(bar, rect)


def percentile(values, p):
    if not values:
        return None
    s = sorted(values)
    k = min(len(s) - 1, int(round((len(s) - 1) * p)))
    return s[k]


def summary():
    refresh_us = None
    for v in presentations.values():
        if v[2]:
            refresh_us = v[2]
            break
    refresh_ms = (refresh_us or 16666) / 1000
    ticks = [f for f in frames if f[0] >= 2.0] if not launch else frames
    intervals = [(b[1] - a[1]) / 1000 for a, b in zip(ticks, ticks[1:]) if b[1] > a[1]]
    span = (ticks[-1][0] - ticks[0][0]) if len(ticks) > 1 else 0
    pres = sorted(v[1] for v in presentations.values() if v[1])
    pres_iv = [(b - a) / 1000 for a, b in zip(pres, pres[1:]) if b > a]
    return dict(
        renderer=renderer_name, opaque=opaque, fullscreen=fullscreen,
        duration_s=round(duration, 1), refresh_ms=round(refresh_ms, 3),
        first_frame_ms=round(first_frame_ms, 1) if first_frame_ms else None,
        fps=round(len(intervals) / span, 2) if span else None,
        tick_p50_ms=round(percentile(intervals, 0.5), 2) if intervals else None,
        tick_p95_ms=round(percentile(intervals, 0.95), 2) if intervals else None,
        tick_max_ms=round(max(intervals), 2) if intervals else None,
        long_ticks=sum(1 for i in intervals if i > 1.5 * refresh_ms),
        ticks=len(intervals),
        present_p50_ms=round(percentile(pres_iv, 0.5), 2) if pres_iv else None,
        present_p95_ms=round(percentile(pres_iv, 0.95), 2) if pres_iv else None,
        present_jumps=sum(1 for i in pres_iv if i > 1.5 * refresh_ms),
        presentations=len(pres_iv),
    )


app = Gtk.Application(application_id='org.titan2.FrameProbe')


def activate(application):
    window = Gtk.ApplicationWindow(application=application, title='t2-frame-probe')
    window.set_decorated(False)
    window.add_css_class('frame-probe')
    css = Gtk.CssProvider()
    css.load_from_data(b'.frame-probe { background: transparent; }')
    Gtk.StyleContext.add_provider_for_display(Gdk.Display.get_default(), css, 800)
    window.set_default_size(480, 440)
    window.set_child(Probe())
    if fullscreen:
        window.fullscreen()
    window.present()
    application.inhibit(window, Gtk.ApplicationInhibitFlags.IDLE, 'frame timing probe')
    GLib.timeout_add(int(duration * 1000), lambda: (application.quit(), False)[1])


app.connect('activate', activate)
app.run([])
result = summary()
if json_path:
    with open(json_path, 'w') as f:
        json.dump(dict(summary=result, frames=frames, presentations=presentations), f)
if launch:
    print(json.dumps(dict(first_frame_ms=result['first_frame_ms'], renderer=result['renderer'])))
else:
    print(json.dumps(result))
