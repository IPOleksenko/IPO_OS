clean:
	@rm -rf $(BUILD)/* 2>/dev/null || true
	@rm -f $(OS_IMAGE) 2>/dev/null || true

.PHONY: clean
