# NaumiOS — Архитектура

## 1. Обзор

NaumiOS — экспериментальная ОС для AArch64, загружается через UEFI с помощью
бутлоадера **Limine**. Целевая среда разработки и тестирования — QEMU (`virt` machine).

## 2. Целевая платформа

| Параметр       | Значение                                |
|----------------|------------------------------------------|
| Архитектура    | AArch64 (ARMv8-A)                        |
| Машина QEMU    | `virt`                                   |
| CPU            | `cortex-a72` (пример, любой armv8 core)  |
| Прошивка       | UEFI (edk2-aarch64: QEMU_EFI.fd / QEMU_VARS.fd) |
| Бутлоадер      | Limine (протокол Limine boot protocol)   |
| Контроллер прерываний | GICv2/v3 (даёт QEMU virt)         |

## 3. Цепочка загрузки

```
QEMU (-pflash QEMU_EFI.fd -pflash QEMU_VARS.fd)
   │
   ▼
UEFI-прошивка edk2-aarch64
   │  читает ESP-раздел образа диска
   ▼
/EFI/BOOT/BOOTAA64.EFI  (Limine)
   │  читает limine.conf, находит kernel.elf
   ▼
kernel.elf — точка входа по Limine boot protocol
```

Limine на AArch64 к моменту передачи управления ядру уже:
- включает MMU и строит начальные таблицы страниц (identity + higher-half/HHDM);
- переходит в состояние, соответствующее AAPCS64 (LP64) calling convention.

Но **не** настраивает `VBAR_EL1` — его содержимое на входе не определено,
поэтому ядро обязано установить собственную таблицу векторов исключений
до того, как разрешать прерывания.

## 4. Инструменты и пакеты (Arch Linux)

Уже установлено ранее:
```bash
sudo pacman -S qemu-system-aarch64 make git dtc
paru -S aarch64-none-elf-gcc aarch64-none-elf-binutils aarch64-none-elf-gdb
```

Дополнительно для Limine-загрузки (всё есть в официальных репах):
```bash
sudo pacman -S limine          # деплой-утилита + BOOTAA64.EFI (extra, v12.x)
sudo pacman -S edk2-aarch64    # UEFI-прошивка для QEMU aarch64:
                                # /usr/share/edk2/aarch64/QEMU_CODE.fd, QEMU_VARS.fd
sudo pacman -S dosfstools mtools gptfdisk   # сборка GPT/FAT32 образа диска (ESP)
```

> Примечание: пакет `edk2-armvirt`, который иногда упоминается в старых
> инструкциях, в актуальных репозиториях Arch заменён на `edk2-aarch64`.

## 5. Структура репозитория

```
NaumiOS/
├── boot/
│   └── entry.c            # точка входа, парсинг Limine requests/responses
├── kernel/
│   ├── arch/aarch64/
│   │   ├── exceptions.S   # таблица векторов исключений (VBAR_EL1)
│   │   ├── gic.c          # инициализация GICv2/v3
│   │   ├── timer.c        # generic timer
│   │   └── context.S      # переключение контекста
│   ├── mm/
│   │   ├── pmm.c          # физический frame allocator (на основе Limine memmap)
│   │   ├── vmm.c          # таблицы страниц, виртуальная память
│   │   └── heap.c         # аллокатор кучи ядра
│   ├── drivers/
│   │   └── uart.c         # PL011 UART (консоль/логи)
│   ├── sched/
│   │   └── sched.c        # планировщик задач
│   └── main.c              # kernel_main
├── include/
│   └── limine.h            # официальный заголовок протокола Limine
├── boot-image/
│   └── limine.conf         # конфиг меню загрузки
├── scripts/
│   └── make-image.sh       # сборка GPT-образа с ESP-разделом
├── linker.ld                # linker script, higher-half layout
├── Makefile
├── .github/workflows/ci.yml
├── LICENSE
└── README.md
```

## 6. Особенности Limine boot protocol на AArch64

