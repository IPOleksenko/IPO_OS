.PHONY: apps clean-apps install-apps

APPS_DIR     := apps
APPS_BUILD   := build/apps
APPS_SRCS    := $(shell find $(APPS_DIR) -maxdepth 2 -name "*.c" -type f)
APPS_BINS    := $(patsubst $(APPS_DIR)/%.c, $(APPS_BUILD)/%.bin, $(APPS_SRCS))

# Compilation flags for applications (PIC for relocation independence)
APPS_CFLAGS := -m32 \
	-ffreestanding \
	-fPIC \
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
	@$(CC) $(APPS_CFLAGS) -Wl,--entry=app_entry \
		$< $(LIB_A) -lgcc -o $@.elf -nostdlib -nostartfiles 2>&1 | grep -v "PIE\|relocation" || true
	
	@# Extract only program sections (not dynamic/debug)
	@$(OBJCOPY) -j .text -j .rodata -j .data -O binary $@.elf $@.code
	
	@# Create IPO_BINARY header (20 bytes)
	@python3 tools/gen_header.py $@.code $@.header
	
	@# Combine header + code
	@cat $@.header $@.code > $@
	
	@# Cleanup
	@rm -f $@.elf $@.code $@.header
	
	@# Show result
	@SIZE=$$(stat -c%s "$@" 2>/dev/null || stat -f%z "$@" 2>/dev/null); \
	echo "[apps] ✓ Created: $@ ($$SIZE bytes)"

# Install applications to filesystem image
install-apps: apps
	@echo "[apps] Applications built successfully:"
	@for app in $(APPS_BINS); do \
		if [ -f "$$app" ]; then \
			SIZE=$$(stat -c%s "$$app" 2>/dev/null || stat -f%z "$$app" 2>/dev/null); \
			echo "  $$app ($$SIZE bytes)"; \
		fi; \
	done

# Clean applications
clean-apps:
	@echo "[apps] Cleaning built applications..."
	@rm -rf $(APPS_BUILD)

# Include in main clean target
clean: clean-apps

