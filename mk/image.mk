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
build/disk.img:
	@mkdir -p build
	@if [ ! -f "$@" ]; then \
		dd if=/dev/zero of=$@ bs=1M count=512 status=none; \
		echo "[image] Created 512MB virtual disk: $@"; \
	fi

# Create a 5MB ISO disk if missing
build/disk.iso:
	@mkdir -p build
	@if [ ! -f "$@" ]; then \
		dd if=/dev/zero of=$@ bs=1M count=5 status=none; \
		echo "[image] Created 5MB ISO disk: $@"; \
	fi

disks: $(BOOT_BIN) $(KERNEL_BIN) build/disk.img build/disk.iso

clean-disks:
	rm -f build/disk.img build/disk.iso
