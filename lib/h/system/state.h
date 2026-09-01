#ifndef IPO_SYSTEM_STATE_H
#define IPO_SYSTEM_STATE_H

#include <stdbool.h>

typedef enum {
    SYSTEM_STATE_BOOT = 0,         // OS Booting / Initializing
    SYSTEM_STATE_TERMINAL_IDLE,    // Shell / Terminal ready and waiting for command
    SYSTEM_STATE_PROCESS_RUNNING,  // Process executing CPU instructions
    SYSTEM_STATE_TEXT_INPUT,       // System waiting for user text input (scanf / sys_read)
    SYSTEM_STATE_ASYNC_RUNNING,    // Background async tasks running
} system_state_t;

/**
 * Get current system state
 */
system_state_t system_get_state(void);

/**
 * Set current system state and log transition
 */
void system_set_state(system_state_t new_state);

/**
 * Get human-readable name of system state
 */
const char *system_state_to_string(system_state_t state);

/**
 * Check if the system is currently in text input mode
 */
bool system_is_input_state(void);

#endif

