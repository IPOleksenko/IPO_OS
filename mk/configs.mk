configs: $(BOOT_CFG_OUT) $(STAGE1_CFG_OUT) $(STAGE2_CFG_OUT)

$(BOOT_CFG_OUT): $(BOOT_CFG_IN)
	mkdir -p $(dir $@)
	cp $< $@

$(STAGE1_CFG_OUT): $(STAGE1_CFG_IN)
	mkdir -p $(dir $@)
	cp $< $@

$(STAGE2_CFG_OUT): $(STAGE2_CFG_IN)
	mkdir -p $(dir $@)
	cp $< $@
