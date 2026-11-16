#ifndef _MOUSE_H
#define _MOUSE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define MOUSE_BTN_LEFT   0x01
#define MOUSE_BTN_RIGHT  0x02
#define MOUSE_BTN_MIDDLE 0x04

typedef struct {
    int32_t x;             /* pixels, 0..max_x-1 */
    int32_t y;             /* pixels, 0..max_y-1 */
    int32_t col;           /* text column, 0..79 */
    int32_t row;           /* text row, 0..24 */
    int32_t dx;
    int32_t dy;
    int32_t dz;            /* scroll wheel: >0 up, <0 down */
    int32_t scroll_pos;
    int32_t max_x;
    int32_t max_y;
    bool left_button;
    bool right_button;
    bool middle_button;
    bool has_wheel;
    uint8_t dev_id;        /* PS/2 device ID (0x00, 0x03, or 0x04) */
    uint8_t packet_size;   /* 3 or 4 bytes */
    uint8_t *raw_packet;   /* heap-allocated */
    size_t raw_packet_len;
    uint32_t buttons;
    uint32_t event_count;
} mouse_state_t;

void mouse_init(void);

void mouse_handle_byte(uint8_t data);

/* Poll PS/2 controller for mouse/keyboard bytes */
void mouse_poll(void);

void mouse_get_state(mouse_state_t *out_state);

/* Derive bounds from active display if max_x <= 0 or max_y <= 0 */
void mouse_set_bounds(int32_t max_x, int32_t max_y);

/* Sync mouse bounds to active display (linear framebuffer, VGA graphics, or text mode) */
void mouse_set_bounds_from_display(void);

void mouse_get_bounds(int32_t *out_x, int32_t *out_y);

void mouse_get_position(int32_t *out_x, int32_t *out_y);

void mouse_get_cumulative_motion(int64_t *out_x, int64_t *out_y);

bool mouse_is_pressed(int button);

/* Check if cursor is in [x, x+w) x [y, y+h) */
bool mouse_is_in_region(int32_t x, int32_t y, int32_t w, int32_t h);

bool mouse_is_clicked_in_region(int32_t x, int32_t y, int32_t w, int32_t h, int button);

bool mouse_check_click(int32_t x, int32_t y, int32_t w, int32_t h);

/* Inject synthetic relative or absolute pointer event into cursor state */
void mouse_inject_event(int32_t dx_or_x, int32_t dy_or_y, int32_t dz,
                        uint32_t buttons_mask, bool is_absolute);

#endif /* _MOUSE_H */
