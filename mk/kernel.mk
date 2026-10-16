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

KERNEL_PRE_ELF := $(BUILD)/kernel/kernel_pre.elf
KERNEL_KSYMS_C := $(BUILD)/kernel/kernel_syms_autogen.c
KERNEL_KSYMS_O := $(BUILD)/kernel/kernel_syms_autogen.o

$(KERNEL_PRE_ELF): $(BUILD)/kernel/entry32.o $(BUILD)/kernel/kernel32.o $(LIB_OBJS)
	@mkdir -p $(dir $@)
	ld -m elf_i386 -T src/kernel/linker.ld -nostdlib --oformat elf32-i386 -o $@ $^ $(LIBGCC)

$(KERNEL_KSYMS_C): $(KERNEL_PRE_ELF)
	@mkdir -p $(dir $@)
	python3 tools/generate_ksyms.py -e $< -o $@

$(KERNEL_KSYMS_O): $(KERNEL_KSYMS_C)
	@mkdir -p $(dir $@)
	$(CC) $(LIB_CFLAGS) -std=gnu11 -c $< -o $@

$(KERNEL_BIN): $(BUILD)/kernel/entry32.o $(BUILD)/kernel/kernel32.o $(LIB_OBJS) $(KERNEL_KSYMS_O)
	@mkdir -p $(dir $@)
	ld -m elf_i386 -T src/kernel/linker.ld -nostdlib --oformat elf32-i386 -o $(KERNEL_ELF) $^ $(LIBGCC)
	python3 tools/generate_ksyms.py -e $(KERNEL_ELF) -o $(KERNEL_KSYMS_C)
	$(CC) $(LIB_CFLAGS) -std=gnu11 -c $(KERNEL_KSYMS_C) -o $(KERNEL_KSYMS_O)
	ld -m elf_i386 -T src/kernel/linker.ld -nostdlib --oformat elf32-i386 -o $(KERNEL_ELF) $^ $(LIBGCC)
	$(OBJCOPY) -O binary $(KERNEL_ELF) $@
	@size=$$(stat -c %s $@); \
	padded=$$(( ($$size + 511) / 512 * 512 )); \
	truncate -s $$padded $@

-include $(shell find $(BUILD)/kernel -name "*.d" 2>/dev/null)