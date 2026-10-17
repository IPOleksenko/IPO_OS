#include <system/state.h>
#include <syscall.h>

static system_state_t g_user_state = SYSTEM_STATE_PROCESS_RUNNING;

system_state_t system_get_state(void) {
    return g_user_state;
}

void system_set_state(system_state_t new_state) {
    g_user_state = new_state;
}

const char *system_state_to_string(system_state_t state) {
    switch (state) {
        case SYSTEM_STATE_BOOT: return "BOOT";
        case SYSTEM_STATE_TERMINAL_IDLE: return "TERMINAL_IDLE";
        case SYSTEM_STATE_PROCESS_RUNNING: return "PROCESS_RUNNING";
        case SYSTEM_STATE_TEXT_INPUT: return "TEXT_INPUT";
        case SYSTEM_STATE_ASYNC_RUNNING: return "ASYNC_RUNNING";
        default: return "UNKNOWN";
    }
}

bool system_is_input_state(void) {
    return g_user_state == SYSTEM_STATE_TEXT_INPUT;
}

void system_request_interrupt(void) {
    uint32_t arg = 1u;
    ipo_syscall(IPO_SYSCALL_SYSTEM_INTERRUPT, 1u, &arg);
}

void system_clear_interrupt(void) {
    uint32_t arg = 0u;
    ipo_syscall(IPO_SYSCALL_SYSTEM_INTERRUPT, 1u, &arg);
}

bool system_is_interrupted(void) {
    return ipo_syscall(IPO_SYSCALL_SYSTEM_INTERRUPT, 0u, NULL) != 0;
}

