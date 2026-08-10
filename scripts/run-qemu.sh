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

exec qemu-system-aarch64 \
    -M virt \
    -cpu cortex-a72 \
    -m 512M \
    -drive if=pflash,unit=0,format=raw,file="$FW_CODE",readonly=on \
    -drive if=pflash,unit=1,format=raw,file="$FW_VARS" \
    -drive file="$IMAGE",format=raw,if=virtio \
    -serial stdio \
    -display none
