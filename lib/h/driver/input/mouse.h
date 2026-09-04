#ifndef _MOUSE_H
#define _MOUSE_H

#include <stdint.h>
#include <stdbool.h>

#define MOUSE_BTN_LEFT   0x01
#define MOUSE_BTN_RIGHT  0x02
#define MOUSE_BTN_MIDDLE 0x04

typedef struct {
    int32_t x;             /* Current X position in pixels (e.g. 0 .. max_x-1) */
    int32_t y;             /* Current Y position in pixels (e.g. 0 .. max_y-1) */
    int32_t col;           /* Text mode column (0 .. 79) */
    int32_t row;           /* Text mode row (0 .. 24) */
    int32_t dx;            /* Delta X from last packet */
    int32_t dy;            /* Delta Y from last packet */
    int32_t dz;            /* Delta Z (scroll wheel: >0 up, <0 down) from last packet */
    int32_t scroll_pos;    /* Accumulated scroll position */
    int32_t max_x;         /* Screen boundary width */
    int32_t max_y;         /* Screen boundary height */
    bool left_button;      /* True if left button is currently pressed */
    bool right_button;     /* True if right button is currently pressed */
    bool middle_button;    /* True if middle button is currently pressed */
    bool has_wheel;        /* True if scroll wheel (IntelliMouse) is enabled */
    uint8_t dev_id;        /* PS/2 device ID (0x00, 0x03, 0x04) */
    uint8_t packet_size;   /* Packet size in bytes (3 or 4) */
    uint8_t raw_packet[4]; /* Most recent raw packet bytes */
    uint32_t buttons;      /* Bitmask of pressed buttons */
    uint32_t event_count;  /* Total number of mouse packets processed */
} mouse_state_t;

/**
 * Initialize PS/2 mouse hardware via 8042 controller.
 */
void mouse_init(void);

/**
 * Process a raw byte received from the mouse data port.
 */
void mouse_handle_byte(uint8_t data);

/**
 * Poll the 8042 controller for mouse/keyboard data.
 */
void mouse_poll(void);

/**
 * Get current mouse snapshot.
 */
void mouse_get_state(mouse_state_t *out_state);

/**
 * Set custom screen boundaries (default: 640x400).
 */
void mouse_set_bounds(int32_t max_x, int32_t max_y);

#endif /* _MOUSE_H */
