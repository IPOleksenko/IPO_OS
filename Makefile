.DEFAULT_GOAL := all

include mk/config.mk
include mk/boot.mk
include mk/lib.mk
include mk/kernel.mk
include mk/app.mk
include mk/image.mk
include mk/run.mk
include mk/clean.mk

all: clean lib kernel boot apps image disks run

.PHONY: all run patch-config lib kernel boot apps image disks
