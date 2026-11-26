# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
#
# Titan 2: Mali-G615 is Valhall v11. Mesa (upstream main and our 26.2.5 backport, commit c66177bd
# "panvk: add v11 support") lists v11 among the "not well-tested" panvk archs (with v6, v7, v14):
# without this variable panvk_physical_device_init() returns VK_ERROR_INCOMPATIBLE_DRIVER and the
# loader shows no Mali device (vulkaninfo: only lavapipe or nothing). GLES (panfrost) is not affected.
# Unset it (or set T2_NO_PANVK=1 before login) to hide panvk again.
[ -n "${T2_NO_PANVK:-}" ] || export PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1
