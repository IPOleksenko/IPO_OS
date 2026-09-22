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

#define KEYBOARD_QUEUE_SIZE 256

struct keyboard_queue {
    volatile uint8_t data[KEYBOARD_QUEUE_SIZE];
    volatile uint32_t head;
    volatile uint32_t tail;
};

static struct keyboard_queue shell_queue = { {0}, 0, 0 };
static struct keyboard_queue app_queue = { {0}, 0, 0 };
static bool keyboard_app_input_mode = false;

static void keyboard_queue_push(struct keyboard_queue *queue, uint8_t scancode) {
    if (scancode == 0x00u) {
        return;
    }

    uint32_t next = (queue->head + 1u) % KEYBOARD_QUEUE_SIZE;
    if (next == queue->tail) {
        // Queue is full, drop oldest
        queue->tail = (queue->tail + 1u) % KEYBOARD_QUEUE_SIZE;
    }

    queue->data[queue->head] = scancode;
    queue->head = next;
}

static uint8_t keyboard_queue_pop(struct keyboard_queue *queue) {
    if (queue->head == queue->tail) {
        return 0x00u;
    }

    uint8_t scancode = queue->data[queue->tail];
    queue->tail = (queue->tail + 1u) % KEYBOARD_QUEUE_SIZE;
    return scancode;
}

static bool is_batch_running(void) {
    if (process_is_batch_active()) {
        return true;
    }
    int res = ipo_syscall(IPO_SYSCALL_PROCESS_IS_BATCH_ACTIVE, 0, NULL);
    if (res == (int)IPO_SYSCALL_ENOSYS) {
        return process_is_batch_active();
    }
    return res != 0;
}

static bool is_wm_active(void) {
    if (wm_session_active()) {
        return true;
    }
    int res = ipo_syscall(IPO_SYSCALL_WM_SESSION_ACTIVE, 0, NULL);
    if (res == (int)IPO_SYSCALL_ENOSYS) {
        return wm_session_active();
    }
    return res != 0;
}

static void trigger_switch_fg(bool prev) {
    if (process_is_batch_active()) {
        if (prev) {
            process_switch_foreground_prev();
        } else {
            process_switch_foreground_next();
        }
        return;
    }
    uint32_t arg = prev ? 1 : 0;
    ipo_syscall(IPO_SYSCALL_PROCESS_SWITCH_FG, 1, &arg);
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
                continue;
            }

            /* Ctrl + PageUp: Switch to previous process / window */
            if ((scancode == 0x49 || scancode == 0xC9) && keyboard_is_ctrl_pressed() && is_batch_running()) {
                serial_printf("[kbd] Ctrl+PageUp detected! sc=0x%x\n", scancode);
                pending_e0 = false;
                if (scancode == 0x49) {
                    trigger_switch_fg(true);
                }
                continue;
            }

            /* Ctrl + PageDown: Switch to next process / window */
            if ((scancode == 0x51 || scancode == 0xD1) && keyboard_is_ctrl_pressed() && is_batch_running()) {
                serial_printf("[kbd] Ctrl+PageDown detected! sc=0x%x\n", scancode);
                pending_e0 = false;
                if (scancode == 0x51) {
                    trigger_switch_fg(false);
                }
                continue;
            }

            /* Alt + Tab: Switch to next process / window */
            if ((scancode == 0x0F || scancode == 0x8F) && keyboard_is_alt_pressed() && is_batch_running() && !is_wm_active()) {
                serial_printf("[kbd] Alt+Tab detected! sc=0x%x\n", scancode);
                pending_e0 = false;
                if (scancode == 0x0F) {
                    trigger_switch_fg(false);
                }
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
    while (inb(KBD_STATUS_PORT) & KBD_STATUS_OUTPUT_BUFFER) {
        (void)inb(KBD_DATA_PORT);
    }
}

void keyboard_flush_queue(void) {
    shell_queue.head = 0;
    shell_queue.tail = 0;
    app_queue.head = 0;
    app_queue.tail = 0;
}

void keyboard_flush_app_queue(void) {
    app_queue.head = 0;
    app_queue.tail = 0;
}

static uint32_t app_input_mode_start_ms = 0;

void keyboard_set_app_input_mode(bool enabled) {
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

static bool is_foreground_process(void) {
    int res = ipo_syscall(IPO_SYSCALL_PROCESS_IS_FOREGROUND, 0, NULL);
    if (res == (int)IPO_SYSCALL_ENOSYS) {
        return true;
    }
    return res != 0;
}

static void yield_waiting(bool waiting) {
    uint32_t arg = waiting ? 1 : 0;
    ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 1, &arg);
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
