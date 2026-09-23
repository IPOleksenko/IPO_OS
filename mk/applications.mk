.PHONY: applications apps clean-applications clean-apps

APPS_DIR     := applications
APPS_BUILD   := build/applications
APPS_SRCS_RAW := $(shell find $(APPS_DIR) -maxdepth 2 -name "*.c" -type f)
GENERIC_APP_SRCS := $(filter-out $(APPS_DIR)/tcc/% $(APPS_DIR)/lua/% $(APPS_DIR)/micropython/% $(APPS_DIR)/nasm/% $(APPS_DIR)/doom/%, $(APPS_SRCS_RAW))
APPS_BINS    := $(patsubst $(APPS_DIR)/%.c, $(APPS_BUILD)/%.bin, $(GENERIC_APP_SRCS)) $(APPS_BUILD)/tcc/tcc.bin $(APPS_BUILD)/doom/doom.bin


APP_ENTRY_OBJ   := $(APPS_BUILD)/entry.o
APP_PRINTF_OBJ  := $(APPS_BUILD)/app_printf.o
APP_KMALLOC_OBJ := $(APPS_BUILD)/app_kmalloc.o
APP_WM_OBJ      := $(APPS_BUILD)/app_wm.o

# Applications are compiled as Position Independent Code (PIC).
# This allows them to run from any memory address dynamically allocated
APPS_CFLAGS := -m32 -O2 \
	-ffreestanding \
	-fno-pic -fno-pie \
	-fno-builtin \
	-nostdlib -nostartfiles \
	-DIPO_APP \
	-Isrc/include \
	-I$(APPS_DIR)/include \
	-I$(APPS_DIR)/micropython/lib/uzlib \
	-I.

TCC_CFLAGS := -m32 \
	-ffreestanding \
	-fno-pic -fno-pie \
	-fno-builtin \
	-nostdlib -nostartfiles \
	-DIPO_APP \
	-DONE_SOURCE=1 \
	-I$(APPS_DIR)/tcc/include \
	-I$(APPS_DIR)/tcc \
	-Isrc/include

# Build all applications
applications: $(APPS_BINS)
apps: applications

$(APP_ENTRY_OBJ): $(APPS_DIR)/entry.s
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ $(APPS_CFLAGS)

$(APP_PRINTF_OBJ): src/kernel/stdio/printf.c
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ -std=gnu11 $(APPS_CFLAGS) -DIPO_APP

$(APP_KMALLOC_OBJ): src/kernel/mm/kmalloc.c
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ -std=gnu11 $(APPS_CFLAGS) -DIPO_APP

$(APP_WM_OBJ): src/drivers/video/wm.c
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ -std=gnu11 $(APPS_CFLAGS) -DIPO_APP

$(APPS_BUILD)/tcc/setjmp.o: $(APPS_DIR)/tcc/setjmp.s
	@mkdir -p $(dir $@)
	$(ASM) -f elf32 $< -o $@

$(APPS_BUILD)/tcc/libc_shim.o: $(APPS_DIR)/tcc/libc_shim.c
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ -std=gnu11 $(TCC_CFLAGS)

$(APPS_BUILD)/tcc/tcc.o: $(APPS_DIR)/tcc/tcc.c $(APPS_DIR)/tcc/libtcc.c $(APPS_DIR)/tcc/tcctools.c $(APPS_DIR)/tcc/tccpp.c $(APPS_DIR)/tcc/tccgen.c $(APPS_DIR)/tcc/tccelf.c $(APPS_DIR)/tcc/tccasm.c $(APPS_DIR)/tcc/tccrun.c $(APPS_DIR)/tcc/tccdbg.c $(APPS_DIR)/tcc/i386-gen.c $(APPS_DIR)/tcc/i386-link.c $(APPS_DIR)/tcc/i386-asm.c
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ -std=gnu11 $(TCC_CFLAGS)

