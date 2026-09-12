.PHONY: lib
lib: $(LIB_A) build/lib/crt1.o build/lib/crti.o build/lib/crtn.o build/lib/libtcc1.a
lib: $(LIB_A) $(USER_LIBC_A) $(USER_LIBM_A) $(USER_LIBCXX_A) $(USER_CRT0) $(USER_CRTI) $(USER_CRTN) build/lib/crt1.o build/lib/libtcc1.a

build/lib/crt1.o: apps/tcc/crt/crt1.s
USER_LIBC_OBJS := \
	$(LIB_BUILD_DIR)/libc/string.o \
	$(LIB_BUILD_DIR)/libc/unistd.o \
	$(LIB_BUILD_DIR)/libc/stdlib.o \
	$(LIB_BUILD_DIR)/libc/stdio.o \
	$(LIB_BUILD_DIR)/libc/sys.o \
	$(LIB_BUILD_DIR)/libc/init.o \
	$(LIB_BUILD_DIR)/libc/setjmp.o

USER_LIBM_OBJS := \
	$(LIB_BUILD_DIR)/libc/math.o

USER_LIBCXX_OBJS := \
	$(LIB_BUILD_DIR)/libc/cxx.o

$(USER_CRT0): lib/crt/crt0.s
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ -m32

build/lib/crti.o: apps/tcc/crt/crti.s
$(USER_CRTI): lib/crt/crti.s
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ -m32

build/lib/crtn.o: apps/tcc/crt/crtn.s
$(USER_CRTN): lib/crt/crtn.s
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ -m32

build/lib/crt1.o: apps/tcc/crt/crt1.s
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ -m32

build/lib/libtcc1.o: apps/tcc/crt/libtcc1.c
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ -m32 -std=gnu11 -ffreestanding -fno-builtin

build/lib/libtcc1.a: build/lib/libtcc1.o
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $<

$(LIB_BUILD_DIR)/libc/setjmp.o: lib/libc/setjmp.s
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ -m32

$(LIB_BUILD_DIR)/libc/%.o: lib/libc/%.c
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ -m32 -std=gnu11 -ffreestanding -fno-builtin -Iapps/include

$(LIB_BUILD_DIR)/libc/cxx.o: lib/libc/cxx.cpp
	@mkdir -p $(dir $@)
	g++ -c $< -o $@ -m32 -nostdinc -Iapps/include -Iapps/include/c++ -ffreestanding -fno-builtin -fno-rtti -fno-exceptions

$(USER_LIBC_A): $(USER_LIBC_OBJS)
	@mkdir -p $(dir $@)
	rm -f $@
	$(AR) rcs $@ $^

$(USER_LIBM_A): $(USER_LIBM_OBJS)
	@mkdir -p $(dir $@)
	rm -f $@
	$(AR) rcs $@ $^

$(USER_LIBCXX_A): $(USER_LIBCXX_OBJS)
	@mkdir -p $(dir $@)
	rm -f $@
	$(AR) rcs $@ $^

$(LIB_A): $(LIB_OBJS)
	@mkdir -p $(dir $@)
	rm -f $@
	$(AR) rcs $@ $^

# C sources
$(LIB_BUILD_DIR)/%.o: $(LIB_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) -MD -c $< -o $@ -std=gnu11 $(LIB_CFLAGS)

# C++ sources
$(LIB_BUILD_DIR)/%.o: $(LIB_DIR)/%.cpp
	@mkdir -p $(dir $@)
	g++ -m32 -nostdinc++ -Iapps/include/c++ -Iapps/include -c $< -o $@ -fno-exceptions -fno-rtti

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
	rm -f $(LIB_A) $(USER_LIBC_A) $(USER_LIBM_A) $(USER_LIBCXX_A)
