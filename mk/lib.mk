# Kernel subsystem library (libk.a)

.PHONY: lib clean-lib

lib: $(LIB_A)

$(LIB_A): $(LIB_OBJS)
	@mkdir -p $(dir $@)
	rm -f $@
	$(AR) rcs $@ $^

clean-lib:
	rm -rf $(LIB_BUILD_DIR)
