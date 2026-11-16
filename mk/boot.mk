.PHONY: boot patch-config
boot: $(BOOT_BIN)

$(BUILD)/boot/config.inc: $(SRC)/boot/config.inc.in $(KERNEL_BIN)
	@mkdir -p $(dir $@)
	@ksize=$$(stat -c %s $(KERNEL_BIN)); \
	ksec=$$(( ($$ksize + 511) / 512 )); \
	echo "KERNEL: $$ksize bytes -> $$ksec sectors"; \
	new_content=$$(sed -e "s/@KERNEL_SECTORS@/$$ksec/g" $<); \
	if [ -f "$@" ] && [ "$$(cat "$@")" = "$$new_content" ]; then \
		echo "Config already up to date"; \
	else \
		printf "%s\n" "$$new_content" > $@; \
	fi

patch-config: $(BUILD)/boot/config.inc
	@echo "Config updated"

$(BOOT_BIN): $(SRC)/boot/boot.asm $(BUILD)/boot/config.inc
	@mkdir -p $(dir $@)
	$(ASM) -f bin -I$(BUILD)/boot $(SRC)/boot/boot.asm -o $@
