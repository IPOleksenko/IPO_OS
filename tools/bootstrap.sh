#!/usr/bin/env bash
set -e

echo "================================================================="
echo "       IPO_OS Developer Toolchain Bootstrap Script               "
echo "================================================================="

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

cd "$ROOT_DIR"

echo "[1/7] Verifying host environment and prerequisites..."
for tool in gcc g++ ld ar make python3; do
    if ! command -v "$tool" &>/dev/null; then
        echo "Error: required host tool '$tool' not found."
        exit 1
    fi
done
echo "Host prerequisites verified."

echo "[2/7] Building C library and userspace runtime..."
mkdir -p build/lib build/toolchain build/toolchain/lua
make lib
cp "$(gcc -m32 -print-libgcc-file-name)" build/lib/libgcc.a

echo "[3/7] Building Binutils & GCC compiler drivers for IPO_OS..."
# GCC & G++
gcc -m32 -nostdinc -Iapps/include -c tools/binutils/gcc.c -o build/toolchain/gcc.o
ld -m elf_i386 -T lib/linker.ld build/lib/crt0.o build/lib/crti.o build/toolchain/gcc.o build/lib/libc.a build/lib/libm.a build/lib/libgcc.a build/lib/crtn.o -o build/toolchain/gcc.elf
cp build/toolchain/gcc.elf build/toolchain/g++.elf

# Linker (ld)
gcc -m32 -nostdinc -Iapps/include -c tools/binutils/ld.c -o build/toolchain/ld.o
ld -m elf_i386 -T lib/linker.ld build/lib/crt0.o build/lib/crti.o build/toolchain/ld.o build/lib/libc.a build/lib/libm.a build/lib/libgcc.a build/lib/crtn.o -o build/toolchain/ld.elf

# Assembler (as)
gcc -m32 -nostdinc -Iapps/include -c tools/binutils/as.c -o build/toolchain/as.o
ld -m elf_i386 -T lib/linker.ld build/lib/crt0.o build/lib/crti.o build/toolchain/as.o build/lib/libc.a build/lib/libm.a build/lib/libgcc.a build/lib/crtn.o -o build/toolchain/as.elf

# Archive manager (ar & ranlib)
gcc -m32 -nostdinc -Iapps/include -c tools/binutils/ar.c -o build/toolchain/ar.o
ld -m elf_i386 -T lib/linker.ld build/lib/crt0.o build/lib/crti.o build/toolchain/ar.o build/lib/libc.a build/lib/libm.a build/lib/libgcc.a build/lib/crtn.o -o build/toolchain/ar.elf
cp build/toolchain/ar.elf build/toolchain/ranlib.elf

# Object copy & strip (objcopy & strip)
gcc -m32 -nostdinc -Iapps/include -c tools/binutils/objcopy.c -o build/toolchain/objcopy.o
ld -m elf_i386 -T lib/linker.ld build/lib/crt0.o build/lib/crti.o build/toolchain/objcopy.o build/lib/libc.a build/lib/libm.a build/lib/libgcc.a build/lib/crtn.o -o build/toolchain/objcopy.elf
cp build/toolchain/objcopy.elf build/toolchain/strip.elf

# Object dumper (objdump)
gcc -m32 -nostdinc -Iapps/include -c tools/binutils/objdump.c -o build/toolchain/objdump.o
ld -m elf_i386 -T lib/linker.ld build/lib/crt0.o build/lib/crti.o build/toolchain/objdump.o build/lib/libc.a build/lib/libm.a build/lib/libgcc.a build/lib/crtn.o -o build/toolchain/objdump.elf

# Readelf
gcc -m32 -nostdinc -Iapps/include -c tools/binutils/readelf.c -o build/toolchain/readelf.o
ld -m elf_i386 -T lib/linker.ld build/lib/crt0.o build/lib/crti.o build/toolchain/readelf.o build/lib/libc.a build/lib/libm.a build/lib/libgcc.a build/lib/crtn.o -o build/toolchain/readelf.elf

