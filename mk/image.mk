# ==============================================================================
# IPO_OS - OS Image & Virtual Disks
# ==============================================================================

.PHONY: image disks clean-disks

image: $(OS_IMAGE) disks

$(OS_IMAGE): $(BOOT_BIN) $(KERNEL_BIN)
	@mkdir -p $(dir $@)
	cat $^ > $@
	@size=$$(stat -c %s $@); \
	padded=$$(( ($$size + 511) / 512 * 512 )); \
	truncate -s $$padded $@

# Create a 512MB raw virtual disk if missing
$(DISK_IMG):
	@mkdir -p $(DISKS_DIR)
	@if [ ! -f "$@" ]; then \
		dd if=/dev/zero of=$@ bs=1M count=512 status=none; \
		echo "[image] Created 512MB virtual disk: $@"; \
	fi

# Create a 5MB ISO disk if missing
$(DISK_ISO):
	@mkdir -p $(DISKS_DIR)
	@if [ ! -f "$@" ]; then \
		dd if=/dev/zero of=$@ bs=1M count=5 status=none; \
		echo "[image] Created 5MB ISO disk: $@"; \
	fi

disks: $(BOOT_BIN) $(KERNEL_BIN) $(DISK_IMG) $(DISK_ISO)

clean-disks:
	rm -rf $(DISKS_DIR)
