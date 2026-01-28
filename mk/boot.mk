.PHONY: boot
boot: $(BOOT_BIN)

$(BUILD)/boot/config.inc: $(SRC)/boot/config.inc.in $(KERNEL_BIN)
	@mkdir -p $(dir $@)
	@size=$$(stat -c %s $(KERNEL_BIN)); \
	sectors=$$(( ($$size + 511) / 512 )); \
	echo "KERNEL: $$size bytes -> $$sectors sectors"; \
	sed "s/@KERNEL_SECTORS@/$$sectors/g" $< > $@

patch-config: $(KERNEL_BIN) $(BUILD)/boot/config.inc
	@echo "Config updated"

$(BOOT_BIN): src/boot/boot.asm $(KERNEL_BIN) $(BUILD)/boot/config.inc
	mkdir -p $(dir $@)
	nasm -f bin -I$(BUILD)/boot src/boot/boot.asm -o $@
