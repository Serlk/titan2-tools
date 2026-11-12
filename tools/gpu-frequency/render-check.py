#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
"""Headless GLES2 workload and runtime-PM cycles on the Titan 2 GPU.

Uses system EGL/GLES libraries through ctypes. No display takeover or package
installation. Fails if Mesa selects software rendering or readback is wrong.
"""
import ctypes as c
import json
import sys
import time
from pathlib import Path

egl = c.CDLL('libEGL.so.1')
gl = c.CDLL('libGLESv2.so.2')

def api(lib, name, result, *args):
    f = getattr(lib, name)
    f.restype, f.argtypes = result, args
    return f

ptr, integer, uint = c.c_void_p, c.c_int, c.c_uint
get_display = api(egl, 'eglGetPlatformDisplay', ptr, uint, ptr, ptr)
initialize = api(egl, 'eglInitialize', uint, ptr, ptr, ptr)
choose = api(egl, 'eglChooseConfig', uint, ptr, ptr, ptr, integer, ptr)
bind_api = api(egl, 'eglBindAPI', uint, uint)
context = api(egl, 'eglCreateContext', ptr, ptr, ptr, ptr, ptr)
surface = api(egl, 'eglCreatePbufferSurface', ptr, ptr, ptr, ptr)
make_current = api(egl, 'eglMakeCurrent', uint, ptr, ptr, ptr, ptr)
terminate = api(egl, 'eglTerminate', uint, ptr)
get_string = api(gl, 'glGetString', c.c_char_p, uint)
create_shader = api(gl, 'glCreateShader', uint, uint)
shader_source = api(gl, 'glShaderSource', None, uint, integer, ptr, ptr)
compile_shader = api(gl, 'glCompileShader', None, uint)
get_shader = api(gl, 'glGetShaderiv', None, uint, uint, ptr)
shader_log = api(gl, 'glGetShaderInfoLog', None, uint, integer, ptr, ptr)
create_program = api(gl, 'glCreateProgram', uint)
attach = api(gl, 'glAttachShader', None, uint, uint)
bind_attrib = api(gl, 'glBindAttribLocation', None, uint, uint, c.c_char_p)
link = api(gl, 'glLinkProgram', None, uint)
get_program = api(gl, 'glGetProgramiv', None, uint, uint, ptr)
use = api(gl, 'glUseProgram', None, uint)
viewport = api(gl, 'glViewport', None, integer, integer, integer, integer)
gen_buffers = api(gl, 'glGenBuffers', None, integer, ptr)
bind_buffer = api(gl, 'glBindBuffer', None, uint, uint)
buffer_data = api(gl, 'glBufferData', None, uint, c.c_ssize_t, ptr, uint)
attrib = api(gl, 'glVertexAttribPointer', None, uint, integer, uint, uint, integer, ptr)
enable_attrib = api(gl, 'glEnableVertexAttribArray', None, uint)
draw = api(gl, 'glDrawArrays', None, uint, integer, integer)
read_pixels = api(gl, 'glReadPixels', None, integer, integer, integer, integer, uint, uint, ptr)
get_error = api(gl, 'glGetError', uint)

def attrs(*values):
    return (integer * (len(values) + 1))(*values, 0x3038)

display = get_display(0x31dd, None, None)  # EGL_PLATFORM_SURFACELESS_MESA
if not display or not initialize(display, None, None):
    raise RuntimeError('Cannot initialize surfaceless EGL')
try:
    config, count = ptr(), integer()
    if not choose(display, attrs(0x3033, 1, 0x3040, 4, 0x3024, 8,
                                 0x3023, 8, 0x3022, 8, 0x3021, 8),
                  c.byref(config), 1, c.byref(count)) or count.value != 1:
        raise RuntimeError('No GLES2 pbuffer config')
    bind_api(0x30a0)
    ctx = context(display, config, None, attrs(0x3098, 2))
    surf = surface(display, config, attrs(0x3057, 1024, 0x3056, 1024))
    if not ctx or not surf or not make_current(display, surf, surf, ctx):
        raise RuntimeError('Cannot create GLES context/surface')
    renderer = get_string(0x1f01).decode()
    print(json.dumps(dict(renderer=renderer, version=get_string(0x1f02).decode())), flush=True)
    if 'Mali-G615' not in renderer:
        raise RuntimeError('Expected hardware Mali-G615 renderer')

    def shader(kind, source):
        s = create_shader(kind)
        text = c.c_char_p(source)
        shader_source(s, 1, c.byref(text), None)
        compile_shader(s)
        ok = integer()
        get_shader(s, 0x8b81, c.byref(ok))
        if not ok.value:
            log = c.create_string_buffer(4096)
            shader_log(s, len(log), None, log)
            raise RuntimeError(log.value.decode())
        return s

    program = create_program()
    attach(program, shader(0x8b31, b'attribute vec2 pos; void main() { gl_Position=vec4(pos,0.,1.); }'))
    attach(program, shader(0x8b30, b'''
        precision highp float;
        void main() {
            vec2 p = gl_FragCoord.xy / 1024.;
            float x = p.x * .123 + p.y * .456;
            for (int i=0; i<64; i++) x = fract(x * 1.731 + .173);
            gl_FragColor = vec4(x, p.x, p.y, 1.);
        }
    '''))
    bind_attrib(program, 0, b'pos')
    link(program)
    ok = integer()
    get_program(program, 0x8b82, c.byref(ok))
    if not ok.value:
        raise RuntimeError('Shader program link failed')
    use(program)
    viewport(0, 0, 1024, 1024)
    vertices = (c.c_float * 6)(-1, -1, 3, -1, -1, 3)
    buffer = uint()
    gen_buffers(1, c.byref(buffer))
    bind_buffer(0x8892, buffer)
    buffer_data(0x8892, c.sizeof(vertices), vertices, 0x88e4)
    attrib(0, 2, 0x1406, 0, 0, None)
    enable_attrib(0)
    pixel = (c.c_ubyte * 4)()

    def frame():
        draw(4, 0, 3)
        read_pixels(512, 512, 1, 1, 0x1908, 0x1401, pixel)
        if get_error() or not (127 <= pixel[1] <= 129 and
                               127 <= pixel[2] <= 129 and pixel[3] == 255):
            raise RuntimeError('GPU readback failed: ' + str(list(pixel)))

    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 15
    frames = 0
    start = last = time.monotonic()
    df = Path('/sys/class/devfreq/13000000.gpu')
    while time.monotonic() - start < seconds:
        frame()
        frames += 1
        now = time.monotonic()
        if now - last >= 1:
            print(json.dumps(dict(seconds=round(now-start, 2), frames=frames,
                                  current=int((df/'cur_freq').read_text()),
                                  target=int((df/'target_freq').read_text()))), flush=True)
            last = now
    print(json.dumps(dict(frames=frames, seconds=round(time.monotonic()-start, 2),
                          readback=list(pixel))), flush=True)
    # Each pause exceeds Panthor's 50 ms runtime autosuspend delay.
    power = df.resolve().parent.parent / 'power'
    original_control = (power / 'control').read_text().strip()
    suspended = 0
    try:
        (power / 'control').write_text('auto')
        for _ in range(120):
            frame()
            time.sleep(0.12)
            suspended += (power / 'runtime_status').read_text().strip() == 'suspended'
    finally:
        (power / 'control').write_text(original_control)
    print(json.dumps(dict(pm_iterations=120, suspended_observed=suspended,
                          readback=list(pixel))), flush=True)
finally:
    make_current(display, None, None, None)
    terminate(display)