$(APPS_BUILD)/tcc/tcc.bin: $(APPS_BUILD)/tcc/tcc.o $(APPS_BUILD)/tcc/libc_shim.o $(APPS_BUILD)/tcc/setjmp.o $(APP_ENTRY_OBJ) $(APP_PRINTF_OBJ) $(APP_KMALLOC_OBJ) $(LIB_A)
	@mkdir -p $(dir $@)
	@echo "[applications] Building official TinyCC: $(APPS_DIR)/tcc/tcc.c → $@"
	@$(CC) $(APPS_CFLAGS) -Wl,-T,$(APPS_DIR)/app.ld \
		$(APP_ENTRY_OBJ) $(APPS_BUILD)/tcc/tcc.o $(APPS_BUILD)/tcc/libc_shim.o $(APPS_BUILD)/tcc/setjmp.o $(APP_PRINTF_OBJ) $(APP_KMALLOC_OBJ) -Wl,--start-group $(LIB_A) -lgcc -Wl,--end-group -o $@.elf -nostdlib -nostartfiles 2>&1 | grep -v "PIE\|relocation" || true
	@$(OBJCOPY) --set-section-flags .bss=alloc,load,contents \
		-j .text -j .rodata -j .data -j .bss -O binary $@.elf $@
	@rm -f $@.elf
	@SIZE=$$(stat -c%s "$@" 2>/dev/null || stat -f%z "$@" 2>/dev/null); \
	echo "[applications] ✓ Created: $@ ($$SIZE bytes)"

# Browser rules
BROWSER_OBJS := \
	$(APPS_BUILD)/browser/browser.o \
	$(APPS_BUILD)/browser/dom.o \
	$(APPS_BUILD)/browser/html_parser.o \
	$(APPS_BUILD)/browser/http_client.o \
	$(APPS_BUILD)/browser/layout.o \
	$(APPS_BUILD)/browser/tinflate.o \
	$(APPS_BUILD)/browser/header.o \
	$(APPS_BUILD)/browser/adler32.o \
	$(APPS_BUILD)/browser/crc32.o

$(APPS_BUILD)/browser/tinflate.o: $(APPS_DIR)/micropython/lib/uzlib/tinflate.c
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ -std=gnu11 $(APPS_CFLAGS)

$(APPS_BUILD)/browser/header.o: $(APPS_DIR)/micropython/lib/uzlib/header.c
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ -std=gnu11 $(APPS_CFLAGS)

$(APPS_BUILD)/browser/adler32.o: $(APPS_DIR)/micropython/lib/uzlib/adler32.c
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ -std=gnu11 $(APPS_CFLAGS)

$(APPS_BUILD)/browser/crc32.o: $(APPS_DIR)/micropython/lib/uzlib/crc32.c
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ -std=gnu11 $(APPS_CFLAGS)

$(APPS_BUILD)/browser/browser.bin: $(BROWSER_OBJS) $(APP_ENTRY_OBJ) $(APP_PRINTF_OBJ) $(APP_KMALLOC_OBJ) $(APP_WM_OBJ) $(LIB_A)
	@mkdir -p $(dir $@)
	@echo "[applications] Building Web Browser: $(APPS_DIR)/browser → $@"
	@$(CC) $(APPS_CFLAGS) -Wl,-T,$(APPS_DIR)/app.ld \
		$(APP_ENTRY_OBJ) $(BROWSER_OBJS) $(APP_PRINTF_OBJ) $(APP_KMALLOC_OBJ) $(APP_WM_OBJ) -Wl,--start-group $(LIB_A) -lgcc -Wl,--end-group -o $@.elf -nostdlib -nostartfiles 2>&1 | grep -v "PIE\|relocation" || true
	@$(OBJCOPY) --set-section-flags .bss=alloc,load,contents \
		-j .text -j .rodata -j .data -j .bss -O binary $@.elf $@
	@rm -f $@.elf
	@SIZE=$$(stat -c%s "$@" 2>/dev/null || stat -f%z "$@" 2>/dev/null); \
	echo "[applications] ✓ Created: $@ ($$SIZE bytes)"
