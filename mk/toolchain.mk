.PHONY: toolchain
toolchain: lib build/lib/libgcc.a \
	build/toolchain/gcc.elf \
	build/toolchain/g++.elf \
	build/toolchain/ld.elf \
	build/toolchain/as.elf \
	build/toolchain/ar.elf \
	build/toolchain/ranlib.elf \
	build/toolchain/objcopy.elf \
	build/toolchain/strip.elf \
	build/toolchain/objdump.elf \
	build/toolchain/readelf.elf \
	build/toolchain/nm.elf \
	build/toolchain/lua/lua.elf \
	build/toolchain/lua/luac.elf \
	build/toolchain/nasm.elf \
	build/toolchain/python.elf

build/lib/libgcc.a:
	@mkdir -p build/lib
	cp $$(gcc -m32 -print-libgcc-file-name) $@

build/toolchain/%.o: tools/binutils/%.c
	@mkdir -p $(dir $@)
	gcc -m32 -nostdinc -Iapplications/include -c $< -o $@

build/toolchain/gcc.elf: build/toolchain/gcc.o build/lib/crt0.o build/lib/crti.o build/lib/libc.a build/lib/libm.a build/lib/libgcc.a build/lib/crtn.o src/userland/linker.ld
	@mkdir -p $(dir $@)
	ld -m elf_i386 -T src/userland/linker.ld build/lib/crt0.o build/lib/crti.o $< build/lib/libc.a build/lib/libm.a build/lib/libgcc.a build/lib/crtn.o -o $@

build/toolchain/g++.elf: build/toolchain/gcc.elf
	cp $< $@

build/toolchain/ld.elf: build/toolchain/ld.o build/lib/crt0.o build/lib/crti.o build/lib/libc.a build/lib/libm.a build/lib/libgcc.a build/lib/crtn.o src/userland/linker.ld
	@mkdir -p $(dir $@)
	ld -m elf_i386 -T src/userland/linker.ld build/lib/crt0.o build/lib/crti.o $< build/lib/libc.a build/lib/libm.a build/lib/libgcc.a build/lib/crtn.o -o $@

build/toolchain/as.elf: build/toolchain/as.o build/lib/crt0.o build/lib/crti.o build/lib/libc.a build/lib/libm.a build/lib/libgcc.a build/lib/crtn.o src/userland/linker.ld
	@mkdir -p $(dir $@)
	ld -m elf_i386 -T src/userland/linker.ld build/lib/crt0.o build/lib/crti.o $< build/lib/libc.a build/lib/libm.a build/lib/libgcc.a build/lib/crtn.o -o $@

build/toolchain/ar.elf: build/toolchain/ar.o build/lib/crt0.o build/lib/crti.o build/lib/libc.a build/lib/libm.a build/lib/libgcc.a build/lib/crtn.o src/userland/linker.ld
	@mkdir -p $(dir $@)
	ld -m elf_i386 -T src/userland/linker.ld build/lib/crt0.o build/lib/crti.o $< build/lib/libc.a build/lib/libm.a build/lib/libgcc.a build/lib/crtn.o -o $@

build/toolchain/ranlib.elf: build/toolchain/ar.elf
	cp $< $@

build/toolchain/objcopy.elf: build/toolchain/objcopy.o build/lib/crt0.o build/lib/crti.o build/lib/libc.a build/lib/libm.a build/lib/libgcc.a build/lib/crtn.o src/userland/linker.ld
	@mkdir -p $(dir $@)
	ld -m elf_i386 -T src/userland/linker.ld build/lib/crt0.o build/lib/crti.o $< build/lib/libc.a build/lib/libm.a build/lib/libgcc.a build/lib/crtn.o -o $@

build/toolchain/strip.elf: build/toolchain/objcopy.elf
	cp $< $@

build/toolchain/objdump.elf: build/toolchain/objdump.o build/lib/crt0.o build/lib/crti.o build/lib/libc.a build/lib/libm.a build/lib/libgcc.a build/lib/crtn.o src/userland/linker.ld
	@mkdir -p $(dir $@)
	ld -m elf_i386 -T src/userland/linker.ld build/lib/crt0.o build/lib/crti.o $< build/lib/libc.a build/lib/libm.a build/lib/libgcc.a build/lib/crtn.o -o $@

build/toolchain/readelf.elf: build/toolchain/readelf.o build/lib/crt0.o build/lib/crti.o build/lib/libc.a build/lib/libm.a build/lib/libgcc.a build/lib/crtn.o src/userland/linker.ld
	@mkdir -p $(dir $@)
	ld -m elf_i386 -T src/userland/linker.ld build/lib/crt0.o build/lib/crti.o $< build/lib/libc.a build/lib/libm.a build/lib/libgcc.a build/lib/crtn.o -o $@

