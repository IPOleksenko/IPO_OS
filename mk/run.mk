setup-tap:
	@./tools/setup-tap.sh $(TAP_DEV)

run:
ifeq ($(NET_MODE),tap)
	@ip link show $(TAP_DEV) >/dev/null 2>&1 || $(MAKE) setup-tap
endif
	@python3 tools/create_live_image.py >/dev/null 2>&1 || true
	$(QEMU) $(QEMU_FLAGS)
