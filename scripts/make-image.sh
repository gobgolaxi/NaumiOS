#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
KERNEL_ELF="$BUILD_DIR/kernel.elf"
IMAGE="$BUILD_DIR/naumios.hdd"
LIMINE_EFI="/usr/share/limine/BOOTAA64.EFI"

if [ ! -f "$KERNEL_ELF" ]; then
    echo "Ядро не собрано — сначала запусти: make" >&2
    exit 1
fi

if [ ! -f "$LIMINE_EFI" ]; then
    echo "Не найден $LIMINE_EFI — установи пакет limine: sudo pacman -S limine" >&2
    exit 1
fi

rm -f "$IMAGE"
dd if=/dev/zero of="$IMAGE" bs=1M count=0 seek=64 status=none
sgdisk "$IMAGE" -n 1:2048 -t 1:ef00
mformat -i "${IMAGE}@@1M"
mmd -i "${IMAGE}@@1M" ::/EFI ::/EFI/BOOT ::/boot ::/boot/limine
mcopy -i "${IMAGE}@@1M" "$KERNEL_ELF" ::/boot/kernel
mcopy -i "${IMAGE}@@1M" "$ROOT_DIR/boot-image/limine.conf" ::/boot/limine/
mcopy -i "${IMAGE}@@1M" "$LIMINE_EFI" ::/EFI/BOOT/

echo "Образ собран: $IMAGE"