# Symbol listing (nm)
gcc -m32 -nostdinc -Iapps/include -c tools/binutils/nm.c -o build/toolchain/nm.o
ld -m elf_i386 -T lib/linker.ld build/lib/crt0.o build/lib/crti.o build/toolchain/nm.o build/lib/libc.a build/lib/libm.a build/lib/libgcc.a build/lib/crtn.o -o build/toolchain/nm.elf

echo "[4/7] Building language runtimes (Lua, Python, NASM)..."
if [ ! -f "build/toolchain/lua/lua.elf" ] || [ ! -f "build/toolchain/lua/luac.elf" ]; then
    echo "Building Lua 5.4.7..."
    cd toolchains/lua-5.4.7/src
    make clean && make a CC="gcc -m32 -nostdinc -I$ROOT_DIR/apps/include" AR="ar rcs" RANLIB="ranlib" MYCFLAGS="-DLUA_USE_POSIX -DLUA_USE_C89 -ffreestanding -fno-builtin"
    cd "$ROOT_DIR"
    gcc -m32 -nostdinc -Iapps/include -c toolchains/lua-5.4.7/src/lua.c -o toolchains/lua-5.4.7/src/lua.o -DLUA_COMPAT_5_3 -DLUA_USE_POSIX -DLUA_USE_C89 -ffreestanding -fno-builtin
    gcc -m32 -nostdinc -Iapps/include -c toolchains/lua-5.4.7/src/luac.c -o toolchains/lua-5.4.7/src/luac.o -DLUA_COMPAT_5_3 -DLUA_USE_POSIX -DLUA_USE_C89 -ffreestanding -fno-builtin
    ld -m elf_i386 -T lib/linker.ld build/lib/crt0.o build/lib/crti.o toolchains/lua-5.4.7/src/lua.o toolchains/lua-5.4.7/src/liblua.a build/lib/libc.a build/lib/libm.a build/lib/libgcc.a build/lib/crtn.o -o build/toolchain/lua/lua.elf
    ld -m elf_i386 -T lib/linker.ld build/lib/crt0.o build/lib/crti.o toolchains/lua-5.4.7/src/luac.o toolchains/lua-5.4.7/src/liblua.a build/lib/libc.a build/lib/libm.a build/lib/libgcc.a build/lib/crtn.o -o build/toolchain/lua/luac.elf
fi

if [ ! -f "build/toolchain/nasm.elf" ]; then
    echo "Building NASM 2.16.03..."
    cd toolchains/nasm-2.16.03
    ld -m elf_i386 -T ../../lib/linker.ld ../../build/lib/crt0.o ../../build/lib/crti.o \
        asm/nasm.o libnasm.a \
        ../../build/lib/libc.a ../../build/lib/libm.a ../../build/lib/libgcc.a ../../build/lib/crtn.o \
        -o ../../build/toolchain/nasm.elf
    cd "$ROOT_DIR"
fi

if [ ! -f "build/toolchain/python.elf" ]; then
    echo "Building MicroPython 3.12..."
    gcc -m32 -c toolchains/micropython_shim.c -o toolchains/micropython_shim.o \
        -Itoolchains/micropython -Itoolchains/micropython/ports/unix \
        -Itoolchains/micropython/ports/unix/variants/minimal \
        -Itoolchains/micropython/ports/unix/build-minimal \
        -Itoolchains/micropython/ports/unix/build-minimal/genhdr
    OBJS="$(find toolchains/micropython/ports/unix/build-minimal -name '*.o' ! -name 'printf.o' ! -name 'abort_.o' ! -name 'vfs_blockdev.o') toolchains/micropython_shim.o"
    ld -m elf_i386 -T lib/linker.ld build/lib/crt0.o build/lib/crti.o \
        $OBJS \
        build/lib/libc.a build/lib/libm.a build/lib/libgcc.a build/lib/crtn.o \
        -o build/toolchain/python.elf
fi

echo "[5/7] Building kernel, bootloader, and standard applications..."
make kernel boot apps

echo "[6/7] Populating IPO_OS 128MB filesystem disk image..."
python3 load_apps.py

echo "[7/7] Verifying filesystem contents..."
python3 disk_editor.py -i build/disk.img ls /app

echo "================================================================="
echo "✓ IPO_OS Developer Toolchain Bootstrap Completed Successfully!  "
echo "================================================================="

