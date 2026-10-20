#include <driver/input/keyboard.h>
#include <driver/input/mouse.h>
#include <vga.h>
#include <ioport.h>
#include <driver/input/keymap/keymap.h>
#include <kernel/process.h>
#include <syscall.h>
#include <system/timer.h>
#include <system/state.h>
#include <wm.h>
#include <stdio.h>
#include <kernel/driver.h>
#include <memory/kmalloc.h>
#include <string.h>

/* Dynamic Self-Expanding Keyboard Queue */
#define KEYBOARD_QUEUE_INITIAL 32

struct keyboard_queue {
    uint8_t  *data;     /* Heap-allocated scancode buffer                  */
    uint32_t  cap;      /* Current allocated capacity (number of entries)  */
    uint32_t  head;     /* Write index (next slot to write)                */
    uint32_t  tail;     /* Read index  (next slot to read)                 */
};

static struct keyboard_queue shell_queue = { NULL, 0, 0, 0 };
static struct keyboard_queue app_queue   = { NULL, 0, 0, 0 };
static bool keyboard_app_input_mode = false;

/* Ensure the queue has an allocated buffer (lazy init on first push) */
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

/* Number of scancodes currently stored */
static uint32_t keyboard_queue_len(const struct keyboard_queue *queue) {
    if (queue->cap == 0) return 0u;
    return (queue->head - queue->tail + queue->cap) % queue->cap;
}

static void keyboard_queue_push(struct keyboard_queue *queue, uint8_t scancode) {
    if (scancode == 0x00u) {
        return;
    }

    keyboard_queue_ensure_init(queue);
    if (queue->data == NULL) return; /* OOM - silently drop */

    /* Grow when full (head+1 == tail) */
    if ((queue->head + 1u) % queue->cap == queue->tail) {
        uint32_t old_cap = queue->cap;
        uint32_t new_cap = old_cap * 2u;
        uint8_t *new_data = (uint8_t *)kmalloc(new_cap);
        if (new_data == NULL) {
            /* OOM: no growth - drop the oldest entry to make room */
            queue->tail = (queue->tail + 1u) % old_cap;
        } else {
            /* Linearize the ring into the new buffer */
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


static bool is_batch_running(void) {
    return process_is_batch_active();
}

static bool is_foreground_process(void) {
    return process_is_foreground();
}

static bool is_wm_active(void) {
    return wm_session_active();
}

static void trigger_switch_fg(bool prev) {
    if (prev) {
        process_switch_foreground_prev();
    } else {
        process_switch_foreground_next();
    }
}

static bool pending_e0 = false;

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
            mouse_handle_byte(data);
        } else {
            /* Keyboard scancode (0x00 is invalid / null scancode) */
            if (data == 0x00u) continue;
            uint8_t scancode = data;
            if (!vga_is_graphics_mode()) {
                vga_cursor_reset_blink();
            }
            update_hot_key_state(scancode);

            bool is_break = (scancode & 0x80u) != 0;
            if (driver_dispatch_key(scancode & 0x7Fu, is_break)) {
                continue;
            }
            serial_printf("[kbd_raw] data=0x%x sc=0x%x ctrl=%d wm=%d batch=%d\n",
                          data, scancode, (int)keyboard_is_ctrl_pressed(),
                          (int)is_wm_active(), (int)is_batch_running());

            if (scancode == 0xE0u) {
                pending_e0 = true;
                continue;
            }

            bool wm_active_and_focused = is_wm_active();
            if (wm_active_and_focused && is_batch_running() && process_get_separate_windows()) {
                process_t *cur_fg = process_get_foreground();
                if (cur_fg && !cur_fg->is_wm_app && !cur_fg->wants_graphics) {
                    wm_active_and_focused = false;
                }
            }
            if (scancode == 0x2E && keyboard_is_ctrl_pressed() && !wm_active_and_focused) {
                system_request_interrupt();
                /* When an app or batch process is active, consume the key
                 * so the running app gets interrupted via system_is_interrupted().
                 * When the terminal shell prompt is active, let the scancode
                 * fall through to shell_queue so terminal.c can handle ^C line clearing. */
                if (keyboard_app_input_mode || is_batch_running()) {
                    continue;
                }
            }

            /* Ctrl / Alt + PageUp: Switch to previous process / window */
            if ((scancode == 0x49 || scancode == 0xC9) && (keyboard_is_ctrl_pressed() || keyboard_is_alt_pressed()) && is_batch_running()) {
                serial_printf("[kbd] PageUp switch detected! sc=0x%x\n", scancode);
                pending_e0 = false;
                if (scancode == 0x49) {
                    trigger_switch_fg(true);
                }
                continue;
            }

            /* Ctrl / Alt + PageDown: Switch to next process / window */
            if ((scancode == 0x51 || scancode == 0xD1) && (keyboard_is_ctrl_pressed() || keyboard_is_alt_pressed()) && is_batch_running()) {
                serial_printf("[kbd] PageDown switch detected! sc=0x%x\n", scancode);
                pending_e0 = false;
                if (scancode == 0x51) {
                    trigger_switch_fg(false);
                }
                continue;
            }

            /* Alt + Tab or Ctrl + Tab: Switch to next process / window */
            if ((scancode == 0x0F || scancode == 0x8F) && (keyboard_is_alt_pressed() || keyboard_is_ctrl_pressed()) && is_batch_running() && !is_wm_active()) {
                serial_printf("[kbd] Tab switch detected! sc=0x%x\n", scancode);
                pending_e0 = false;
                if (scancode == 0x0F) {
                    trigger_switch_fg(false);
                }
                continue;
            }

            /* Alt + 1..9: Switch directly to workspace N */
            if (scancode >= 0x02 && scancode <= 0x0A && keyboard_is_alt_pressed() && is_batch_running() && !is_wm_active()) {
                serial_printf("[kbd] Alt+%d switch detected!\n", scancode - 0x01);
                pending_e0 = false;
                process_switch_foreground_to_workspace((int)(scancode - 0x02));
                continue;
            }

            struct keyboard_queue *q = (keyboard_app_input_mode || is_wm_active()) ? &app_queue : &shell_queue;
            if (pending_e0) {
                keyboard_queue_push(q, 0xE0u);
                pending_e0 = false;
            }
            keyboard_queue_push(q, scancode);
        }
    }
}

