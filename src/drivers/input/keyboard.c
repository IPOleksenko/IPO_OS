#include <driver/input/keyboard.h>
#include <driver/input/mouse.h>
#include <vga.h>
#include <ioport.h>
#include <driver/input/keymap/keymap.h>
#include <kernel/process.h>
#include <system/state.h>
#include <wm.h>
#include <stdio.h>
#include <kernel/driver.h>
#include <kernel/input_ext.h>
#include <memory/kmalloc.h>

#define KEYBOARD_QUEUE_INITIAL 32

struct keyboard_queue {
    uint8_t  *data;
    uint32_t  cap;
    uint32_t  head;
    uint32_t  tail;
};

static struct keyboard_queue shell_queue = { NULL, 0, 0, 0 };
static struct keyboard_queue app_queue   = { NULL, 0, 0, 0 };
static bool keyboard_app_input_mode = false;

static void keyboard_queue_ensure_init(struct keyboard_queue *queue) {
    if (queue->data == NULL) {
        queue->data = (uint8_t *)kmalloc(KEYBOARD_QUEUE_INITIAL);
        if (queue->data) {
            queue->cap  = KEYBOARD_QUEUE_INITIAL;
            queue->head = 0;
            queue->tail = 0;
        }
    }
}

static uint32_t keyboard_queue_len(const struct keyboard_queue *queue) {
    if (queue->cap == 0) return 0u;
    return (queue->head - queue->tail + queue->cap) % queue->cap;
}

static void keyboard_queue_push(struct keyboard_queue *queue, uint8_t scancode) {
    if (scancode == 0x00u) {
        return;
    }

    keyboard_queue_ensure_init(queue);
    if (queue->data == NULL) return;

    if ((queue->head + 1u) % queue->cap == queue->tail) {
        uint32_t old_cap = queue->cap;
        uint32_t new_cap = (old_cap <= UINT32_MAX / 2u) ? (old_cap * 2u) : 0u;
        uint8_t *new_data = new_cap ? (uint8_t *)kmalloc(new_cap) : NULL;
        if (new_data == NULL) {
            queue->tail = (queue->tail + 1u) % old_cap;
        } else {
            uint32_t len = keyboard_queue_len(queue);
            for (uint32_t i = 0; i < len; i++) {
                new_data[i] = queue->data[(queue->tail + i) % old_cap];
            }
            kfree(queue->data);
            queue->data = new_data;
            queue->tail = 0;
            queue->head = len;
            queue->cap  = new_cap;
        }
    }

    queue->data[queue->head] = scancode;
    queue->head = (queue->head + 1u) % queue->cap;
}

static uint8_t keyboard_queue_pop(struct keyboard_queue *queue) {
    if (queue->data == NULL || queue->cap == 0 || queue->head == queue->tail) {
        return 0x00u;
    }

    uint8_t scancode = queue->data[queue->tail];
    queue->tail = (queue->tail + 1u) % queue->cap;
    return scancode;
}

static inline bool is_background_batch(void) {
    return process_is_batch_active() && !process_is_foreground();
}

static ipo_key_decoder_t s_kbd_decoder = {0};

