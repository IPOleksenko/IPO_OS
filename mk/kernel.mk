# Kernel build rules

.PHONY: kernel
kernel: $(KERNEL_BIN)

$(BUILD)/kernel/entry.o: $(SRC)/kernel/entry.asm
	@mkdir -p $(dir $@)
	$(ASM) $(ASM_ELF_FLAGS) $< -o $@

$(BUILD)/kernel/kernel.o: $(SRC)/kernel/kernel.c
	@mkdir -p $(dir $@)
	$(CC) $(LIB_CFLAGS) -std=gnu11 -c $< -o $@

$(BUILD)/kernel/%.o: $(SRC)/%.c
	@mkdir -p $(dir $@)
	$(CC) -MD -c $< -o $@ -std=gnu11 $(LIB_CFLAGS)

$(BUILD)/kernel/%.o: $(SRC)/%.asm
	@mkdir -p $(dir $@)
	$(ASM) $(ASM_ELF_FLAGS) $< -o $@

$(BUILD)/kernel/%.o: $(SRC)/%.s
	@mkdir -p $(dir $@)
	$(CC) -MD -c $< -o $@ $(LIB_CFLAGS)

$(BUILD)/kernel/%.o: $(SRC)/%.S
	@mkdir -p $(dir $@)
	$(CC) -MD -c $< -o $@ $(LIB_CFLAGS)

$(BUILD)/kernel/kernel_pre.elf: $(BUILD)/kernel/entry.o $(BUILD)/kernel/kernel.o $(LIB_OBJS)
	@mkdir -p $(dir $@)
	$(LD) -m elf_i386 --oformat elf32-i386 -T $(SRC)/kernel/linker.ld -nostdlib --no-warn-rwx-segments -o $@ $^ $(LIBGCC)

$(BUILD)/kernel/kernel_syms_autogen.c: $(BUILD)/kernel/kernel_pre.elf
	@mkdir -p $(dir $@)
	python3 tools/generate_ksyms.py -e $< -o $@

$(BUILD)/kernel/kernel_syms_autogen.o: $(BUILD)/kernel/kernel_syms_autogen.c
	@mkdir -p $(dir $@)
	$(CC) $(LIB_CFLAGS) -std=gnu11 -c $< -o $@

$(KERNEL_BIN): $(BUILD)/kernel/entry.o $(BUILD)/kernel/kernel.o $(LIB_OBJS) $(BUILD)/kernel/kernel_syms_autogen.o
	@mkdir -p $(dir $@)
	$(LD) -m elf_i386 --oformat elf32-i386 -T $(SRC)/kernel/linker.ld -nostdlib --no-warn-rwx-segments -o $(KERNEL_ELF) $^ $(LIBGCC)
	python3 tools/generate_ksyms.py -e $(KERNEL_ELF) -o $(BUILD)/kernel/kernel_syms_autogen.c
	$(CC) $(LIB_CFLAGS) -std=gnu11 -c $(BUILD)/kernel/kernel_syms_autogen.c -o $(BUILD)/kernel/kernel_syms_autogen.o
	$(LD) -m elf_i386 --oformat elf32-i386 -T $(SRC)/kernel/linker.ld -nostdlib --no-warn-rwx-segments -o $(KERNEL_ELF) $^ $(LIBGCC)
	$(OBJCOPY) -O binary $(KERNEL_ELF) $@
	@size=$$(stat -c %s $@); \
	padded=$$(( ($$size + 511) / 512 * 512 )); \
	truncate -s $$padded $@

-include $(shell find $(BUILD)/kernel -name "*.d" 2>/dev/null)
