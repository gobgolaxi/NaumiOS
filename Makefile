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

# Userland: every userland/<name>/ directory with its own user.ld is a
# separate freestanding EL0 executable (fixed load address, single
# PT_LOAD), built to build/<name>.elf. Not part of the kernel image itself
# (loaded later, either as a Limine module or read off the FAT16 volume by
# the shell's `run` command — see kernel/loader/spawn.c), so this whole
# tree stays outside kernel/ and out of CSRCS. userland/lib/ is headers
# only (naumi.h), not a program, so it has no user.ld and is skipped.
USER_LD_SCRIPTS := $(wildcard userland/*/user.ld)
USER_PROGRAMS   := $(patsubst userland/%/user.ld,%,$(USER_LD_SCRIPTS))
USER_ELFS       := $(patsubst %,$(BUILD_DIR)/%.elf,$(USER_PROGRAMS))

USER_LDFLAGS = \
	-nostdlib \
	-static \
	-z max-page-size=0x1000 \
	--gc-sections \
	-m aarch64elf \
	-T userland/$(1)/user.ld

DEPS := $(filter %.d,$(OBJS:.o=.d))

.PHONY: all run clean

all: $(KERNEL_ELF) $(USER_ELFS)

$(KERNEL_ELF): $(OBJS) linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) $(OBJS) -o $@

# One rule per userland program, generated below: build/<name>.elf from
# every .c under userland/<name>/, linked with userland/<name>/user.ld.
define USERLAND_PROGRAM
USER_SRCS_$(1) := $$(shell find userland/$(1) -type f -name '*.c')
USER_OBJS_$(1) := $$(patsubst %.c,$$(BUILD_DIR)/%.c.o,$$(USER_SRCS_$(1)))
DEPS += $$(filter %.d,$$(USER_OBJS_$(1):.o=.d))

$$(BUILD_DIR)/$(1).elf: $$(USER_OBJS_$(1)) userland/$(1)/user.ld
	@mkdir -p $$(dir $$@)
	$$(LD) $$(call USER_LDFLAGS,$(1)) $$(USER_OBJS_$(1)) -o $$@
endef

$(foreach prog,$(USER_PROGRAMS),$(eval $(call USERLAND_PROGRAM,$(prog))))

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
