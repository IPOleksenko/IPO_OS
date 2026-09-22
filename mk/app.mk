.PHONY: apps clean-apps

APPS_DIR     := applications
APPS_BUILD   := build/applications
APPS_SRCS_RAW := $(shell find $(APPS_DIR) -maxdepth 2 -name "*.c" -type f)
GENERIC_APP_SRCS := $(filter-out $(APPS_DIR)/tcc/% $(APPS_DIR)/lua/% $(APPS_DIR)/micropython/% $(APPS_DIR)/nasm/%, $(APPS_SRCS_RAW))
APPS_BINS    := $(patsubst $(APPS_DIR)/%.c, $(APPS_BUILD)/%.bin, $(GENERIC_APP_SRCS)) $(APPS_BUILD)/tcc/tcc.bin
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
apps: $(APPS_BINS)

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
	@echo "[apps] Building official TinyCC: $(APPS_DIR)/tcc/tcc.c → $@"
	@$(CC) $(APPS_CFLAGS) -Wl,-T,$(APPS_DIR)/app.ld \
		$(APP_ENTRY_OBJ) $(APPS_BUILD)/tcc/tcc.o $(APPS_BUILD)/tcc/libc_shim.o $(APPS_BUILD)/tcc/setjmp.o $(APP_PRINTF_OBJ) $(APP_KMALLOC_OBJ) -Wl,--start-group $(LIB_A) -lgcc -Wl,--end-group -o $@.elf -nostdlib -nostartfiles 2>&1 | grep -v "PIE\|relocation" || true
	@$(OBJCOPY) --set-section-flags .bss=alloc,load,contents \
		-j .text -j .rodata -j .data -j .bss -O binary $@.elf $@
	@rm -f $@.elf
	@SIZE=$$(stat -c%s "$@" 2>/dev/null || stat -f%z "$@" 2>/dev/null); \
	echo "[apps] ✓ Created: $@ ($$SIZE bytes)"

# Rule: Compile .c to object file
$(APPS_BUILD)/%.o: $(APPS_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ -std=gnu11 $(APPS_CFLAGS)

# Rule: Create executable binary from object file
$(APPS_BUILD)/%.bin: $(APPS_BUILD)/%.o $(APP_ENTRY_OBJ) $(APP_PRINTF_OBJ) $(APP_KMALLOC_OBJ) $(APP_WM_OBJ) $(LIB_A)
	@mkdir -p $(dir $@)
	@echo "[apps] Building: $*.c → $@"
	@$(CC) $(APPS_CFLAGS) -Wl,-T,$(APPS_DIR)/app.ld \
		$(APP_ENTRY_OBJ) $< $(APP_PRINTF_OBJ) $(APP_KMALLOC_OBJ) $(APP_WM_OBJ) -Wl,--start-group $(LIB_A) -lgcc -Wl,--end-group -o $@.elf -nostdlib -nostartfiles 2>&1 | grep -v "PIE\|relocation" || true
	@$(OBJCOPY) --set-section-flags .bss=alloc,load,contents \
		-j .text -j .rodata -j .data -j .bss -O binary $@.elf $@
	@rm -f $@.elf
	@SIZE=$$(stat -c%s "$@" 2>/dev/null || stat -f%z "$@" 2>/dev/null); \
	echo "[apps] ✓ Created: $@ ($$SIZE bytes)"

# Clean applications
clean-apps:
	@echo "[apps] Cleaning built applications..."
	@rm -rf $(APPS_BUILD) build/apps

# Include in main clean target
clean: clean-apps
