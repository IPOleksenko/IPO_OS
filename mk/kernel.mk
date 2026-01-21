$(BUILD)/kernel.o: src/kernel/entry.asm
	mkdir -p $(dir $@)
	$(ASM) $(ASM_ELF_FLAGS) $< -o $@

$(KERNEL_ELF): $(BUILD)/kernel.o src/kernel/linker.ld
	$(LD) $(LD_FLAGS) -o $@ $<

$(KERNEL_BIN): $(KERNEL_ELF)
	$(OBJCOPY) -O binary $< $@
