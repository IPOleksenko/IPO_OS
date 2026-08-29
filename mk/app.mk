.PHONY: apps clean-apps

APPS_DIR     := apps
APPS_BUILD   := build/apps
APPS_SRCS    := $(shell find $(APPS_DIR) -maxdepth 2 -name "*.c" -type f)
APPS_BINS    := $(patsubst $(APPS_DIR)/%.c, $(APPS_BUILD)/%.bin, $(APPS_SRCS))

# Applications use a fixed flat image address. The loader copies the image to
# this address, so all absolute references remain valid without relocations.
APPS_CFLAGS := -m32 \
	-ffreestanding \
	-fno-pic -fno-pie \
	-fno-builtin \
	-nostdlib -nostartfiles \
	-Ilib/h

# Build all applications
apps: $(APPS_BINS)

# Rule: Compile .c to object file
$(APPS_BUILD)/%.o: $(APPS_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ -std=gnu11 $(APPS_CFLAGS)

# Rule: Create IPO_BINARY executable from object file
# This rule extracts code sections and creates IPO_BINARY header entirely in Make
$(APPS_BUILD)/%.bin: $(APPS_BUILD)/%.o $(LIB_A)
	@mkdir -p $(dir $@)
	@echo "[apps] Building: $*.c → $@"
	
	@# Link with kernel library and runtime libs (suppress PIE warnings)
	@$(CC) $(APPS_CFLAGS) -Wl,--entry=main -Wl,-T,apps/app.ld \
		$< $(LIB_A) -lgcc -o $@.elf -nostdlib -nostartfiles 2>&1 | grep -v "PIE\|relocation" || true
	
	@# Extract only program sections (not dynamic/debug)
	@$(OBJCOPY) -j .text -j .rodata -j .data -O binary $@.elf $@.code
	
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

