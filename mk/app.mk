.PHONY: apps clean-apps

APPS_DIR     := apps
APPS_BUILD   := build/apps
APPS_SRCS    := $(shell find $(APPS_DIR) -maxdepth 2 -name "*.c" -type f)
APPS_BINS    := $(patsubst $(APPS_DIR)/%.c, $(APPS_BUILD)/%.bin, $(APPS_SRCS))
APP_PRINTF_OBJ := $(APPS_BUILD)/app_printf.o

# Applications are compiled as Position Independent Code (PIC).
# This allows them to run from any memory address dynamically allocated
APPS_CFLAGS := -m32 \
	-ffreestanding \
	-fPIC \
	-fno-builtin \
	-nostdlib -nostartfiles \
	-Ilib/h

# Build all applications
apps: $(APPS_BINS)

$(APP_PRINTF_OBJ): lib/c/stdio/printf.c
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ -std=gnu11 $(APPS_CFLAGS) -DIPO_APP

# Rule: Compile .c to object file
$(APPS_BUILD)/%.o: $(APPS_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ -std=gnu11 $(APPS_CFLAGS)

# Rule: Create IPO_BINARY executable from object file
# This rule extracts code sections and creates IPO_BINARY header entirely in Make
$(APPS_BUILD)/%.bin: $(APPS_BUILD)/%.o $(APP_PRINTF_OBJ) $(LIB_A)
	@mkdir -p $(dir $@)
	@echo "[apps] Building: $*.c → $@"
	
	@# Link with kernel library and runtime libs (suppress PIE warnings)
	@$(CC) $(APPS_CFLAGS) -Wl,--entry=main -Wl,-T,apps/app.ld \
		$< $(APP_PRINTF_OBJ) $(LIB_A) -lgcc -o $@.elf -nostdlib -nostartfiles 2>&1 | grep -v "PIE\|relocation" || true
	
	@# Extract only program sections (not dynamic/debug)
	@$(OBJCOPY) --set-section-flags .bss=alloc,load,contents \
		-j .text -j .rodata -j .data -j .bss -O binary $@.elf $@.code
	
	@# Store the linked entry address as an offset in the flat image.
	@ENTRY=$$(nm -n $@.elf | awk '$$3 == "main" { print "0x" $$1; exit }'); \
	python3 tools/gen_header.py $@.code $@.header $$ENTRY
	
	@# Combine header + code
	@cat $@.header $@.code > $@
	
	@# Cleanup
	@rm -f $@.elf $@.code $@.header
	
	@# Show result
	@SIZE=$$(stat -c%s "$@" 2>/dev/null || stat -f%z "$@" 2>/dev/null); \
	echo "[apps] ✓ Created: $@ ($$SIZE bytes)"

# Clean applications
clean-apps:
	@echo "[apps] Cleaning built applications..."
	@rm -rf $(APPS_BUILD)

# Include in main clean target
clean: clean-apps

