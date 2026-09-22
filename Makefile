.DEFAULT_GOAL := all

include mk/config.mk
include mk/boot.mk
include mk/lib.mk
include mk/toolchain.mk
include mk/kernel.mk
include mk/applications.mk
include mk/image.mk
include mk/run.mk
include mk/clean.mk

all: clean lib kernel boot applications toolchain image disks run

.PHONY: all run patch-config lib kernel boot applications apps image disks toolchain
