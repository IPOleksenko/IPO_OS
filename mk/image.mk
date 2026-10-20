.PHONY: image
image: $(OS_IMAGE) disks live-image

$(OS_IMAGE): $(BOOT_BIN) $(KERNEL_BIN)
	cat $^ > $@
	@size=$$(stat -c %s $@); \
	padded=$$(( ($$size + 511) / 512 * 512 )); \
	truncate -s $$padded $@

# Create a 512MB IMG disk (raw format)
build/disk.img:
	mkdir -p build
	dd if=/dev/zero of=$@ bs=1M count=512
	@echo "Created 512MB virtual disk: $@"

# Create a 5MB ISO disk
build/disk.iso:
	mkdir -p build
	dd if=/dev/zero of=$@ bs=1M count=5
	@echo "Created 5MB ISO disk: $@"

.PHONY: format-disk
format-disk: $(BOOT_BIN) $(KERNEL_BIN) build/disk.img
	python3 tools/format_disk_via_os.py --boot $(BOOT_BIN) --kernel $(KERNEL_BIN) --disk build/disk.img

.PHONY: disks
disks: $(BOOT_BIN) $(KERNEL_BIN) build/disk.img build/disk.iso toolchain format-disk
	python3 tools/load_applications.py

.PHONY: live-image
live-image: $(OS_IMAGE) disks
	python3 tools/create_live_image.py

.PHONY: clean-disks
clean-disks:
	rm -f build/disk.img build/disk.iso
