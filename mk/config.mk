# ==================================================
#                     TOOLS
# ==================================================

ASM     := nasm
LD      := ld
OBJCOPY := objcopy
QEMU    := qemu-system-x86_64
CC      := gcc
AR      := ar

SRC     := src
BUILD   := build

LIBGCC := $(shell gcc -m32 -print-libgcc-file-name)

# ==================================================
#                     PATHS
# ==================================================

STAGE1_BIN := $(BUILD)/boot/stage1/stage1.bin
STAGE2_BIN := $(BUILD)/boot/stage2/stage2.bin

BOOT_CFG_IN   := $(SRC)/boot/config.inc.in
STAGE1_CFG_IN := $(SRC)/boot/stage1/config.inc.in
STAGE2_CFG_IN := $(SRC)/boot/stage2/config.inc.in

BOOT_CFG_OUT   := $(BUILD)/boot/config.inc
STAGE1_CFG_OUT := $(BUILD)/boot/stage1/config.inc
STAGE2_CFG_OUT := $(BUILD)/boot/stage2/config.inc

KERNEL_ELF := $(BUILD)/kernel.elf
KERNEL_BIN := $(BUILD)/kernel.bin
OS_IMAGE   := $(BUILD)/IPO_OS.img

LIB_DIR       := lib
LIB_BUILD_DIR := $(BUILD)/lib
LIB           := $(LIB_BUILD_DIR)/libc.a

# ==================================================
#                     FLAGS
# ==================================================

ASM_BIN_FLAGS := -f bin \
	-I$(SRC)/boot \
	-I$(SRC)/boot/stage1 \
	-I$(SRC)/boot/stage2 \
	-I$(BUILD)/boot \
	-I$(BUILD)/boot/stage1 \
	-I$(BUILD)/boot/stage2


ASM_ELF_FLAGS := -f elf32
LD_FLAGS      := -T $(SRC)/kernel/linker.ld -nostdlib
QEMU_FLAGS    := -drive format=raw,file=$(OS_IMAGE)

LIB_CFLAGS := -m32 -ffreestanding -fno-pic -fno-pie -fno-builtin -nostdlib -nostartfiles -Ilib/h
