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
	req_sectors=$$(grep 'KERNEL_SECTORS' $(BUILD)/boot/config.inc 2>/dev/null | awk '{print $$3}'); \
	partition_start_lba=$$(grep 'MBR_PARTITION_START_LBA' $(BUILD)/boot/config.inc | awk '{print $$3}'); \
	partition_sectors=$$(grep 'MBR_PARTITION_SECTORS' $(BUILD)/boot/config.inc | awk '{print $$3}'); \
	if [ -z "$$partition_start_lba" ] || [ -z "$$partition_sectors" ]; then \
		echo "[image] MBR partition geometry missing from boot configuration" >&2; exit 1; \
	fi; \
	partition_end_lba_exclusive=$$((partition_start_lba + partition_sectors)); \
	min_size=$$((partition_end_lba_exclusive * 512)); \
	if [ -n "$$req_sectors" ]; then \
		kernel_min_size=$$(( ($$req_sectors + 1) * 512 )); \
		if [ "$$kernel_min_size" -gt "$$min_size" ]; then min_size=$$kernel_min_size; fi; \
	fi; \
	if [ "$$min_size" -gt "$$padded" ]; then padded=$$min_size; fi; \
	truncate -s $$padded $@

# Create a 512 MiB raw virtual disk if missing
$(DISK_IMG):
	@mkdir -p $(DISKS_DIR)
	@if [ ! -f "$@" ]; then \
		dd if=/dev/zero of=$@ bs=1M count=512 status=none; \
		echo "[image] Created 512 MiB virtual disk: $@"; \
	fi

# Create a 5 MiB blank raw CD-ROM image if missing
$(DISK_ISO):
	@mkdir -p $(DISKS_DIR)
	@if [ ! -f "$@" ]; then \
		dd if=/dev/zero of=$@ bs=1M count=5 status=none; \
		echo "[image] Created 5 MiB blank raw CD-ROM image: $@"; \
	fi

disks: $(BOOT_BIN) $(KERNEL_BIN) $(DISK_IMG) $(DISK_ISO)

clean-disks:
	rm -rf $(DISKS_DIR)
