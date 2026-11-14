# OS binary and virtual disk installation

.PHONY: image disk clean-disk

image: $(OS_BIN)

# Compile flat OS binary (MBR bootloader + kernel) in build/IPO_OS.bin
$(OS_BIN): $(BOOT_BIN) $(KERNEL_BIN)
	@mkdir -p $(dir $@)
	cat $^ > $@
	@size=$$(stat -c %s $@); \
	padded=$$(( ($$size + 511) / 512 * 512 )); \
	req_sectors=$$(grep 'KERNEL_SECTORS' $(BUILD)/boot/config.inc 2>/dev/null | awk '{print $$3}'); \
	partition_start_lba=$$(grep 'MBR_PARTITION_START_LBA' $(BUILD)/boot/config.inc | awk '{print $$3}'); \
	if [ -z "$$partition_start_lba" ]; then \
		echo "[image] MBR partition geometry missing from boot configuration" >&2; exit 1; \
	fi; \
	if [ -n "$$req_sectors" ]; then \
		kernel_min_size=$$(( ($$req_sectors + 1) * 512 )); \
		if [ "$$kernel_min_size" -gt "$$padded" ]; then padded=$$kernel_min_size; fi; \
	fi; \
	max_os_size=$$(( 2030 * 512 )); \
	if [ "$$padded" -gt "$$max_os_size" ]; then \
		echo "[image] ERROR: OS binary ($$padded bytes) exceeds pre-partition boot area ($$max_os_size bytes)" >&2; \
		exit 1; \
	fi; \
	truncate -s $$padded $@
	@echo "[image] Compiled OS binary: $@ ($$(stat -c %s $@) bytes)"

# Create a 512 MiB raw virtual disk in build/disk/disk.img if missing,
# and install (transfer) build/IPO_OS.bin onto the disk boot area.
$(DISK_IMG): $(OS_BIN)
	@mkdir -p $(DISK_DIR)
	@if [ ! -f "$@" ]; then \
		dd if=/dev/zero of=$@ bs=1M count=512 status=none; \
		echo "[disk] Created 512 MiB virtual disk: $@"; \
	fi
	@dd if=$(OS_BIN) of=$@ bs=512 conv=notrunc status=none
	@echo "[disk] Installed $(OS_BIN) -> $@"

disk: $(DISK_IMG)

clean-disk:
	rm -rf $(DISK_DIR)
