.PHONY: lib
lib: $(LIB_A)

$(LIB_A): $(LIB_OBJS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

# C sources
$(LIB_BUILD_DIR)/%.o: $(LIB_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) -MD -c $< -o $@ -std=gnu11 $(LIB_CFLAGS)

# ASM sources
$(LIB_BUILD_DIR)/%.o: $(LIB_DIR)/%.asm
	@mkdir -p $(dir $@)
	$(ASM) $(ASM_ELF_FLAGS) $< -o $@

$(LIB_BUILD_DIR)/%.o: $(LIB_DIR)/%.s
	@mkdir -p $(dir $@)
	$(CC) -MD -c $< -o $@ $(LIB_CFLAGS)

$(LIB_BUILD_DIR)/%.o: $(LIB_DIR)/%.S
	@mkdir -p $(dir $@)
	$(CC) -MD -c $< -o $@ $(LIB_CFLAGS)

.PHONY: clean-lib
clean-lib:
	rm -rf $(LIB_BUILD_DIR)
	rm -f $(LIB_A)
