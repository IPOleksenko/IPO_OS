#include <driver/input/mouse.h>
#include <driver/input/keyboard.h>
#include <ioport.h>
#include <stdio.h>
#include <kernel/driver.h>

#define KBD_DATA_PORT    0x60
#define KBD_STATUS_PORT  0x64
#define KBD_CMD_PORT     0x64

#define KBD_STATUS_OBF   0x01  /* Output Buffer Full */
#define KBD_STATUS_IBF   0x02  /* Input Buffer Full */
#define KBD_STATUS_AUX   0x20  /* Auxiliary (Mouse) data in 0x60 */

static mouse_state_t current_mouse_state = {
    .x = 320,
    .y = 200,
    .col = 40,
    .row = 12,
    .dx = 0,
    .dy = 0,
    .dz = 0,
    .scroll_pos = 0,
    .max_x = 640,
    .max_y = 400,
    .left_button = false,
    .right_button = false,
    .middle_button = false,
    .has_wheel = false,
    .buttons = 0,
    .event_count = 0
};

static uint8_t mouse_cycle = 0;
static uint8_t mouse_packet[4] = {0, 0, 0, 0};
static uint8_t mouse_packet_size = 3;
static uint8_t mouse_device_id = 0;
static bool mouse_has_wheel = false;
static bool mouse_initialized = false;

static void mouse_wait_write(void) {
    for (int timeout = 0; timeout < 100000; timeout++) {
        if ((inb(KBD_STATUS_PORT) & KBD_STATUS_IBF) == 0) return;
        io_wait();
    }
}

static uint8_t mouse_wait_read(void) {
    for (int timeout = 0; timeout < 100000; timeout++) {
        if (inb(KBD_STATUS_PORT) & KBD_STATUS_OBF) {
            return inb(KBD_DATA_PORT);
        }
        io_wait();
    }
    return 0;
}

static void mouse_write(uint8_t byte) {
    mouse_wait_write();
    outb(KBD_CMD_PORT, 0xD4); // Route to mouse
    mouse_wait_write();
    outb(KBD_DATA_PORT, byte);
}

static uint8_t mouse_read(void) {
    return mouse_wait_read();
}

static void mouse_flush(void) {
    for (int i = 0; i < 20; i++) {
        if (inb(KBD_STATUS_PORT) & KBD_STATUS_OBF) {
            (void)inb(KBD_DATA_PORT);
            io_wait();
        } else {
            break;
        }
    }
}

