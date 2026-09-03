#include <driver/input/keyboard.h>
#include <vga.h>
#include <ioport.h>
#include <driver/input/keymap/keymap.h>
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
    while (inb(KBD_STATUS_PORT) & KBD_STATUS_OUTPUT_BUFFER) {
        uint8_t scancode = inb(KBD_DATA_PORT);
        if (scancode != 0x00u) {
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
}

void keyboard_flush_app_queue(void) {
    app_queue.head = 0;
    app_queue.tail = 0;
}

void keyboard_set_app_input_mode(bool enabled) {
    keyboard_app_input_mode = enabled;
    serial_printf("[keyboard] app_input_mode = %d\n", enabled ? 1 : 0);
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

uint8_t keyboard_get_scancode(void) {
    keyboard_poll();
    struct keyboard_queue *q = keyboard_app_input_mode ? &app_queue : &shell_queue;
    return keyboard_queue_pop(q);
}

uint8_t keyboard_wait_scancode(void) {
    while (1) {
        uint8_t scancode = keyboard_get_scancode();
        if (scancode != 0x00u) {
            return scancode;
        }
        io_wait();
    }
}