void keyboard_poll(void) {
    if (!vga_is_graphics_mode()) {
        vga_cursor_blink_tick();
    }
    while (1) {
        uint8_t status = inb(KBD_STATUS_PORT);
        if (!(status & KBD_STATUS_OUTPUT_BUFFER)) {
            break;
        }
        uint8_t data = inb(KBD_DATA_PORT);

        if (status & 0x20) {
            /* Auxiliary / Mouse byte (0x00 is valid for dx, dy, dz!) */
            if (!ipo_ps2_dispatch_byte(true, data)) {
                mouse_handle_byte(data);
            }
        } else {
            /* Keyboard PS/2 byte hook */
            if (ipo_ps2_dispatch_byte(false, data)) {
                continue;
            }
            /* Keyboard scancode (0x00 is invalid / null scancode) */
            if (data == 0x00u) continue;
            uint8_t scancode = data;
            if (!vga_is_graphics_mode()) {
                vga_cursor_reset_blink();
            }
            update_hot_key_state(scancode);

            ipo_key_event_t kev;
            if (!ipo_key_decoder_feed(&s_kbd_decoder, scancode, &kev)) {
                continue;
            }

            bool is_break = (kev.flags & IPO_KEY_FLAG_BREAK) != 0;
            bool is_e0 = (kev.flags & IPO_KEY_FLAG_EXT_E0) != 0;
            bool is_e1 = (kev.flags & IPO_KEY_FLAG_EXT_E1) != 0;
            uint8_t sc_low = (uint8_t)(kev.keycode & 0x7Fu);

            if (driver_dispatch_key(sc_low, is_break)) {
                continue;
            }

            process_t *cur_fg = process_get_foreground();
            if (!wm_session_active()) {
                bool consumed = false;
                uint32_t fg_pid = cur_fg ? cur_fg->pid : 0u;
                (void)ipo_input_process_key_event(&kev, 0u, fg_pid, false, &consumed);
                if (consumed) {
                    continue;
                }
            }

            serial_printf("[kbd_raw] key=0x%x flags=0x%x ctrl=%d wm=%d batch=%d\n",
                          kev.keycode, kev.flags, (int)keyboard_is_ctrl_pressed(),
                          (int)wm_session_active(), (int)process_is_batch_active());

            bool gfx_active_and_focused = wm_session_active() || vga_is_graphics_mode() ||
                                          (cur_fg && (cur_fg->is_wm_app || cur_fg->wants_graphics));
            if (gfx_active_and_focused && process_is_batch_active() && process_get_separate_windows()) {
                if (cur_fg && !cur_fg->is_wm_app && !cur_fg->wants_graphics && !vga_is_graphics_mode()) {
                    gfx_active_and_focused = false;
                }
            }
            if (!is_e0 && !is_e1 && !is_break && sc_low == 0x2Eu &&
                keyboard_is_ctrl_pressed() && !gfx_active_and_focused) {
                system_request_interrupt();
                /* When an app or batch process is active, consume the key
                 * so the running app gets interrupted via system_is_interrupted().
                 * When the terminal shell prompt is active, let the scancode
                 * fall through to shell_queue so terminal.c can handle ^C line clearing. */
                if (keyboard_app_input_mode || process_is_batch_active()) {
                    continue;
                }
            }

            /* Ctrl + PageUp / PageDown: switch between active foreground tabs/processes */
            if (!is_e1 && (sc_low == 0x49u || sc_low == 0x51u) &&
                keyboard_is_ctrl_pressed() && process_is_batch_active()) {
                serial_printf("[kbd] Ctrl+%s switch detected! key=0x%x\n",
                              (sc_low == 0x49u) ? "PageUp" : "PageDown", kev.keycode);
                if (!is_break) {
                    if (sc_low == 0x49u) {
                        process_switch_foreground_prev();
                    } else {
                        process_switch_foreground_next();
                    }
                }
                continue;
            }

            struct keyboard_queue *q = (keyboard_app_input_mode || wm_session_active()) ? &app_queue : &shell_queue;
            for (uint8_t bi = 0; bi < kev.raw_len; bi++) {
                keyboard_queue_push(q, kev.raw_bytes[bi]);
            }
        }
    }
}

void keyboard_flush_hardware(void) {
    if (is_background_batch()) return;
    while (inb(KBD_STATUS_PORT) & KBD_STATUS_OUTPUT_BUFFER) {
        (void)inb(KBD_DATA_PORT);
    }
}

void keyboard_flush_queue(void) {
    if (is_background_batch()) return;
    shell_queue.head = 0;
    shell_queue.tail = 0;
    app_queue.head   = 0;
    app_queue.tail   = 0;
    ipo_key_decoder_reset(&s_kbd_decoder);
}

void keyboard_flush_app_queue(void) {
    if (is_background_batch()) return;
    app_queue.head = 0;
    app_queue.tail = 0;
}

void keyboard_set_app_input_mode(bool enabled) {
    if (is_background_batch()) return;
    if (keyboard_app_input_mode == enabled) {
        return;
    }
    keyboard_app_input_mode = enabled;
    if (enabled) {
        keyboard_flush_hardware();
        keyboard_flush_queue();
        keyboard_flush_app_queue();
        keyboard_clear_key_state();
    }
}

bool keyboard_is_app_input_mode(void) {
    return keyboard_app_input_mode;
}

void keyboard_enqueue_scancode(uint8_t scancode) {
    if (keyboard_app_input_mode) {
        keyboard_queue_push(&app_queue, scancode);
        return;
    }
    keyboard_queue_push(&shell_queue, scancode);
}

uint8_t keyboard_get_scancode_raw(void) {
    if (is_background_batch()) {
        return 0x00u;
    }

    keyboard_poll();
    struct keyboard_queue *q = (keyboard_app_input_mode || wm_session_active()) ? &app_queue : &shell_queue;
    uint8_t sc = keyboard_queue_pop(q);
    if (sc == 0x00u && wm_session_active()) {
        sc = keyboard_queue_pop(&shell_queue);
    }
    return sc;
}

uint8_t keyboard_get_scancode(void) {
    if (is_background_batch()) {
        return 0x00u;
    }

    if (process_in_process_context()) {
        process_t *cur = process_get_current();
        if (cur && wm_has_raw_gfx_window_for_pid(cur->pid)) {
            wm_compositor_tick();
            return wm_raw_gfx_get_scancode(cur);
        }
    }

    return keyboard_get_scancode_raw();
}

uint8_t keyboard_wait_scancode(void) {
    keyboard_set_app_input_mode(true);
    while (1) {
        if (system_is_interrupted()) {
            return 0x00u;
        }
        if (!process_is_foreground()) {
            process_yield();
            continue;
        }

        uint8_t scancode = keyboard_get_scancode();
        if (scancode != 0x00u) {
            process_yield();
            return scancode;
        }
        process_yield();
        io_wait();
    }
}