void keyboard_flush_hardware(void) {
    if (is_batch_running() && !is_foreground_process()) return;
    while (inb(KBD_STATUS_PORT) & KBD_STATUS_OUTPUT_BUFFER) {
        (void)inb(KBD_DATA_PORT);
    }
}

void keyboard_flush_queue(void) {
    if (is_batch_running() && !is_foreground_process()) return;
    shell_queue.head = 0;
    shell_queue.tail = 0;
    app_queue.head   = 0;
    app_queue.tail   = 0;
}

void keyboard_flush_app_queue(void) {
    if (is_batch_running() && !is_foreground_process()) return;
    app_queue.head = 0;
    app_queue.tail = 0;
}

static uint32_t app_input_mode_start_ms = 0;

void keyboard_set_app_input_mode(bool enabled) {
    if (is_batch_running() && !is_foreground_process()) return;
    if (keyboard_app_input_mode == enabled) {
        return;
    }
    keyboard_app_input_mode = enabled;
    if (enabled) {
        app_input_mode_start_ms = timer_millis();
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


static void yield_waiting(bool waiting) {
    (void)waiting;
    process_yield();
}

uint8_t keyboard_get_scancode(void) {
    if (!is_wm_active() && !is_foreground_process()) {
        return 0x00u;
    }

    keyboard_poll();
    struct keyboard_queue *q = (keyboard_app_input_mode || is_wm_active()) ? &app_queue : &shell_queue;
    uint8_t sc = keyboard_queue_pop(q);
    if (sc == 0x00u && is_wm_active()) {
        sc = keyboard_queue_pop(&shell_queue);
    }
    return sc;
}

uint8_t keyboard_wait_scancode(void) {
    keyboard_set_app_input_mode(true);
    while (1) {
        if (system_is_interrupted()) {
            return 0x00u;
        }
        if (!is_foreground_process()) {
            yield_waiting(true);
            continue;
        }

        uint8_t scancode = keyboard_get_scancode();
        if (scancode != 0x00u) {
            yield_waiting(false);
            return scancode;
        }
        yield_waiting(true);
        io_wait();
    }
}
