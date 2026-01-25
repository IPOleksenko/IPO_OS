$(BUILD)/entry32.o: src/kernel/entry32.asm
	mkdir -p $(dir $@)
	$(ASM) $(ASM_ELF_FLAGS) $< -o $@

$(BUILD)/kernel32.o: src/kernel/kernel32.c
	mkdir -p $(dir $@)
	gcc -m32 -ffreestanding -fno-builtin -nostdinc -Ilib/h -c $< -o $@

$(KERNEL_BIN): $(BUILD)/entry32.o $(BUILD)/kernel32.o $(LIB_OBJS)
	ld -m elf_i386 -T src/kernel/linker.ld -nostdlib --oformat elf32-i386 -o $(KERNEL_ELF) $^
	$(OBJCOPY) -O binary $(KERNEL_ELF) $@