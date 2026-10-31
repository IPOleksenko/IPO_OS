setup-tap:
	@./tools/setup-tap.sh $(TAP_DEV)

run:
ifeq ($(NET_MODE),tap)
	@$(MAKE) setup-tap || { \
		echo "[network] TAP setup failed; restarting with QEMU user networking."; \
		$(MAKE) run NET_MODE=user; \
		exit $$?; \
	}
endif
	$(QEMU) $(QEMU_FLAGS)
