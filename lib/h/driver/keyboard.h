#ifndef _KEYBOARD_H
#define _KEYBOARD_H

#include <stdint.h>
#include <stdbool.h>

#define KBD_DATA_PORT 0x60
#define KBD_STATUS_PORT 0x64
#define KBD_STATUS_OUTPUT_BUFFER 0x01

/**
 * Poll the hardware keyboard controller port and enqueue any available scancodes.
 */
void keyboard_poll(void);

/**
 * Non-blocking: retrieve next scancode from the active queue (returns 0 if empty).
 */
uint8_t keyboard_get_scancode(void);

/**
 * Blocking: wait until next scancode is available from the active queue.
 */
uint8_t keyboard_wait_scancode(void);

/**
 * Enqueue a scancode to the active queue.
 */
void keyboard_enqueue_scancode(uint8_t scancode);

/**
 * Flush shell keyboard queue.
 */
void keyboard_flush_queue(void);

/**
 * Flush app keyboard queue.
 */
void keyboard_flush_app_queue(void);

/**
 * Drain any lingering scancodes directly from the hardware I/O port.
 */
void keyboard_flush_hardware(void);

/**
 * Enable/disable application input mode.
 * When enabled, keyboard scancodes are routed to app_queue.
 */
void keyboard_set_app_input_mode(bool enabled);

/**
 * Check if app input mode is active.
 */
bool keyboard_is_app_input_mode(void);

#endif
