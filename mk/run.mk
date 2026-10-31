setup-tap:
	@./tools/setup-tap.sh $(TAP_DEV)

run:
ifneq ($(BOOTROM),)
	@if [ ! -f "$(BOOTROM)" ]; then \
		echo "ERROR: Boot ROM binary '$(BOOTROM)' not found!" >&2; \
		exit 1; \
	fi
	@echo "================================================================="
	@echo "[IPO_OS] Launching QEMU with Boot ROM: $(BOOTROM)"
ifneq ($(FIRMWARE),)
	@echo "[IPO_OS] FIRMWARE is ignored because BOOTROM is set."
endif
	@echo "================================================================="
else ifneq ($(FIRMWARE),)
	@if [ ! -f "$(FIRMWARE)" ]; then \
		echo "ERROR: Firmware binary '$(FIRMWARE)' not found!" >&2; \
		exit 1; \
	fi
	@echo "================================================================="
	@echo "[IPO_OS] Launching QEMU with BIOS Firmware: $(FIRMWARE)"
	@echo "================================================================="
endif
ifeq ($(NET_MODE),tap)
	@$(MAKE) setup-tap || { \
		echo "[network] TAP setup failed; restarting with QEMU user networking."; \
		$(MAKE) run NET_MODE=user; \
		exit $$?; \
	}
endif
	$(QEMU) $(QEMU_FLAGS)

.PHONY: setup-tap run
