#ifndef IPO_SYSTEM_STATE_H
#define IPO_SYSTEM_STATE_H

#include <stdbool.h>

typedef enum {
    SYSTEM_STATE_BOOT = 0,
    SYSTEM_STATE_TERMINAL_IDLE,
    SYSTEM_STATE_PROCESS_RUNNING,
    SYSTEM_STATE_TEXT_INPUT,       /* blocked on scanf / sys_read */
    SYSTEM_STATE_ASYNC_RUNNING,
} system_state_t;

system_state_t system_get_state(void);

/* Update system state and log transition */
void system_set_state(system_state_t new_state);

const char *system_state_to_string(system_state_t state);

bool system_is_input_state(void);

/* Cancel active operation/queue (Ctrl+C) */
void system_request_interrupt(void);
void system_clear_interrupt(void);
bool system_is_interrupted(void);

#endif

