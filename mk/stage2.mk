
$(STAGE2_BIN): \
	src/boot/stage2/stage2.asm \
	$(BOOT_CFG_OUT) \
	$(STAGE2_CFG_OUT)
	mkdir -p $(dir $@)
	$(ASM) $(ASM_BIN_FLAGS) $< -o $@.tmp
	dd if=$@.tmp of=$@ bs=512 count=1 conv=notrunc
	rm $@.tmp