void mouse_init(void) {
    if (mouse_initialized) return;

    serial_printf("[mouse] Initializing PS/2 mouse (with IntelliMouse wheel detection)...\n");

    // Flush any pending data from 8042 controller
    mouse_flush();

    // Enable auxiliary device (mouse port) on 8042
    mouse_wait_write();
    outb(KBD_CMD_PORT, 0xA8);

    // Read controller configuration byte
    mouse_wait_write();
    outb(KBD_CMD_PORT, 0x20);
    uint8_t status = mouse_wait_read();

    // Clear mouse clock disable (bit 5) and keep interrupts polled (clear bit 1 & bit 0)
    status &= ~0x02;  // IRQ12 disable (polled I/O)
    status &= ~0x20;  // Mouse clock enable (0 = enabled)
    mouse_wait_write();
    outb(KBD_CMD_PORT, 0x60);
    mouse_wait_write();
    outb(KBD_DATA_PORT, status);

    // Reset to default settings (0xF6)
    mouse_write(0xF6);
    (void)mouse_read(); // ACK (0xFA)

    // Microsoft IntelliMouse sequence to enable scroll wheel:
    // Set sample rate 200, then 100, then 80
    mouse_write(0xF3);
    (void)mouse_read(); // ACK
    mouse_write(200);
    (void)mouse_read(); // ACK

    mouse_write(0xF3);
    (void)mouse_read(); // ACK
    mouse_write(100);
    (void)mouse_read(); // ACK

    mouse_write(0xF3);
    (void)mouse_read(); // ACK
    mouse_write(80);
    (void)mouse_read(); // ACK

    // Read Device ID (0xF2)
    mouse_write(0xF2);
    (void)mouse_read(); // ACK
    uint8_t dev_id = mouse_read();
    serial_printf("[mouse] PS/2 Device ID: 0x%x\n", dev_id);
    mouse_device_id = dev_id;

    if (dev_id == 0x03 || dev_id == 0x04) {
        mouse_packet_size = 4;
        mouse_has_wheel = true;
        current_mouse_state.has_wheel = true;
        current_mouse_state.dev_id = dev_id;
        current_mouse_state.packet_size = 4;
        serial_printf("[mouse] IntelliMouse with wheel enabled (4-byte packets, ID=0x%x)\n", dev_id);
    } else {
        mouse_packet_size = 3;
        mouse_has_wheel = false;
        current_mouse_state.has_wheel = false;
        current_mouse_state.dev_id = dev_id;
        current_mouse_state.packet_size = 3;
        serial_printf("[mouse] Standard PS/2 mouse (3-byte packets, ID=0x%x)\n", dev_id);
    }

    // Set resolution: 4 counts/mm
    mouse_write(0xE8);
    (void)mouse_read();
    mouse_write(0x03);
    (void)mouse_read();

    // Enable data reporting (0xF4)
    mouse_write(0xF4);
    uint8_t ack_enable = mouse_read(); // ACK (0xFA)
    serial_printf("[mouse] Data reporting enabled (ACK=0x%x)\n", ack_enable);

    // Flush any leftover controller bytes so packet stream starts cleanly
    mouse_flush();

    mouse_cycle = 0;
    mouse_initialized = true;

    static driver_t ps2_mouse_drv = {
        .name = "ps2_mouse",
        .description = "PS/2 Mouse Driver with Wheel Support",
        .flags = DRIVER_FLAG_KERNEL | DRIVER_FLAG_ACTIVE,
        .init = NULL,
        .cleanup = NULL,
        .on_command = NULL,
        .next = NULL
    };
    driver_register(&ps2_mouse_drv);

    serial_printf("[mouse] PS/2 mouse driver registered successfully.\n");
}

