patch-config: $(STAGE2_BIN) $(STAGE1_CFG_OUT)
	@size=$$(stat -c %s $(STAGE2_BIN)); \
	sectors=$$(( ($$size + 511) / 512 )); \
	echo "STAGE2: $$size bytes -> $$sectors sectors"; \
	sed -i "s/@STAGE2_SECTORS@/$$sectors/g" $(STAGE1_CFG_OUT)

$(STAGE1_BIN): \
	src/boot/stage1/stage1.asm \
	$(BOOT_CFG_OUT) \
	$(STAGE1_CFG_OUT) \
	patch-config
	mkdir -p $(dir $@)
	$(ASM) $(ASM_BIN_FLAGS) $< -o $@