# DOOM (official linuxdoom-1.10) rules
DOOM_DIR := $(APPS_DIR)/doom
DOOM_CORE_DIR := $(DOOM_DIR)/linuxdoom
DOOM_BUILD := $(APPS_BUILD)/doom

DOOM_SRCS := \
	$(DOOM_CORE_DIR)/am_map.c \
	$(DOOM_CORE_DIR)/doomdef.c \
	$(DOOM_CORE_DIR)/doomstat.c \
	$(DOOM_CORE_DIR)/dstrings.c \
	$(DOOM_CORE_DIR)/d_items.c \
	$(DOOM_CORE_DIR)/d_main.c \
	$(DOOM_CORE_DIR)/d_net.c \
	$(DOOM_CORE_DIR)/f_finale.c \
	$(DOOM_CORE_DIR)/f_wipe.c \
	$(DOOM_CORE_DIR)/g_game.c \
	$(DOOM_CORE_DIR)/hu_lib.c \
	$(DOOM_CORE_DIR)/hu_stuff.c \
	$(DOOM_CORE_DIR)/info.c \
	$(DOOM_CORE_DIR)/m_argv.c \
	$(DOOM_CORE_DIR)/m_bbox.c \
	$(DOOM_CORE_DIR)/m_cheat.c \
	$(DOOM_CORE_DIR)/m_fixed.c \
	$(DOOM_CORE_DIR)/m_menu.c \
	$(DOOM_CORE_DIR)/m_misc.c \
	$(DOOM_CORE_DIR)/m_random.c \
	$(DOOM_CORE_DIR)/m_swap.c \
	$(DOOM_CORE_DIR)/p_ceilng.c \
	$(DOOM_CORE_DIR)/p_doors.c \
	$(DOOM_CORE_DIR)/p_enemy.c \
	$(DOOM_CORE_DIR)/p_floor.c \
	$(DOOM_CORE_DIR)/p_inter.c \
	$(DOOM_CORE_DIR)/p_lights.c \
	$(DOOM_CORE_DIR)/p_map.c \
	$(DOOM_CORE_DIR)/p_maputl.c \
	$(DOOM_CORE_DIR)/p_mobj.c \
	$(DOOM_CORE_DIR)/p_plats.c \
	$(DOOM_CORE_DIR)/p_pspr.c \
	$(DOOM_CORE_DIR)/p_saveg.c \
	$(DOOM_CORE_DIR)/p_setup.c \
	$(DOOM_CORE_DIR)/p_sight.c \
	$(DOOM_CORE_DIR)/p_spec.c \
	$(DOOM_CORE_DIR)/p_switch.c \
	$(DOOM_CORE_DIR)/p_telept.c \
	$(DOOM_CORE_DIR)/p_tick.c \
	$(DOOM_CORE_DIR)/p_user.c \
	$(DOOM_CORE_DIR)/r_bsp.c \
	$(DOOM_CORE_DIR)/r_data.c \
	$(DOOM_CORE_DIR)/r_draw.c \
	$(DOOM_CORE_DIR)/r_main.c \
	$(DOOM_CORE_DIR)/r_plane.c \
	$(DOOM_CORE_DIR)/r_segs.c \
	$(DOOM_CORE_DIR)/r_sky.c \
	$(DOOM_CORE_DIR)/r_things.c \
	$(DOOM_CORE_DIR)/sounds.c \
	$(DOOM_CORE_DIR)/s_sound.c \
	$(DOOM_CORE_DIR)/st_lib.c \
	$(DOOM_CORE_DIR)/st_stuff.c \
	$(DOOM_CORE_DIR)/tables.c \
	$(DOOM_CORE_DIR)/v_video.c \
	$(DOOM_CORE_DIR)/wi_stuff.c \
	$(DOOM_CORE_DIR)/w_wad.c \
	$(DOOM_CORE_DIR)/z_zone.c \
	$(DOOM_DIR)/i_video.c \
	$(DOOM_DIR)/i_system.c \
	$(DOOM_DIR)/i_sound.c \
	$(DOOM_DIR)/i_net.c \
	$(DOOM_DIR)/i_main.c \
	$(DOOM_DIR)/ipo_libc_compat.c

