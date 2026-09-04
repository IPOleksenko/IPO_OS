.PHONY: lib
lib: $(LIB_A) build/lib/crt1.o build/lib/crti.o build/lib/crtn.o build/lib/libtcc1.a

build/lib/crt1.o: apps/tcc/crt/crt1.s
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ -m32

build/lib/crti.o: apps/tcc/crt/crti.s
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ -m32

build/lib/crtn.o: apps/tcc/crt/crtn.s
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ -m32

build/lib/libtcc1.o: apps/tcc/crt/libtcc1.c
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ -m32 -std=gnu11 -ffreestanding -fno-builtin

build/lib/libtcc1.a: build/lib/libtcc1.o
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $<

$(LIB_A): $(LIB_OBJS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

# C sources
$(LIB_BUILD_DIR)/%.o: $(LIB_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) -MD -c $< -o $@ -std=gnu11 $(LIB_CFLAGS)

# ASM sources
$(LIB_BUILD_DIR)/%.o: $(LIB_DIR)/%.asm
	@mkdir -p $(dir $@)
	$(ASM) $(ASM_ELF_FLAGS) $< -o $@

$(LIB_BUILD_DIR)/%.o: $(LIB_DIR)/%.s
	@mkdir -p $(dir $@)
	$(CC) -MD -c $< -o $@ $(LIB_CFLAGS)

$(LIB_BUILD_DIR)/%.o: $(LIB_DIR)/%.S
	@mkdir -p $(dir $@)
	$(CC) -MD -c $< -o $@ $(LIB_CFLAGS)

.PHONY: clean-lib
clean-lib:
	rm -rf $(LIB_BUILD_DIR)
	rm -f $(LIB_A)
