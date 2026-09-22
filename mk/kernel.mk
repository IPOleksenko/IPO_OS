.PHONY: kernel
kernel: $(KERNEL_BIN)

$(BUILD)/kernel/entry32.o: src/kernel/entry32.asm
	@mkdir -p $(dir $@)
	$(ASM) $(ASM_ELF_FLAGS) $< -o $@

$(BUILD)/kernel/kernel32.o: src/kernel/kernel32.c
	@mkdir -p $(dir $@)
	$(CC) $(LIB_CFLAGS) -c $< -o $@

# C sources
$(BUILD)/kernel/%.o: $(SRC)/%.c
	@mkdir -p $(dir $@)
	$(CC) -MD -c $< -o $@ -std=gnu11 $(LIB_CFLAGS)

# ASM sources
$(BUILD)/kernel/%.o: $(SRC)/%.asm
	@mkdir -p $(dir $@)
	$(ASM) $(ASM_ELF_FLAGS) $< -o $@

$(BUILD)/kernel/%.o: $(SRC)/%.s
	@mkdir -p $(dir $@)
	$(CC) -MD -c $< -o $@ $(LIB_CFLAGS)

$(BUILD)/kernel/%.o: $(SRC)/%.S
	@mkdir -p $(dir $@)
	$(CC) -MD -c $< -o $@ $(LIB_CFLAGS)

$(KERNEL_BIN): $(BUILD)/kernel/entry32.o $(BUILD)/kernel/kernel32.o $(LIB_OBJS)
	@mkdir -p $(dir $@)
	ld -m elf_i386 -T src/kernel/linker.ld -nostdlib --oformat elf32-i386 -o $(KERNEL_ELF) $^ $(LIBGCC)
	$(OBJCOPY) -O binary $(KERNEL_ELF) $@
	@size=$$(stat -c %s $@); \
	padded=$$(( ($$size + 511) / 512 * 512 )); \
	truncate -s $$padded $@