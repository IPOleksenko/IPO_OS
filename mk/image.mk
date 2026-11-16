# OS image and disk targets

.PHONY: image disks clean-disks

image: $(OS_BIN)

# Concatenate boot sector and flat kernel binary
$(OS_BIN): $(BOOT_BIN) $(KERNEL_BIN)
	@mkdir -p $(dir $@)
	cat $(BOOT_BIN) $(KERNEL_BIN) > $@
	@echo "[image] Built unified OS image: $@ ($$(stat -c %s $@) bytes)"

# Create a 512 MiB raw disk image if missing and write boot/kernel image
$(DISK_IMG): $(OS_BIN)
	@mkdir -p $(DISK_DIR)
	@if [ ! -f "$@" ]; then \
		dd if=/dev/zero of=$@ bs=1MiB count=512 status=none; \
		echo "[disk] Created 512 MiB virtual disk: $@"; \
	fi
	@dd if=$(OS_BIN) of=$@ bs=512 conv=notrunc status=none
	@echo "[disk] Installed $(OS_BIN) -> $@"

disks: $(DISK_IMG)

clean-disks:
	rm -rf $(DISK_DIR)
