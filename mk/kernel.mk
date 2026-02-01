.PHONY: kernel
kernel: $(KERNEL_BIN)

$(BUILD)/kernel/entry32.o: src/kernel/entry32.asm
	mkdir -p $(dir $@)
	$(ASM) $(ASM_ELF_FLAGS) $< -o $@

$(BUILD)/kernel/kernel32.o: src/kernel/kernel32.c
	mkdir -p $(dir $@)
	gcc -m32 -ffreestanding -fno-pic -fno-pie -fno-builtin -nostdlib -nostartfiles -Ilib/h -c $< -o $@

$(KERNEL_BIN): $(BUILD)/kernel/entry32.o $(BUILD)/kernel/kernel32.o $(LIB_OBJS)
	ld -m elf_i386 -T src/kernel/linker.ld -nostdlib --oformat elf32-i386 -o $(KERNEL_ELF) $^ $(LIBGCC)
	$(OBJCOPY) -O binary $(KERNEL_ELF) $@