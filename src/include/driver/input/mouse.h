#ifndef _MOUSE_H
#define _MOUSE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

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
    bool has_wheel;        /* True if scroll wheel is enabled */
    uint8_t dev_id;        /* PS/2 device ID (0x00, 0x03, 0x04) */
    uint8_t packet_size;   /* Packet size in bytes (3 or 4) */
    uint8_t *raw_packet;   /* Most recent raw packet bytes (dynamically allocated) */
    size_t raw_packet_len; /* Dynamic length of raw packet */
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
 * Set custom screen boundaries.
 * If max_x <= 0 or max_y <= 0, boundaries are dynamically detected from active display.
 */
void mouse_set_bounds(int32_t max_x, int32_t max_y);

/**
 * Automatically update mouse bounds based on currently active display area
 * (VBE BGA if active, VGA graphics if active, or text mode).
 */
void mouse_set_bounds_from_display(void);

/**
 * Get current mouse screen boundaries.
 */
void mouse_get_bounds(int32_t *out_x, int32_t *out_y);

/**
 * Get current mouse position (X, Y coordinates).
 */
void mouse_get_position(int32_t *out_x, int32_t *out_y);

/**
 * Get cumulative relative mouse motion since initialization.
 */
void mouse_get_cumulative_motion(int64_t *out_x, int64_t *out_y);

/**
 * Check if a specific mouse button is currently pressed (MOUSE_BTN_LEFT, MOUSE_BTN_RIGHT, MOUSE_BTN_MIDDLE).
 */
bool mouse_is_pressed(int button);

/**
 * Check if the mouse cursor is located inside the specified rectangular region [x, x+w) x [y, y+h).
 */
bool mouse_is_in_region(int32_t x, int32_t y, int32_t w, int32_t h);

/**
 * Check if a mouse click / press occurred inside the specified rectangular region.
 */
bool mouse_is_clicked_in_region(int32_t x, int32_t y, int32_t w, int32_t h, int button);

/**
 * Convenience function: checks if left mouse button is pressed inside rectangular region.
 */
bool mouse_check_click(int32_t x, int32_t y, int32_t w, int32_t h);

#endif /* _MOUSE_H */
