# NaumiOS kernel build — AArch64, loaded by Limine.

OUTPUT      := kernel
TOOLCHAIN   := aarch64-none-elf-
CC          := $(TOOLCHAIN)gcc
LD          := $(TOOLCHAIN)ld

BUILD_DIR   := build
KERNEL_ELF  := $(BUILD_DIR)/$(OUTPUT).elf

CFLAGS := \
	-Wall -Wextra \
	-std=gnu11 \
	-nostdinc \
	-ffreestanding \
	-fno-stack-protector \
	-fno-stack-check \
	-fno-lto \
	-fno-PIC \
	-ffunction-sections \
	-fdata-sections \
	-mcpu=generic \
	-march=armv8-a+nofp+nosimd \
	-mgeneral-regs-only \
	-g -O2

GCC_FREESTANDING_INCLUDE := $(shell $(CC) -print-file-name=include)
CPPFLAGS := -Iinclude -isystem $(GCC_FREESTANDING_INCLUDE) -MMD -MP

LDFLAGS := \
	-nostdlib \
	-static \
	-z max-page-size=0x1000 \
	--gc-sections \
	-m aarch64elf \
	-T linker.ld

CSRCS := $(shell find kernel -type f -name '*.c')
SSRCS := $(shell find kernel -type f -name '*.S')
OBJS := $(patsubst %.c,$(BUILD_DIR)/%.c.o,$(CSRCS)) $(patsubst %.S,$(BUILD_DIR)/%.S.o,$(SSRCS))
DEPS := $(filter %.d,$(OBJS:.o=.d))

.PHONY: all run clean

all: $(KERNEL_ELF)

$(KERNEL_ELF): $(OBJS) linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) $(OBJS) -o $@

$(BUILD_DIR)/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(BUILD_DIR)/%.S.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

-include $(DEPS)

run: all
	scripts/make-image.sh
	scripts/run-qemu.sh

clean:
	rm -rf $(BUILD_DIR)
