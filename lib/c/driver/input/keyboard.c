#include <driver/input/keyboard.h>
#include <driver/input/mouse.h>
#include <vga.h>
#include <ioport.h>
#include <driver/input/keymap/keymap.h>
#include <kernel/process.h>
#include <syscall.h>
#include <system/timer.h>
#include <system/state.h>
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

void keyboard_poll(void) {
    vga_cursor_blink_tick();
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
            vga_cursor_reset_blink();
            update_hot_key_state(scancode);
            if (scancode == 0x2E && keyboard_is_ctrl_pressed()) {
                system_request_interrupt();
            }
            if (keyboard_app_input_mode) {
                keyboard_queue_push(&app_queue, scancode);
            } else {
                keyboard_queue_push(&shell_queue, scancode);
            }
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
    if (!is_foreground_process()) {
        yield_waiting(true);
        return 0x00u;
    }

    keyboard_poll();
    struct keyboard_queue *q = keyboard_app_input_mode ? &app_queue : &shell_queue;
    uint8_t sc = keyboard_queue_pop(q);
    if (keyboard_app_input_mode && (sc == 0x1C || sc == 0x9C)) {
        if (timer_elapsed_ms(app_input_mode_start_ms) < 250u) {
            sc = 0x00u;
        }
    }
    if (sc == 0x00u) {
        yield_waiting(true);
    } else {
        yield_waiting(false);
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
