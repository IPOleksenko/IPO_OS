#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include <kernel/drv/keyboard.h>
#include <kernel/drv/tty.h>
#include <kernel/sys/ioports.h>
#include <kernel/sys/isr.h>
#include <kernel/drv/keymap.h>
#include <kernel/sys/fs.h>
#include <kernel/sys/kheap.h>

static bool extended_scancode = false;

// Keyboard interrupt handler
static void keyboard_irq_handler(__attribute__((unused)) registers_t r) {
    uint8_t scancode = inb(0x60);
    
    // Handle extended scancodes
    if (scancode == 0xE0) {
        extended_scancode = true;
        return;
    }
    
    if (extended_scancode) {
        extended_scancode = false;
        
        // Check if it's a key press (not release)
        if (!(scancode & 0x80)) {
            switch (scancode) {
                case 0x48: // Up arrow
                    scroll_up();
                    return;
                case 0x50: // Down arrow
                    scroll_down();
                    return;
            }
        }
        return;
    }
}

// Initialize keyboard interrupt handler
void keyboard_init(void) {
    install_irq_handler(1, keyboard_irq_handler);
}

char* keyboard_input() {
    static char* input_buffer = NULL;
    static size_t buffer_capacity = 0;
    size_t buffer_index = 0;
    bool shift_pressed = false;
    
    // Initial buffer size
    const size_t initial_capacity = 256;
    const size_t growth_factor = 2;

    // Allocate initial buffer if it doesn't exist yet
    if (!input_buffer) {
        input_buffer = (char*)kmalloc(initial_capacity);
        if (!input_buffer) {
            printf("Error: Out of memory for input buffer\n");
            return NULL;
        }
        buffer_capacity = initial_capacity;
    }

    // Display current path instead of simple ">"
    printf("\n");
    printf(fs_get_current_path());
    printf(" > ");
    
    while (1) {
        // Wait until the keyboard controller is ready
        if (!(inb(0x64) & 0x1)) {
            continue;
        }

        uint8_t scancode = inb(0x60); // Read the scancode

        // Check for key release (break code)
        if (scancode & 0x80) {
            uint8_t key_released = scancode & 0x7F; // Get the make code
            if (key_released == 0x2A || key_released == 0x36) { // Left or Right Shift released
                shift_pressed = false;
            }
            continue;
        }

        // Skip extended scancodes in input mode (handled by interrupt)
        if (scancode == 0xE0) {
            continue;
        }

        // Check for key press
        if (scancode == 0x2A || scancode == 0x36) { // Left or Right Shift pressed
            shift_pressed = true;
            continue;
        }

        const char* current_keymap = get_keymap(shift_pressed);
        char key = current_keymap[scancode];

        switch (key) {
            case '\n': // Enter
                if (buffer_index > 0) {
                    // Make sure there's space for null terminator
                    if (buffer_index >= buffer_capacity) {
                        size_t new_capacity = buffer_capacity * growth_factor;
                        char* new_buffer = (char*)kmalloc(new_capacity);
                        if (new_buffer) {
                            for (size_t i = 0; i < buffer_index; i++) {
                                new_buffer[i] = input_buffer[i];
                            }
                            kfree(input_buffer);
                            input_buffer = new_buffer;
                            buffer_capacity = new_capacity;
                        }
                    }
                    input_buffer[buffer_index] = '\0';
                    putchar('\n');
                    return input_buffer;
                }
                break;

            case '\b': // Backspace
                if (buffer_index > 0) {
                    buffer_index--;
                    putchar('\b');
                    putchar(' ');
                    putchar('\b');
                }
                break;

            case '\t':  // Tab
                {
                    size_t tab_size = 4; // Tab size
                    size_t next_tab_stop = (buffer_index + tab_size) & ~(tab_size - 1);
                    size_t spaces_to_add = next_tab_stop - buffer_index;

                    // Check if we need to expand the buffer
                    while (buffer_index + spaces_to_add >= buffer_capacity) {
                        size_t new_capacity = buffer_capacity * growth_factor;
                        char* new_buffer = (char*)kmalloc(new_capacity);
                        if (!new_buffer) {
                            // If memory allocation failed, ignore tab
                            break;
                        }
                        for (size_t i = 0; i < buffer_index; i++) {
                            new_buffer[i] = input_buffer[i];
                        }
                        kfree(input_buffer);
                        input_buffer = new_buffer;
                        buffer_capacity = new_capacity;
                    }

                    // Add spaces for tab
                    for (size_t i = 0; i < spaces_to_add && buffer_index < buffer_capacity - 1; i++) {
                        input_buffer[buffer_index++] = ' ';
                        putchar(' '); // Show space on the screen
                    }
                }
                break;

            default: // Regular character
                if (key) {
                    // Check if we need to expand the buffer
                    if (buffer_index >= buffer_capacity - 1) {
                        size_t new_capacity = buffer_capacity * growth_factor;
                        char* new_buffer = (char*)kmalloc(new_capacity);
                        if (!new_buffer) {
                            // If memory allocation failed, ignore character
                            break;
                        }
                        for (size_t i = 0; i < buffer_index; i++) {
                            new_buffer[i] = input_buffer[i];
                        }
                        kfree(input_buffer);
                        input_buffer = new_buffer;
                        buffer_capacity = new_capacity;
                    }
                    
                    input_buffer[buffer_index++] = key;
                    putchar(key);
                }
                break;
        }
    }
}
