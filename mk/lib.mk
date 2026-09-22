.PHONY: lib clean-lib

lib: $(LIB_A) $(USER_LIBC_A) $(USER_LIBM_A) $(USER_LIBCXX_A) $(USER_CRT0) $(USER_CRTI) $(USER_CRTN) build/lib/crt1.o build/lib/libtcc1.a

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

$(USER_CRT0): $(SRC)/userland/crt/crt0.s
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ -m32

$(USER_CRTI): $(SRC)/userland/crt/crti.s
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ -m32

$(USER_CRTN): $(SRC)/userland/crt/crtn.s
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ -m32

build/lib/crt1.o: $(APPS_DIR)/tcc/crt/crt1.s
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ -m32

build/lib/libtcc1.o: $(APPS_DIR)/tcc/crt/libtcc1.c
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ -m32 -std=gnu11 -ffreestanding -fno-builtin

build/lib/libtcc1.a: build/lib/libtcc1.o
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $<

$(LIB_BUILD_DIR)/libc/setjmp.o: $(SRC)/userland/libc/setjmp.s
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ -m32

$(LIB_BUILD_DIR)/libc/%.o: $(SRC)/userland/libc/%.c
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ -m32 -std=gnu11 -ffreestanding -fno-builtin -I$(APPS_DIR)/include -I$(SRC)/include

$(LIB_BUILD_DIR)/libc/cxx.o: $(SRC)/userland/libc/cxx.cpp
	@mkdir -p $(dir $@)
	g++ -c $< -o $@ -m32 -nostdinc -I$(APPS_DIR)/include -I$(APPS_DIR)/include/c++ -ffreestanding -fno-builtin -fno-rtti -fno-exceptions

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

clean-lib:
	rm -rf $(LIB_BUILD_DIR)
	rm -f $(LIB_A) $(USER_LIBC_A) $(USER_LIBM_A) $(USER_LIBCXX_A)
