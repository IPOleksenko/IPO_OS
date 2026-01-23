$(BUILD)/entry.o: src/kernel/entry.asm
	mkdir -p $(dir $@)
	$(ASM) $(ASM_ELF_FLAGS) $< -o $@

$(BUILD)/kernel_c.o: src/kernel/kernel.c
	mkdir -p $(dir $@)
	gcc -m16 -ffreestanding -c $< -o $@

$(KERNEL_BIN): $(BUILD)/entry.o $(BUILD)/kernel_c.o
	ld -m elf_i386 -T src/kernel/linker.ld -nostdlib --oformat elf32-i386 -o $(BUILD)/kernel.elf $^
	$(OBJCOPY) -O binary $(BUILD)/kernel.elf $@
