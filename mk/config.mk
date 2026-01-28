# ==================================================
#                     TOOLS
# ==================================================

ASM     := nasm
CC      := gcc
LD      := ld
AR      := ar
OBJCOPY := objcopy
QEMU    := qemu-system-i386


# ==================================================
#                 PROJECT DIRECTORIES
# ==================================================

SRC     := src
BUILD   := build


# ==================================================
#                 OUTPUT / ARTIFACTS
# ==================================================

BOOT_BIN   := $(BUILD)/boot/boot.bin

KERNEL_ELF := $(BUILD)/kernel.elf
KERNEL_BIN := $(BUILD)/kernel.bin
OS_IMAGE   := $(BUILD)/IPO_OS.img


# ==================================================
#                 LIBRARY PATHS
# ==================================================

LIB_DIR       := lib
LIB_BUILD_DIR := $(BUILD)/lib
LIB_A         := $(LIB_BUILD_DIR)/libc.a


# ==================================================
#                 BOOT CONFIG PATHS
# ==================================================

BOOT_CFG_IN    := $(SRC)/boot/config.inc.in
STAGE1_CFG_IN  := $(SRC)/boot/stage1/config.inc.in
STAGE2_CFG_IN  := $(SRC)/boot/stage2/config.inc.in

BOOT_CFG_OUT   := $(BUILD)/boot/config.inc
STAGE1_CFG_OUT := $(BUILD)/boot/stage1/config.inc
STAGE2_CFG_OUT := $(BUILD)/boot/stage2/config.inc


# ==================================================
#                 LIBRARY SOURCES
# ==================================================

SRCS_C        := $(shell find $(LIB_DIR)/c   -type f -name '*.c'   2>/dev/null)
SRCS_ASM      := $(shell find $(LIB_DIR)/asm -type f -name '*.asm' 2>/dev/null)
SRCS_S        := $(shell find $(LIB_DIR)/asm -type f -name '*.s'   2>/dev/null)
SRCS_CAPS_S   := $(shell find $(LIB_DIR)/asm -type f -name '*.S'   2>/dev/null)


# ==================================================
#                 LIBRARY OBJECTS
# ==================================================

LIB_OBJS := \
	$(patsubst $(LIB_DIR)/%.c,   $(LIB_BUILD_DIR)/%.o, $(SRCS_C)) \
	$(patsubst $(LIB_DIR)/%.asm, $(LIB_BUILD_DIR)/%.o, $(SRCS_ASM)) \
	$(patsubst $(LIB_DIR)/%.s,   $(LIB_BUILD_DIR)/%.o, $(SRCS_S)) \
	$(patsubst $(LIB_DIR)/%.S,   $(LIB_BUILD_DIR)/%.o, $(SRCS_CAPS_S))


# ==================================================
#                 RUNTIME / LINKER LIBS
# ==================================================

LIBGCC := $(shell $(CC) -m32 -print-libgcc-file-name)


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

LIB_CFLAGS := -m32 \
	-ffreestanding \
	-fno-pic -fno-pie \
	-fno-builtin \
	-nostdlib -nostartfiles \
	-Ilib/h

LD_FLAGS := -T $(SRC)/kernel/linker.ld -nostdlib

QEMU_FLAGS := -drive format=raw,file=$(OS_IMAGE)
