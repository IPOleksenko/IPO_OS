include mk/config.mk
include mk/configs.mk
include mk/boot.mk
include mk/lib.mk
include mk/kernel.mk
include mk/image.mk
include mk/run.mk
include mk/clean.mk

all: $(OS_IMAGE)

.PHONY: all run configs patch-config
