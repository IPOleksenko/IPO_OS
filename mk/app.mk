.PHONY: apps clean-apps

APPS_DIR     := apps
APPS_BUILD   := build/apps
APPS_SRCS_RAW := $(shell find $(APPS_DIR) -maxdepth 2 -name "*.c" -type f)
GENERIC_APP_SRCS := $(filter-out apps/tcc/%, $(APPS_SRCS_RAW))
APPS_BINS    := $(patsubst $(APPS_DIR)/%.c, $(APPS_BUILD)/%.bin, $(GENERIC_APP_SRCS)) $(APPS_BUILD)/tcc/tcc.bin
APP_ENTRY_OBJ  := $(APPS_BUILD)/entry.o
APP_PRINTF_OBJ := $(APPS_BUILD)/app_printf.o
APP_KMALLOC_OBJ := $(APPS_BUILD)/app_kmalloc.o

# Applications are compiled as Position Independent Code (PIC).
# This allows them to run from any memory address dynamically allocated
APPS_CFLAGS := -m32 \
	-ffreestanding \
	-fno-pic -fno-pie \
	-fno-builtin \
	-nostdlib -nostartfiles \
	-DIPO_APP \
	-Ilib/h

TCC_CFLAGS := -m32 \
	-ffreestanding \
	-fno-pic -fno-pie \
	-fno-builtin \
	-nostdlib -nostartfiles \
	-DIPO_APP \
	-DONE_SOURCE=1 \
	-Iapps/tcc/include \
	-Iapps/tcc \
	-Ilib/h

# Build all applications
apps: $(APPS_BINS)

$(APP_ENTRY_OBJ): apps/entry.s
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ $(APPS_CFLAGS)

$(APP_PRINTF_OBJ): lib/c/stdio/printf.c
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ -std=gnu11 $(APPS_CFLAGS) -DIPO_APP

$(APP_KMALLOC_OBJ): lib/c/memory/kmalloc.c
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ -std=gnu11 $(APPS_CFLAGS) -DIPO_APP

$(APPS_BUILD)/tcc/setjmp.o: apps/tcc/setjmp.s
	@mkdir -p $(dir $@)
	$(ASM) -f elf32 $< -o $@

$(APPS_BUILD)/tcc/libc_shim.o: apps/tcc/libc_shim.c
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ -std=gnu11 $(TCC_CFLAGS)

$(APPS_BUILD)/tcc/tcc.o: apps/tcc/tcc.c apps/tcc/libtcc.c apps/tcc/tcctools.c apps/tcc/tccpp.c apps/tcc/tccgen.c apps/tcc/tccelf.c apps/tcc/tccasm.c apps/tcc/tccrun.c apps/tcc/tccdbg.c apps/tcc/i386-gen.c apps/tcc/i386-link.c apps/tcc/i386-asm.c
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ -std=gnu11 $(TCC_CFLAGS)

$(APPS_BUILD)/tcc/tcc.bin: $(APPS_BUILD)/tcc/tcc.o $(APPS_BUILD)/tcc/libc_shim.o $(APPS_BUILD)/tcc/setjmp.o $(APP_ENTRY_OBJ) $(APP_PRINTF_OBJ) $(APP_KMALLOC_OBJ) $(LIB_A)
	@mkdir -p $(dir $@)
	@echo "[apps] Building official TinyCC: apps/tcc/tcc.c → $@"
	@$(CC) $(APPS_CFLAGS) -Wl,-T,apps/app.ld \
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

# Rule: Create raw flat executable binary from object file
$(APPS_BUILD)/%.bin: $(APPS_BUILD)/%.o $(APP_ENTRY_OBJ) $(APP_PRINTF_OBJ) $(APP_KMALLOC_OBJ) $(LIB_A)
	@mkdir -p $(dir $@)
	@echo "[apps] Building: $*.c → $@"
	
	@# Link entry point first so execution starts at offset 0
	@$(CC) $(APPS_CFLAGS) -Wl,-T,apps/app.ld \
		$(APP_ENTRY_OBJ) $< $(APP_PRINTF_OBJ) $(APP_KMALLOC_OBJ) -Wl,--start-group $(LIB_A) -lgcc -Wl,--end-group -o $@.elf -nostdlib -nostartfiles 2>&1 | grep -v "PIE\|relocation" || true
	
	@# Extract program sections into flat binary
	@$(OBJCOPY) --set-section-flags .bss=alloc,load,contents \
		-j .text -j .rodata -j .data -j .bss -O binary $@.elf $@
	
	@# Cleanup
	@rm -f $@.elf
	
	@# Show result
	@SIZE=$$(stat -c%s "$@" 2>/dev/null || stat -f%z "$@" 2>/dev/null); \
	echo "[apps] ✓ Created: $@ ($$SIZE bytes)"

# Clean applications
clean-apps:
	@echo "[apps] Cleaning built applications..."
	@rm -rf $(APPS_BUILD)

# Include in main clean target
clean: clean-apps

