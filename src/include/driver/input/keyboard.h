#ifndef _KEYBOARD_H
#define _KEYBOARD_H

#include <stdint.h>
#include <stdbool.h>

#define KBD_DATA_PORT 0x60
#define KBD_STATUS_PORT 0x64
#define KBD_STATUS_OUTPUT_BUFFER 0x01

void keyboard_poll(void);

/* Non-blocking pop from active queue; returns 0 if empty */
uint8_t keyboard_get_scancode(void);

/* Direct queue pop bypassing WM per-window routing (avoids compositor recursion) */
uint8_t keyboard_get_scancode_raw(void);

uint8_t keyboard_wait_scancode(void);

void keyboard_enqueue_scancode(uint8_t scancode);

void keyboard_flush_queue(void);

void keyboard_flush_app_queue(void);

/* Drain pending scancodes directly from the controller I/O port */
void keyboard_flush_hardware(void);

/* Route incoming scancodes to app_queue when enabled */
void keyboard_set_app_input_mode(bool enabled);

bool keyboard_is_app_input_mode(void);

#endif
