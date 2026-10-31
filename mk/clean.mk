clean:
	@find $(BUILD) -mindepth 1 ! -path '$(BUILD)/boot*' ! -path '$(BUILD)/disks*' -delete 2>/dev/null || true
	@find $(BUILD)/boot -mindepth 1 ! -name 'config.inc' -delete 2>/dev/null || true
	@rm -rf $(BUILD)/kernel $(BUILD)/lib 2>/dev/null || true
	@rm -f $(OS_IMAGE) 2>/dev/null || true

.PHONY: clean
