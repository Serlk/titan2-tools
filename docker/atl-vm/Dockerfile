# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com>
#
# Host side of the ATL VM bench (scripts/atl-vm.sh): qemu-system-aarch64 with virtio-gpu-gl
# (virglrenderer on the host GPU through /dev/dri/renderD*), egl-headless + VNC, 9p share,
# e2fsprogs for the guest disk image, python3 for the QMP client.
#   docker build -t titan2-atl-qemu docker/atl-vm
FROM alpine:3.24
RUN apk add --no-cache qemu-system-aarch64 qemu-img qemu-hw-display-virtio-gpu-gl \
	qemu-hw-display-virtio-gpu-pci-gl qemu-ui-egl-headless qemu-ui-opengl virglrenderer \
	mesa-dri-gallium mesa-egl mesa-gl mesa-gbm mesa-vulkan-ati vulkan-loader vulkan-loader-dev e2fsprogs python3 socat util-linux-misc
