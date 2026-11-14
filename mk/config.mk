# Tools

ASM     := nasm
CC      := gcc
LD      := ld
AR      := ar
OBJCOPY := objcopy
QEMU    := qemu-system-i386

# Project directories

SRC          := src
BUILD        := build

# Output artifacts

BOOT_BIN   := $(BUILD)/boot/boot.bin

KERNEL_ELF := $(BUILD)/kernel/kernel.elf
KERNEL_BIN := $(BUILD)/kernel/kernel.bin
OS_BIN     := $(BUILD)/IPO_OS.bin
OS_IMAGE   := $(OS_BIN)

DISK_DIR   := $(BUILD)/disk
DISK_IMG   := $(DISK_DIR)/disk.img

# Library paths

LIB_BUILD_DIR := $(BUILD)/lib
LIB_A         := $(LIB_BUILD_DIR)/libk.a

# Boot configuration

BOOT_CFG_IN    := $(SRC)/boot/config.inc.in
BOOT_CFG_OUT   := $(BUILD)/boot/config.inc

# Kernel and subsystem sources

KERNEL_DIRS   := $(SRC)/kernel $(SRC)/drivers $(SRC)/fs $(SRC)/net $(SRC)/graphics

SRCS_C        := $(filter-out $(SRC)/kernel/kernel32.c, $(shell find $(KERNEL_DIRS) -type f -name '*.c' 2>/dev/null))
SRCS_ASM      := $(filter-out $(SRC)/kernel/entry32.asm, $(shell find $(KERNEL_DIRS) -type f -name '*.asm' 2>/dev/null))
SRCS_S        := $(shell find $(KERNEL_DIRS) -type f -name '*.s' 2>/dev/null)
SRCS_CAPS_S   := $(shell find $(KERNEL_DIRS) -type f -name '*.S' 2>/dev/null)

# Library / kernel objects

LIB_OBJS := \
	$(patsubst $(SRC)/%.c,   $(BUILD)/kernel/%.o, $(SRCS_C)) \
	$(patsubst $(SRC)/%.asm, $(BUILD)/kernel/%.o, $(SRCS_ASM)) \
	$(patsubst $(SRC)/%.s,   $(BUILD)/kernel/%.o, $(SRCS_S)) \
	$(patsubst $(SRC)/%.S,   $(BUILD)/kernel/%.o, $(SRCS_CAPS_S))

KERNEL_OBJS := $(LIB_OBJS)

# Runtime / linker libraries

LIBGCC := $(shell $(CC) -m32 -print-libgcc-file-name)

# Compiler / assembler flags

ASM_BIN_FLAGS := -f bin \
	-I$(SRC)/boot \
	-I$(BUILD)/boot

ASM_ELF_FLAGS := -f elf32

LIB_CFLAGS := -m32 \
	-ffreestanding \
	-fno-pic -fno-pie \
	-fno-builtin \
	-nostdlib -nostartfiles \
	-fno-asynchronous-unwind-tables \
	-fno-unwind-tables \
	-I$(SRC)/include

LD_FLAGS := -T $(SRC)/kernel/linker.ld -nostdlib

NET_MODE ?= tap
TAP_DEV  ?= tap0

ifeq ($(NET_MODE),tap)
  QEMU_NET_FLAGS := -netdev tap,id=net0,ifname=$(TAP_DEV),script=no,downscript=no -device rtl8139,netdev=net0
else
  QEMU_NET_FLAGS := -netdev user,id=net0,net=192.168.7.0/24,host=192.168.7.1,dns=192.168.7.3,dhcpstart=192.168.7.2,hostfwd=tcp::8080-:8080,hostfwd=tcp::8000-:8000 -device rtl8139,netdev=net0
endif

AUDIODEV ?= pa

QEMU_SERIAL_FLAGS := -serial stdio

# Firmware and Boot ROM options

# Optional complete BIOS firmware image (not the <32 KiB payload; e.g. IPO_Firmware/build/firmware.bin)
FIRMWARE ?=

# Optional custom Boot ROM (e.g. make run BOOTROM=../IPO_Boot_ROM/build/bootrom.bin)
BOOTROM  ?=

# If both are specified, BOOTROM takes precedence. Either selected file is attached as the system BIOS ROM.
ifneq ($(BOOTROM),)
  QEMU_BIOS_FLAGS := -bios $(BOOTROM)
else ifneq ($(FIRMWARE),)
  QEMU_BIOS_FLAGS := -bios $(FIRMWARE)
endif

CPU ?= pentium3,+rdrand

QEMU_FLAGS := 	-M pc -cpu $(CPU) -m 2048 \
				$(QEMU_BIOS_FLAGS) \
				-drive format=raw,file=$(DISK_IMG),if=ide,index=0,file.locking=off \
               	-device sb16,audiodev=snd \
               	-audiodev $(AUDIODEV),id=snd -machine pcspk-audiodev=snd \
               	$(QEMU_NET_FLAGS) \
				$(QEMU_SERIAL_FLAGS)
