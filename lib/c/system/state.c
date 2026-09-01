#include <system/state.h>
#include <stdio.h>

static system_state_t current_system_state = SYSTEM_STATE_BOOT;

system_state_t system_get_state(void) {
    return current_system_state;
}

const char *system_state_to_string(system_state_t state) {
    switch (state) {
        case SYSTEM_STATE_BOOT:
            return "BOOT";
        case SYSTEM_STATE_TERMINAL_IDLE:
            return "TERMINAL_IDLE";
        case SYSTEM_STATE_PROCESS_RUNNING:
            return "PROCESS_RUNNING";
        case SYSTEM_STATE_TEXT_INPUT:
            return "TEXT_INPUT";
        case SYSTEM_STATE_ASYNC_RUNNING:
            return "ASYNC_RUNNING";
        default:
            return "UNKNOWN";
    }
}

void system_set_state(system_state_t new_state) {
    if (current_system_state == new_state) {
        return;
    }

    system_state_t old_state = current_system_state;
    current_system_state = new_state;

    serial_printf("[system_state] transition: %s -> %s\n",
                  system_state_to_string(old_state),
                  system_state_to_string(new_state));
}

bool system_is_input_state(void) {
    return current_system_state == SYSTEM_STATE_TEXT_INPUT;
}