static void mouse_process_packet(void) {
    uint8_t flags = mouse_packet[0];
    // Overflow check
    if (flags & 0xC0) {
        return;
    }

    int32_t dx = (int32_t)mouse_packet[1];
    if (flags & 0x10) {
        dx -= 256;
    }

    int32_t raw_dy = (int32_t)mouse_packet[2];
    if (flags & 0x20) {
        raw_dy -= 256;
    }

    /* In standard PS/2 mouse hardware:
     * raw_dy > 0 when mouse moves UP (forward, away from user).
     * raw_dy < 0 when mouse moves DOWN (backward, towards user).
     * In screen coordinates (VGA text/graphics):
     * y = 0 is top of screen, y increases downwards.
     * Therefore screen delta dy = -raw_dy. */
    int32_t dy = -raw_dy;

    int32_t dz = 0;
    if (mouse_packet_size >= 4 && mouse_has_wheel) {
        int8_t z_raw = (int8_t)mouse_packet[3];
        if (mouse_device_id == 0x04) {
            /* 5-button IntelliMouse Explorer: lower 4 bits are signed movement */
            int8_t z_4bit = (int8_t)(mouse_packet[3] & 0x0F);
            if (z_4bit & 0x08) {
                z_4bit |= (int8_t)0xF0;
            }
            dz = (int32_t)z_4bit;
        } else {
            /* Standard 3-button IntelliMouse: signed 8-bit movement */
            if (z_raw >= -8 && z_raw <= 8) {
                dz = (int32_t)z_raw;
            } else {
                int8_t z_4bit = (int8_t)(mouse_packet[3] & 0x0F);
                if (z_4bit & 0x08) {
                    z_4bit |= (int8_t)0xF0;
                }
                dz = (int32_t)z_4bit;
            }
        }
        /* Hardware/emulator reports negative when rolling forward (UP) and positive when rolling backward (DOWN).
         * Invert dz so that positive dz (+1) is SCROLL UP and negative dz (-1) is SCROLL DOWN. */
        dz = -dz;
    }

    current_mouse_state.dx = dx;
    current_mouse_state.dy = dy;
    current_mouse_state.dz = dz;
    current_mouse_state.scroll_pos += dz;

    current_mouse_state.x += dx;
    current_mouse_state.y += dy;

    // Clamp to screen bounds
    if (current_mouse_state.x < 0) current_mouse_state.x = 0;
    if (current_mouse_state.x >= current_mouse_state.max_x) current_mouse_state.x = current_mouse_state.max_x - 1;

    if (current_mouse_state.y < 0) current_mouse_state.y = 0;
    if (current_mouse_state.y >= current_mouse_state.max_y) current_mouse_state.y = current_mouse_state.max_y - 1;

    // Update text mode coordinates (80x25 for standard VGA 640x400)
    current_mouse_state.col = (current_mouse_state.x * 80) / current_mouse_state.max_x;
    current_mouse_state.row = (current_mouse_state.y * 25) / current_mouse_state.max_y;
    if (current_mouse_state.col < 0) current_mouse_state.col = 0;
    if (current_mouse_state.col >= 80) current_mouse_state.col = 79;
    if (current_mouse_state.row < 0) current_mouse_state.row = 0;
    if (current_mouse_state.row >= 25) current_mouse_state.row = 24;

    current_mouse_state.left_button = (flags & 0x01) != 0;
    current_mouse_state.right_button = (flags & 0x02) != 0;
    current_mouse_state.middle_button = (flags & 0x04) != 0;
    current_mouse_state.buttons = flags & 0x07;
    current_mouse_state.has_wheel = mouse_has_wheel;
    current_mouse_state.dev_id = mouse_device_id;
    current_mouse_state.packet_size = mouse_packet_size;
    for (int k = 0; k < 4; k++) current_mouse_state.raw_packet[k] = mouse_packet[k];
    current_mouse_state.event_count++;
}

void mouse_handle_byte(uint8_t data) {
    if (mouse_cycle == 0) {
        // Bit 3 of byte 0 must be 1, and bits 6 & 7 (overflow) must be 0 in valid PS/2 packets
        if ((data & 0x08) == 0 || (data & 0xC0) != 0) {
            return;
        }
        mouse_packet[0] = data;
        mouse_cycle = 1;
    } else if (mouse_cycle == 1) {
        mouse_packet[1] = data;
        mouse_cycle = 2;
    } else if (mouse_cycle == 2) {
        mouse_packet[2] = data;
        if (mouse_packet_size == 3) {
            mouse_cycle = 0;
            mouse_process_packet();
        } else {
            mouse_cycle = 3;
        }
    } else if (mouse_cycle == 3) {
        mouse_packet[3] = data;
        mouse_cycle = 0;
        mouse_process_packet();
    }
}

void mouse_poll(void) {
    keyboard_poll();
}

void mouse_get_state(mouse_state_t *out_state) {
    if (!out_state) return;
    mouse_poll();
    *out_state = current_mouse_state;
}

void mouse_set_bounds(int32_t max_x, int32_t max_y) {
    if (max_x > 0) current_mouse_state.max_x = max_x;
    if (max_y > 0) current_mouse_state.max_y = max_y;
    if (current_mouse_state.x >= current_mouse_state.max_x) current_mouse_state.x = current_mouse_state.max_x - 1;
    if (current_mouse_state.y >= current_mouse_state.max_y) current_mouse_state.y = current_mouse_state.max_y - 1;
}
