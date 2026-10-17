.PHONY: lib clean-lib

USER_APP_API_SRCS := $(wildcard $(APPS_DIR)/lib/*.c) $(wildcard $(APPS_DIR)/lib/gl/*.c)
USER_APP_API_OBJS := $(patsubst $(APPS_DIR)/lib/%.c, $(LIB_BUILD_DIR)/app_api/%.o, $(USER_APP_API_SRCS))

APP_LIB_A     := $(LIB_BUILD_DIR)/libapp.a
APP_API_LIB_A := $(LIB_BUILD_DIR)/libapp_api.a

USER_LIBC_OBJS := \
	$(LIB_BUILD_DIR)/libc/string.o \
	$(LIB_BUILD_DIR)/libc/unistd.o \
	$(LIB_BUILD_DIR)/libc/stdlib.o \
	$(LIB_BUILD_DIR)/libc/stdio.o \
	$(LIB_BUILD_DIR)/libc/sys.o \
	$(LIB_BUILD_DIR)/libc/init.o \
	$(LIB_BUILD_DIR)/libc/setjmp.o \
	$(LIB_BUILD_DIR)/libc/math.o

USER_LIBM_OBJS := \
	$(LIB_BUILD_DIR)/libc/math.o

USER_LIBCXX_OBJS := \
	$(LIB_BUILD_DIR)/libc/cxx.o

lib: $(LIB_A) $(USER_LIBC_A) $(USER_LIBM_A) $(USER_LIBCXX_A) $(APP_LIB_A) $(APP_API_LIB_A) $(USER_CRT0) $(USER_CRTI) $(USER_CRTN) build/lib/crt1.o build/lib/libtcc1.a

$(USER_CRT0): $(APPS_DIR)/lib/crt/crt0.s
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ -m32

$(USER_CRTI): $(APPS_DIR)/lib/crt/crti.s
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ -m32

$(USER_CRTN): $(APPS_DIR)/lib/crt/crtn.s
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ -m32

build/lib/crt1.o: $(APPS_DIR)/tcc/crt/crt1.s
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ -m32

build/lib/libtcc1.o: $(APPS_DIR)/tcc/crt/libtcc1.c
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ -m32 -std=gnu11 -ffreestanding -fno-builtin -fno-pic -fno-pie

build/lib/libtcc1.a: build/lib/libtcc1.o
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $<

$(LIB_BUILD_DIR)/libc/setjmp.o: $(APPS_DIR)/lib/libc/setjmp.s
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ -m32

$(LIB_BUILD_DIR)/libc/%.o: $(APPS_DIR)/lib/libc/%.c
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ -m32 -std=gnu11 -ffreestanding -fno-builtin -fno-pic -fno-pie -I$(APPS_DIR)/include

$(LIB_BUILD_DIR)/libc/cxx.o: $(APPS_DIR)/lib/libc/cxx.cpp
	@mkdir -p $(dir $@)
	g++ -c $< -o $@ -m32 -nostdinc -I$(APPS_DIR)/include -I$(APPS_DIR)/include/c++ -ffreestanding -fno-builtin -fno-rtti -fno-exceptions -fno-pic -fno-pie

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

$(LIB_BUILD_DIR)/app_api/%.o: $(APPS_DIR)/lib/%.c
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@ -m32 -std=gnu11 -ffreestanding -fno-builtin -fno-pic -fno-pie -I$(APPS_DIR)/include

$(APP_LIB_A): $(USER_APP_API_OBJS)
	@mkdir -p $(dir $@)
	rm -f $@
	$(AR) rcs $@ $^

$(APP_API_LIB_A): $(APP_LIB_A)
	@mkdir -p $(dir $@)
	cp -f $< $@

clean-lib:
	rm -rf $(LIB_BUILD_DIR)
	rm -f $(LIB_A) $(APP_LIB_A) $(APP_API_LIB_A) $(USER_LIBC_A) $(USER_LIBM_A) $(USER_LIBCXX_A)
