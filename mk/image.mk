.PHONY: image
image: $(OS_IMAGE) disks

$(OS_IMAGE): $(BOOT_BIN) $(KERNEL_BIN)
	cat $^ > $@
	@size=$$(stat -c %s $@); \
	padded=$$(( ($$size + 511) / 512 * 512 )); \
	truncate -s $$padded $@

# Create a 32MB IMG disk (raw format)
build/disk.img:
	mkdir -p build
	dd if=/dev/zero of=$@ bs=1M count=32
	@echo "Created 32MB virtual disk: $@"

# Create a 5MB ISO disk (for testing)
build/disk.iso:
	mkdir -p build
	dd if=/dev/zero of=$@ bs=1M count=5
	@echo "Created 5MB ISO disk: $@"

build/system/fonts.bin: tools/build_font_db.py
	python3 tools/build_font_db.py

.PHONY: disks
disks: build/disk.img build/disk.iso build/system/fonts.bin

.PHONY: clean-disks
clean-disks:
	rm -f build/disk.img build/disk.iso
