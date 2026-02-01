.PHONY: image
image: $(OS_IMAGE) disks

$(OS_IMAGE): $(BOOT_BIN) $(KERNEL_BIN)
	cat $^ > $@

# Create a 10MB IMG disk (raw format)
build/disk.img:
	mkdir -p build
	dd if=/dev/zero of=$@ bs=1M count=10
	@echo "Created 10MB virtual disk: $@"

# Create a 5MB ISO disk (for testing)
build/disk.iso:
	mkdir -p build
	dd if=/dev/zero of=$@ bs=1M count=5
	@echo "Created 5MB ISO disk: $@"

.PHONY: disks
disks: build/disk.img build/disk.iso

.PHONY: clean-disks
clean-disks:
	rm -f build/disk.img build/disk.iso
