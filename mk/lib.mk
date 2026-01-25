SRCS_C := $(shell find $(LIB_DIR)/c -type f -name '*.c' 2>/dev/null)
SRCS_ASM := $(shell find $(LIB_DIR)/asm -type f -name '*.asm' 2>/dev/null)
SRCS_S := $(shell find $(LIB_DIR)/asm -type f -name '*.s' 2>/dev/null)
SRCS_CAPS_S := $(shell find $(LIB_DIR)/asm -type f -name '*.S' 2>/dev/null)

LIB_OBJS := $(patsubst $(LIB_DIR)/%.c,$(LIB_BUILD_DIR)/%.o,$(SRCS_C)) \
		$(patsubst $(LIB_DIR)/%.asm,$(LIB_BUILD_DIR)/%.o,$(SRCS_ASM)) \
		$(patsubst $(LIB_DIR)/%.s,$(LIB_BUILD_DIR)/%.o,$(SRCS_S)) \
		$(patsubst $(LIB_DIR)/%.S,$(LIB_BUILD_DIR)/%.o,$(SRCS_CAPS_S))

.PHONY: lib
lib: $(LIB)

$(LIB): $(LIB_OBJS)
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
	rm -f $(LIB)
