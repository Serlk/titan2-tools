#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
#
# Copies the "mtk-ccci" ModemManager plugin (tools/mm-titan2/plugin) into a
# ModemManager source tree as src/plugins/mtk-ccci and wires it into the meson
# build (meson_options.txt, meson.build, src/plugins/meson.build,
# src/plugins/mm-builtin-plugins.c). Idempotent: running it again refreshes
# the copied files and leaves the already patched build files alone.
#
#   tools/mm-titan2/apply.sh [MM_TREE]
#
# MM_TREE (argument or environment; default: src/ModemManager in this repository) is a
# ModemManager checkout at the commit Alpine 3.24 ships:
#   git clone https://gitlab.freedesktop.org/mobile-broadband/ModemManager.git src/ModemManager
#   git -C src/ModemManager checkout fcc9d19f
#
# Then: meson setup build -Dplugin_mtk_ccci=enabled ... && meson compile -C build
set -eu

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
mm=${1:-${MM_TREE:-$repo/src/ModemManager}}

[ -f "$mm/meson.build" ] && [ -d "$mm/src/plugins" ] || {
	echo "apply.sh: $mm is not a ModemManager source tree" >&2
	exit 1
}

dst="$mm/src/plugins/mtk-ccci"
mkdir -p "$dst/tests"
for f in "$here"/plugin/*.c "$here"/plugin/*.h "$here"/plugin/*.rules; do
	cp -f "$f" "$dst/"
done
cp -f "$here"/plugin/tests/*.c "$dst/tests/"
echo "plugin files copied to $dst"

python3 -I - "$mm" <<'PY'
import re, sys, pathlib

mm = pathlib.Path(sys.argv[1])
marker = "mtk-ccci"

def patch(path, check, apply):
    text = path.read_text()
    if check in text:
        print(f"{path.relative_to(mm)}: already patched")
        return
    new = apply(text)
    if new == text:
        sys.exit(f"apply.sh: could not find the insertion point in {path}")
    path.write_text(new)
    print(f"{path.relative_to(mm)}: patched")

# 1. the meson option
p = mm / "meson_options.txt"
patch(p, "plugin_mtk_ccci", lambda t: t.replace(
    "option('plugin_mtk', type: 'feature', value: 'auto', description: 'enable mtk plugin support')\n",
    "option('plugin_mtk', type: 'feature', value: 'auto', description: 'enable mtk plugin support')\n"
    "option('plugin_mtk_ccci', type: 'feature', value: 'auto', description: 'enable mtk ccci (MT6878 integrated modem) plugin support')\n"))

# 2. the plugin's requirements in the top-level meson.build
p = mm / "meson.build"
patch(p, "'mtk-ccci':", lambda t: t.replace(
    "  'mtk': {'available': true, 'shared': mtk_shared_reqs},\n",
    "  'mtk': {'available': true, 'shared': mtk_shared_reqs},\n"
    "  'mtk-ccci': {'available': true, 'shared': []},\n"))

# 3. the build definition in src/plugins/meson.build
section = """# plugin: mtk-ccci (MediaTek MT6878 integrated modem behind the mtk_ccci WWAN driver)
if plugins_options['mtk-ccci']
  test_udev_rules_dir_c_args = ['-DTESTUDEVRULESDIR_MTK_CCCI="@0@"'.format(plugins_dir / 'mtk-ccci')]
  plugins_test_udev_rules_dir_c_args += test_udev_rules_dir_c_args

  common_c_args = ['-DMM_MODULE_NAME="mtk-ccci"', '-DPKGSTATEDIR="@0@"'.format(mm_pkgsharedstatedir)]

  sources = files(
    'mtk-ccci/mm-bearer-mtk-ccci.c',
    'mtk-ccci/mm-broadband-modem-mtk-ccci.c',
    'mtk-ccci/mm-plugin-mtk-ccci.c',
    'mtk-ccci/mm-port-mipc.c',
    'mtk-ccci/mm-wwan-link.c',
    'mtk-ccci/mm-mtk-ccci-ims.c',
    'mtk-ccci/mm-mtk-ccci-sms-ready.c',
    'mtk-ccci/mm-mtk-ccci-sms-poll.c',
    'mtk-ccci/mm-call-mtk-ccci.c',
    'mtk-ccci/mm-sim-mtk-ccci.c',
  )

  plugins += {'plugin-mtk-ccci': {
    'plugin': true,
    'helper': {'sources': files('mtk-ccci/mm-mipc-proto.c', 'mtk-ccci/mm-modem-helpers-mtk-ccci.c'), 'include_directories': plugins_incs, 'c_args': common_c_args},
    'module': {'sources': sources, 'include_directories': plugins_incs, 'c_args': test_udev_rules_dir_c_args + common_c_args},
    'test': {'sources': files('mtk-ccci/tests/test-mipc-proto.c'), 'include_directories': include_directories('mtk-ccci'), 'dependencies': libhelpers_dep},
  }}

  plugins_udev_rules += files('mtk-ccci/77-mm-mtk-ccci.rules')

  if enable_tests
    # MMPortMipc against a fake modem on a pty (links the daemon's port library)
    test_port_mipc = executable(
      'test-plugin-mtk-ccci-port-mipc',
      sources: files('mtk-ccci/tests/test-port-mipc.c', 'mtk-ccci/mm-port-mipc.c', 'mtk-ccci/mm-mipc-proto.c', 'mtk-ccci/mm-mtk-ccci-ims.c'),
      include_directories: plugins_incs + [include_directories('mtk-ccci')],
      dependencies: [libport_dep, libmm_glib_dep, util_dep],
      c_args: common_c_args,
    )
    test('test-plugin-mtk-ccci-port-mipc', test_port_mipc)
    test_sms_ready = executable(
      'test-plugin-mtk-ccci-sms-ready',
      sources: files('mtk-ccci/tests/test-sms-ready.c', 'mtk-ccci/mm-mtk-ccci-sms-ready.c', 'mtk-ccci/mm-port-mipc.c', 'mtk-ccci/mm-mipc-proto.c'),
      include_directories: plugins_incs + [include_directories('mtk-ccci')],
      dependencies: [libport_dep, libmm_glib_dep],
      c_args: common_c_args + ['-DSMS_READY_RETRY_MS=1'],
    )
    test('test-plugin-mtk-ccci-sms-ready', test_sms_ready)
    test_sms_poll = executable(
      'test-plugin-mtk-ccci-sms-poll',
      sources: files('mtk-ccci/tests/test-sms-poll.c', 'mtk-ccci/mm-mtk-ccci-sms-poll.c'),
      include_directories: plugins_incs + [include_directories('mtk-ccci')],
      dependencies: [libmmbase_dep, libsms_dep],
      c_args: common_c_args,
    )
    test('test-plugin-mtk-ccci-sms-poll', test_sms_poll)
    test_dual_sim = executable(
      'test-plugin-mtk-ccci-dual-sim',
      sources: files('mtk-ccci/tests/test-dual-sim.c', 'mtk-ccci/mm-sim-mtk-ccci.c', '../tests/fake-modem.c', '../tests/fake-call.c', 'mtk-ccci/mm-call-mtk-ccci.c', 'mtk-ccci/mm-port-mipc.c', 'mtk-ccci/mm-mipc-proto.c', 'mtk-ccci/mm-modem-helpers-mtk-ccci.c'),
      include_directories: plugins_incs + [include_directories('mtk-ccci')],
      dependencies: [libmmbase_dep, util_dep],
      c_args: common_c_args,
    )
    test('test-plugin-mtk-ccci-dual-sim', test_dual_sim)
    test_bearer_mipc = executable(
      'test-plugin-mtk-ccci-bearer-mipc',
      sources: files('mtk-ccci/tests/test-bearer-mipc.c', 'mtk-ccci/mm-bearer-mtk-ccci.c', 'mtk-ccci/mm-port-mipc.c', 'mtk-ccci/mm-mipc-proto.c'),
      include_directories: plugins_incs + [include_directories('mtk-ccci')],
      dependencies: [libmmbase_dep, libsms_dep, util_dep],
      c_args: common_c_args,
    )
    test('test-plugin-mtk-ccci-bearer-mipc', test_bearer_mipc)
  endif
endif

"""
p = mm / "src/plugins/meson.build"
t = p.read_text()
start = t.find("# plugin: mtk-ccci (")
end = t.find("builtin_sources = []\n", max(start, 0))
if end < 0:
    sys.exit(f"apply.sh: missing builtin_sources in {p}")
if start < 0:
    start = end
new = t[:start] + section + t[end:]
if new != t:
    p.write_text(new)
    print(f"{p.relative_to(mm)}: plugin section refreshed")
else:
    print(f"{p.relative_to(mm)}: already patched")

# 4. the built-in plugin list (used with -Dbuiltin_plugins=true)
p = mm / "src/plugins/mm-builtin-plugins.c"
def builtin(t):
    t = t.replace("#if defined ENABLE_PLUGIN_MTK\nMMPlugin *mm_plugin_create_mtk (void);\n#endif\n",
                  "#if defined ENABLE_PLUGIN_MTK\nMMPlugin *mm_plugin_create_mtk (void);\n#endif\n"
                  "#if defined ENABLE_PLUGIN_MTK_CCCI\nMMPlugin *mm_plugin_create_mtk_ccci (void);\n#endif\n", 1)
    t = t.replace("#if defined ENABLE_PLUGIN_MTK\n    PREPEND_PLUGIN (mtk);\n#endif\n",
                  "#if defined ENABLE_PLUGIN_MTK\n    PREPEND_PLUGIN (mtk);\n#endif\n"
                  "#if defined ENABLE_PLUGIN_MTK_CCCI\n    PREPEND_PLUGIN (mtk_ccci);\n#endif\n", 1)
    return t
patch(p, "ENABLE_PLUGIN_MTK_CCCI", builtin)

# 5. the udev rules file goes through MM's generic rules parser in the test suite
p = mm / "src/plugins/tests/test-udev-rules.c"
def udev_test(t):
    t = t.replace("#if defined ENABLE_PLUGIN_LINKTOP\nstatic void\ntest_linktop (void)\n",
                  "#if defined ENABLE_PLUGIN_MTK_CCCI\nstatic void\ntest_mtk_ccci (void)\n{\n    common_test (TESTUDEVRULESDIR_MTK_CCCI);\n}\n#endif\n\n"
                  "#if defined ENABLE_PLUGIN_LINKTOP\nstatic void\ntest_linktop (void)\n", 1)
    t = t.replace("#if defined ENABLE_PLUGIN_LINKTOP\n    g_test_add_func (\"/MM/test-udev-rules/linktop\", test_linktop);\n",
                  "#if defined ENABLE_PLUGIN_MTK_CCCI\n    g_test_add_func (\"/MM/test-udev-rules/mtk-ccci\", test_mtk_ccci);\n#endif\n"
                  "#if defined ENABLE_PLUGIN_LINKTOP\n    g_test_add_func (\"/MM/test-udev-rules/linktop\", test_linktop);\n", 1)
    return t
patch(p, "TESTUDEVRULESDIR_MTK_CCCI", udev_test)
PY

echo "done; configure with: meson setup build -Dplugin_mtk_ccci=enabled (in $mm)"