build/toolchain/nm.elf: build/toolchain/nm.o build/lib/crt0.o build/lib/crti.o build/lib/libc.a build/lib/libm.a build/lib/libgcc.a build/lib/crtn.o src/userland/linker.ld
	@mkdir -p $(dir $@)
	ld -m elf_i386 -T src/userland/linker.ld build/lib/crt0.o build/lib/crti.o $< build/lib/libc.a build/lib/libm.a build/lib/libgcc.a build/lib/crtn.o -o $@

build/toolchain/lua/lua.elf build/toolchain/lua/luac.elf: build/lib/libc.a build/lib/libm.a build/lib/libgcc.a build/lib/crt0.o build/lib/crti.o build/lib/crtn.o src/userland/linker.ld
	@mkdir -p build/toolchain/lua
	cd applications/lua/src && make clean && make a CC="gcc -m32 -nostdinc -I$(CURDIR)/applications/include" AR="ar rcs" RANLIB="ranlib" MYCFLAGS="-DLUA_USE_POSIX -DLUA_USE_C89 -ffreestanding -fno-builtin"
	gcc -m32 -nostdinc -Iapplications/include -c applications/lua/src/lua.c -o applications/lua/src/lua.o -DLUA_COMPAT_5_3 -DLUA_USE_POSIX -DLUA_USE_C89 -ffreestanding -fno-builtin
	gcc -m32 -nostdinc -Iapplications/include -c applications/lua/src/luac.c -o applications/lua/src/luac.o -DLUA_COMPAT_5_3 -DLUA_USE_POSIX -DLUA_USE_C89 -ffreestanding -fno-builtin
	ld -m elf_i386 -T src/userland/linker.ld build/lib/crt0.o build/lib/crti.o applications/lua/src/lua.o applications/lua/src/liblua.a build/lib/libc.a build/lib/libm.a build/lib/libgcc.a build/lib/crtn.o -o build/toolchain/lua/lua.elf
	ld -m elf_i386 -T src/userland/linker.ld build/lib/crt0.o build/lib/crti.o applications/lua/src/luac.o applications/lua/src/liblua.a build/lib/libc.a build/lib/libm.a build/lib/libgcc.a build/lib/crtn.o -o build/toolchain/lua/luac.elf

build/toolchain/nasm.elf: build/lib/libc.a build/lib/libm.a build/lib/libgcc.a build/lib/crt0.o build/lib/crti.o build/lib/crtn.o src/userland/linker.ld
	@mkdir -p build/toolchain
	@mkdir -p applications/nasm/doc && touch applications/nasm/doc/Makefile.in
	$(MAKE) -C applications/nasm asm/nasm.o
	cd applications/nasm && ld -m elf_i386 -T $(CURDIR)/src/userland/linker.ld $(CURDIR)/build/lib/crt0.o $(CURDIR)/build/lib/crti.o asm/nasm.o libnasm.a $(CURDIR)/build/lib/libc.a $(CURDIR)/build/lib/libm.a $(CURDIR)/build/lib/libgcc.a $(CURDIR)/build/lib/crtn.o -o $(CURDIR)/$@

build/toolchain/python.elf: applications/micropython/micropython_shim.c build/lib/libc.a build/lib/libm.a build/lib/libgcc.a build/lib/crt0.o build/lib/crti.o build/lib/crtn.o src/userland/linker.ld
	@mkdir -p build/toolchain
	$(MAKE) -C applications/micropython/ports/unix VARIANT=minimal CFLAGS_EXTRA="-m32" LDFLAGS_EXTRA="-m32"
	gcc -m32 -c applications/micropython/micropython_shim.c -o build/toolchain/micropython_shim.o -Iapplications/micropython -Iapplications/micropython/ports/unix -Iapplications/micropython/ports/unix/variants/minimal -Iapplications/micropython/ports/unix/build-minimal -Iapplications/micropython/ports/unix/build-minimal/genhdr
	ld -m elf_i386 -T src/userland/linker.ld build/lib/crt0.o build/lib/crti.o $$(find applications/micropython/ports/unix/build-minimal -name '*.o' ! -name 'printf.o' ! -name 'abort_.o' ! -name 'vfs_blockdev.o') build/toolchain/micropython_shim.o build/lib/libc.a build/lib/libm.a build/lib/libgcc.a build/lib/crtn.o -o $@
