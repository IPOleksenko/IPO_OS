.PHONY: image
image: $(OS_IMAGE)

$(OS_IMAGE): $(BOOT_BIN) $(KERNEL_BIN)
	cat $^ > $@
