#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
IMAGE="$BUILD_DIR/naumios.hdd"
FW_CODE="/usr/share/edk2/aarch64/QEMU_EFI.fd"
FW_VARS_SRC="/usr/share/edk2/aarch64/QEMU_VARS.fd"
FW_VARS="$BUILD_DIR/QEMU_VARS.fd"

if [ ! -f "$IMAGE" ]; then
    echo "Образ не собран — сначала запусти: scripts/make-image.sh" >&2
    exit 1
fi

# QEMU_VARS.fd должен быть локальной перезаписываемой копией — прошивка пишет в него.
cp -f "$FW_VARS_SRC" "$FW_VARS"

# NAUMIOS_DISPLAY=gtk (or sdl) scripts/run-qemu.sh opens a real graphical
# window showing the framebuffer, with keyboard/mouse forwarded to the
# guest via virtio-input. Default stays headless (`none`) for automated/
# unattended runs — `qemu-system-aarch64 -display help` lists what your
# build supports if gtk isn't it.
DISPLAY_MODE="${NAUMIOS_DISPLAY:-none}"

# Explicit virtio-mmio (virtio-blk-device) rather than the `if=virtio`
# shorthand, which on `virt` defaults to virtio-blk-*pci* — mmio is what our
# own virtio_mmio.c driver understands, and skips needing a PCI enumerator
# just to talk to the boot/data disk. EDK2 boots from virtio-mmio disks
# fine too (VirtioFdtDxe, driven off the same DTB nodes we scan).
exec qemu-system-aarch64 \
    -M virt \
    -cpu cortex-a72 \
    -m 512M \
    -drive if=pflash,unit=0,format=raw,file="$FW_CODE",readonly=on \
    -drive if=pflash,unit=1,format=raw,file="$FW_VARS" \
    -drive file="$IMAGE",format=raw,if=none,id=hd0 \
    -device virtio-blk-device,drive=hd0 \
    -device ramfb \
    -device virtio-keyboard-device \
    -device virtio-tablet-device \
    -serial stdio \
    -display "$DISPLAY_MODE"