DOOM_OBJS := $(patsubst $(APPS_DIR)/%.c, $(APPS_BUILD)/%.o, $(DOOM_SRCS))

DOOM_CFLAGS := -m32 -O2 \
	-std=gnu89 \
	-ffreestanding \
	-fno-pic -fno-pie \
	-fno-builtin \
	-nostdlib -nostartfiles \
	-DIPO_APP \
	-DNORMALUNIX \
	-I$(APPS_DIR)/include \
	-Isrc/include \
	-I$(DOOM_DIR)/include \
	-I$(DOOM_CORE_DIR) \
	-I$(DOOM_DIR) \
	-include $(DOOM_DIR)/doom_compat.h \
	-Wno-unused-variable \
	-Wno-unused-function \
	-Wno-implicit-function-declaration \
	-Wno-implicit-int

$(APPS_BUILD)/doom/%.o: $(APPS_DIR)/doom/%.c
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ $(DOOM_CFLAGS)

$(APPS_BUILD)/doom/doom.bin: $(DOOM_OBJS) $(APP_ENTRY_OBJ) $(LIB_A)
	@mkdir -p $(dir $@)
	@echo "[applications] Building DOOM: $(DOOM_DIR) → $@"
	@$(CC) $(DOOM_CFLAGS) -Wl,-T,$(APPS_DIR)/app.ld \
		$(APP_ENTRY_OBJ) $(DOOM_OBJS) \
		-Wl,--allow-multiple-definition \
		-Wl,--start-group build/lib/libc.a build/lib/libm.a $(LIB_A) -lgcc -Wl,--end-group -o $@.elf -nostdlib -nostartfiles 2>&1 | grep -v "PIE\|relocation" || true
	@$(OBJCOPY) --set-section-flags .bss=alloc,load,contents \
		-j .text -j .rodata -j .data -j .bss -O binary $@.elf $@
	@rm -f $@.elf
	@SIZE=$$(stat -c%s "$@" 2>/dev/null || stat -f%z "$@" 2>/dev/null); \
	echo "[applications] ✓ Created: $@ ($$SIZE bytes)"


# Rule: Compile .c to object file
$(APPS_BUILD)/%.o: $(APPS_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ -std=gnu11 $(APPS_CFLAGS)

# Rule: Create executable binary from object file
$(APPS_BUILD)/%.bin: $(APPS_BUILD)/%.o $(APP_ENTRY_OBJ) $(APP_PRINTF_OBJ) $(APP_KMALLOC_OBJ) $(APP_WM_OBJ) $(LIB_A)
	@mkdir -p $(dir $@)
	@echo "[applications] Building: $*.c → $@"
	@$(CC) $(APPS_CFLAGS) -Wl,-T,$(APPS_DIR)/app.ld \
		$(APP_ENTRY_OBJ) $< $(APP_PRINTF_OBJ) $(APP_KMALLOC_OBJ) $(APP_WM_OBJ) -Wl,--start-group $(LIB_A) -lgcc -Wl,--end-group -o $@.elf -nostdlib -nostartfiles 2>&1 | grep -v "PIE\|relocation" || true
	@$(OBJCOPY) --set-section-flags .bss=alloc,load,contents \
		-j .text -j .rodata -j .data -j .bss -O binary $@.elf $@
	@rm -f $@.elf
	@SIZE=$$(stat -c%s "$@" 2>/dev/null || stat -f%z "$@" 2>/dev/null); \
	echo "[applications] ✓ Created: $@ ($$SIZE bytes)"

# Clean applications
clean-applications:
	@echo "[applications] Cleaning built applications..."
	@rm -rf $(APPS_BUILD) build/apps

clean-apps: clean-applications

clean: clean-applications