- Ядро — обычный ELF-executable, слинкованный на адрес верхней половины
  адресного пространства (higher half).
- В бинарнике на 8-байтной границе должен присутствовать **base revision tag** —
  три 64-битных значения (magic + magic + номер ревизии), которым ядро
  сообщает бутлоадеру, какую версию протокола поддерживает.
- Обмен данными идёт через секцию **requests**: ядро объявляет структуры-запросы
  (memory map, HHDM offset, framebuffer, RSDP/device tree blob, kernel address,
  modules/initrd), Limine заполняет в них ответы перед передачей управления.
- Точка входа — обычная C-функция, ABI = AAPCS64.
- **Важно**: точный layout структур запросов может отличаться между base
  revisions и версиями Limine — при реализации `boot/entry.c` сверяться
  с `PROTOCOL.md` из установленной версии пакета `limine`, а не с этим документом.

## 7. Слои ядра

1. **Entry & handshake** (`boot/entry.c`) — чтение ответов Limine, ранняя инициализация.
2. **arch/aarch64** — таблица векторов исключений, инициализация GIC, generic timer,
   переключение контекста.
3. **mm** — физический аллокатор фреймов на основе memmap от Limine, менеджер
   виртуальной памяти (таблицы страниц), куча ядра.
4. **drivers** — UART PL011 как первый драйвер для вывода логов; далее —
   framebuffer (через Limine framebuffer request), virtio-blk/virtio-net
   (QEMU virt их эмулирует "из коробки").
5. **sched** — сначала простой round-robin, затем вытесняющий планировщик
   по таймерным прерываниям.
6. **Долгосрочно**: переходы EL0/EL1 и syscalls, ELF-загрузчик пользовательских
   процессов, VFS + простая ФС (initrd можно подать через Limine module request).

## 8. Сборка загрузочного образа

Limine на UEFI грузится с ESP (EFI System Partition) — FAT32-раздела с файлом
`/EFI/BOOT/BOOTAA64.EFI`. Проще всего собрать GPT-образ диска с одним ESP-разделом:

```
scripts/make-image.sh:
  1. dd — создать пустой файл-образ (например, 64 МБ)
  2. sgdisk — создать GPT с одним разделом типа EFI System
  3. mkfs.fat -F32 — отформатировать раздел
  4. mmd/mcopy (mtools) — скопировать внутрь:
       /EFI/BOOT/BOOTAA64.EFI  (из пакета limine)
       /kernel.elf
       /limine.conf
```

## 9. Запуск в QEMU

```bash
cp /usr/share/edk2/aarch64/QEMU_VARS.fd .   # локальная копия, т.к. QEMU пишет в vars
qemu-system-aarch64 \
  -M virt -cpu cortex-a72 -m 512M \
  -pflash /usr/share/edk2/aarch64/QEMU_CODE.fd \
  -pflash QEMU_VARS.fd \
  -drive file=build/naumios.img,format=raw,if=virtio \
  -serial stdio -nographic
```

## 10. CI (GitHub Actions)

`.github/workflows/ci.yml`:
1. Установить кросс-тулчейн, limine, edk2-aarch64 в контейнере/раннере.
2. Собрать ядро и образ (`make && scripts/make-image.sh`).
3. Прогнать в QEMU headless с таймаутом, проверить по serial-выводу
   ключевую строку (например, `"NaumiOS boot OK"`), чтобы ловить регрессии
   ещё до мержа PR.

## 11. Roadmap

Соответствует текущему task list проекта:

1. ~~Git-репозиторий, структура, лицензия~~ — готово
2. Makefile + linker script (higher-half, под Limine)
3. `boot/entry.c` — обработка Limine requests, точка входа
4. UART-вывод, сборка загрузочного образа, первый успешный boot в QEMU
5. Exception vectors (VBAR_EL1)
6. Управление памятью (pmm/vmm/heap)
7. GIC + generic timer
8. Планировщик задач
9. GitHub Actions CI с автозапуском в QEMU
