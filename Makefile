.DEFAULT_GOAL := all

include mk/config.mk
include mk/boot.mk
include mk/lib.mk
include mk/kernel.mk
include mk/image.mk
include mk/run.mk
include mk/clean.mk

all: clean lib kernel boot image disks run

.PHONY: all run patch-config lib kernel boot image disks
