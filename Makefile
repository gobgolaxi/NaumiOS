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
# userland/lib/include holds a small <stdio.h>/<stdlib.h>/<string.h>/
# <ctype.h>/<assert.h>/<math.h> shim (see userland/lib/stdio.c, libc.c) —
# this toolchain is -nostdinc/-ffreestanding, so there is no real libc to
# supply these. Only third-party sources (third_party/doomgeneric) actually
# use system-header-style angle-bracket includes for them; kernel/ and the
# rest of userland/ don't touch this path.
# DOOMGENERIC_RESX/RESY: every third_party/doomgeneric translation unit
# must agree on these (each includes doomgeneric.h separately and falls
# back to its own 640x400 default otherwise), so this has to be a global
# define rather than something set only in our own platform file. 320x200
# is vanilla DOOM's native SCREENWIDTH/SCREENHEIGHT (see i_video.h) — matching
# it exactly means i_video.c's own scaling math (s_Fb vs SCREENWIDTH*fb_scaling)
# is a no-op, so DG_ScreenBuffer is a plain unscaled 320x200 RGB buffer.
# Inert for every other target (kernel, other userland programs), which
# never reference these macros.
# RAISKAOS_SOUND: a pre-existing hook already wired up in
# third_party/doomgeneric/i_sound.h/i_sound.c specifically for external
# ports to register their own DG_sound_module/DG_music_module without
# patching those files (`#if defined(FEATURE_SOUND) || defined(RAISKAOS_SOUND)`
# — named after whichever fork of doomgeneric this tree came from, not
# implying any relationship to that other project). Defining just this
# (not FEATURE_SOUND) keeps the rest of that flag's broader surface
# (Timidity config, extra menu entries, ...) switched off — this port only
# wants the sound *module* hook, see userland/doom/doomgeneric_naumios.c.
CPPFLAGS := -Iinclude -Iuserland/lib/include -isystem $(GCC_FREESTANDING_INCLUDE) \
	-DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 -DRAISKAOS_SOUND -MMD -MP

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
# tree stays outside kernel/ and out of CSRCS. userland/lib/ has no
# user.ld of its own (it's not a program) — its .c files (libc.c) are
# compiled into every program's build below instead, so every userland
# program gets malloc/memcpy/etc "for free" without a shared-library link
# step (this toolchain is -nostdlib, there is no dynamic linker).
USER_LIB_SRCS := $(shell find userland/lib -type f -name '*.c')

# The DOOM engine itself (third_party/doomgeneric — see that directory's
# LICENSE, GPLv2, separate from this project's own MIT license), minus
# every backend it ships for platforms we're not (SDL/X11/Windows/Allegro/
# emscripten/etc — userland/doom/doomgeneric_naumios.c is this project's
# own) and w_file_stdc.c (userland/doom/w_file_naumios.c replaces it — see
# that file for why). This exact file list is a known-working reference
# (confirmed compiling and linking DOOM for another from-scratch OS on this
# same machine), not independently rediscovered.
DOOM_ENGINE_SRCS := $(addprefix third_party/doomgeneric/, \
	dummy.c am_map.c doomdef.c doomstat.c dstrings.c d_event.c d_items.c \
	d_iwad.c d_loop.c d_main.c d_mode.c d_net.c f_finale.c f_wipe.c g_game.c \
	hu_lib.c hu_stuff.c info.c i_cdmus.c i_endoom.c i_joystick.c i_scale.c \
	i_sound.c i_system.c i_timer.c memio.c m_argv.c m_bbox.c m_cheat.c \
	m_config.c m_controls.c m_fixed.c m_menu.c m_misc.c m_random.c \
	p_ceilng.c p_doors.c p_enemy.c p_floor.c p_inter.c p_lights.c p_map.c \
	p_maputl.c p_mobj.c p_plats.c p_pspr.c p_saveg.c p_setup.c p_sight.c \
	p_spec.c p_switch.c p_telept.c p_tick.c p_user.c r_bsp.c r_data.c \
	r_draw.c r_main.c r_plane.c r_segs.c r_sky.c r_things.c sha1.c sounds.c \
	statdump.c st_lib.c st_stuff.c s_sound.c tables.c v_video.c wi_stuff.c \
	w_checksum.c w_file.c w_main.c w_wad.c z_zone.c i_input.c i_video.c \
	doomgeneric.c)

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
# The "doom" program additionally pulls in the engine sources listed above
# (they live under third_party/, not userland/doom/, so the plain `find`
# above them doesn't see them).
define USERLAND_PROGRAM
USER_SRCS_$(1) := $$(shell find userland/$(1) -type f -name '*.c') $$(USER_LIB_SRCS)
ifeq ($(1),doom)
USER_SRCS_$(1) += $$(DOOM_ENGINE_SRCS)
endif
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
