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

SRC          := src
APPS_DIR     := applications
BUILD        := build


# ==================================================
#                 OUTPUT / ARTIFACTS
# ==================================================

BOOT_BIN   := $(BUILD)/boot/boot.bin

KERNEL_ELF := $(BUILD)/kernel/kernel.elf
KERNEL_BIN := $(BUILD)/kernel/kernel.bin
OS_IMAGE   := $(BUILD)/IPO_OS.img


# ==================================================
#                 LIBRARY PATHS
# ==================================================

LIB_BUILD_DIR := $(BUILD)/lib
LIB_A         := $(LIB_BUILD_DIR)/libk.a

USER_LIBC_A   := $(LIB_BUILD_DIR)/libc.a
USER_LIBM_A   := $(LIB_BUILD_DIR)/libm.a
USER_LIBCXX_A := $(LIB_BUILD_DIR)/libstdc++.a
USER_CRT0     := $(LIB_BUILD_DIR)/crt0.o
USER_CRTI     := $(LIB_BUILD_DIR)/crti.o
USER_CRTN     := $(LIB_BUILD_DIR)/crtn.o


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
#            KERNEL & SUBSYSTEM SOURCES
# ==================================================

KERNEL_DIRS   := $(SRC)/kernel $(SRC)/drivers $(SRC)/fs $(SRC)/net $(SRC)/graphics

SRCS_C        := $(filter-out $(SRC)/kernel/kernel32.c, $(shell find $(KERNEL_DIRS) -type f -name '*.c' 2>/dev/null))
SRCS_ASM      := $(filter-out $(SRC)/kernel/entry32.asm, $(shell find $(KERNEL_DIRS) -type f -name '*.asm' 2>/dev/null))
SRCS_S        := $(shell find $(KERNEL_DIRS) -type f -name '*.s' 2>/dev/null)
SRCS_CAPS_S   := $(shell find $(KERNEL_DIRS) -type f -name '*.S' 2>/dev/null)


# ==================================================
#                 LIBRARY / KERNEL OBJECTS
# ==================================================

LIB_OBJS := \
	$(patsubst $(SRC)/%.c,   $(BUILD)/kernel/%.o, $(SRCS_C)) \
	$(patsubst $(SRC)/%.asm, $(BUILD)/kernel/%.o, $(SRCS_ASM)) \
	$(patsubst $(SRC)/%.s,   $(BUILD)/kernel/%.o, $(SRCS_S)) \
	$(patsubst $(SRC)/%.S,   $(BUILD)/kernel/%.o, $(SRCS_CAPS_S))

KERNEL_OBJS := $(LIB_OBJS)


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
	-I$(SRC)/include

LD_FLAGS := -T $(SRC)/kernel/linker.ld -nostdlib

AUDIODEV ?= pa

QEMU_FLAGS := 	-m 2048 \
				-drive format=raw,file=$(OS_IMAGE),if=ide,index=0 \
              	-drive format=raw,file=build/disk.img,if=ide,index=1 \
               	-cdrom build/disk.iso \
               	-device sb16,audiodev=snd \
               	-audiodev $(AUDIODEV),id=snd -machine pcspk-audiodev=snd \
               	-netdev user,id=net0 -device rtl8139,netdev=net0 \
				-serial stdio
