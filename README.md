# NaumiOS

[![build](https://github.com/gobgolaxi/NaumiOS/actions/workflows/build.yml/badge.svg)](https://github.com/gobgolaxi/NaumiOS/actions/workflows/build.yml)

Экспериментальная операционная система для архитектуры ARM (AArch64), написанная с нуля.

## Требования для сборки

- `aarch64-none-elf-gcc` / `aarch64-none-elf-binutils` / `aarch64-none-elf-gdb` (AUR)
- `qemu-system-aarch64`
- `make`, `dtc`

Установка на Arch Linux:

```bash
sudo pacman -S qemu-system-aarch64 make git dtc
paru -S aarch64-none-elf-gcc aarch64-none-elf-binutils aarch64-none-elf-gdb
```

## Сборка и запуск

```bash
make
make run   # запуск в QEMU (virt machine)
```

## Структура проекта

- `boot/` — стартовый ассемблерный код (точка входа, переход в ядро)
- `kernel/` — исходный код ядра на C
- `include/` — заголовочные файлы

## Целевая платформа

- Архитектура: AArch64
- Эмуляция: QEMU `virt` machine

## Лицензия

[MIT](LICENSE)
