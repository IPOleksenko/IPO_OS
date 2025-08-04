#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include <kernel/drv/tty.h>
#include <kernel/sys/ioports.h>

size_t terminal_row = 0;
size_t terminal_column = 0;
uint8_t terminal_color = 0;
uint16_t* terminal_buffer = NULL;
uint16_t scroll_buffer[SCROLL_BUFFER_SIZE][VGA_HEIGHT * VGA_WIDTH];
size_t scroll_offset = 0;
size_t scroll_buffer_pos = 0;

uint16_t current_terminal_state[VGA_HEIGHT * VGA_WIDTH];
size_t saved_terminal_row = 0;
size_t saved_terminal_column = 0;
bool state_saved = false;


void scroll_terminal(void)
{
    // Save the entire current screen to the scroll buffer
    for (size_t i = 0; i < VGA_HEIGHT * VGA_WIDTH; i++) {
        scroll_buffer[scroll_buffer_pos][i] = terminal_buffer[i];
    }

    // Move to the next screen in the circular buffer
    scroll_buffer_pos = (scroll_buffer_pos + 1) % SCROLL_BUFFER_SIZE;

    // Shift all lines up by one
    for (size_t y = 1; y < VGA_HEIGHT; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            const size_t from_index = y * VGA_WIDTH + x;
            const size_t to_index = (y - 1) * VGA_WIDTH + x;
            terminal_buffer[to_index] = terminal_buffer[from_index];
        }
    }

    // Clear the last line
    for (size_t x = 0; x < VGA_WIDTH; x++) {
        const size_t index = (VGA_HEIGHT - 1) * VGA_WIDTH + x;
        terminal_buffer[index] = vga_entry(' ', terminal_color);
    }

    terminal_row = VGA_HEIGHT - 1;
}

// Scroll up through history
void scroll_up(void)
{
    size_t max_scroll = (scroll_buffer_pos < SCROLL_BUFFER_SIZE) 
        ? scroll_buffer_pos 
        : SCROLL_BUFFER_SIZE;

    if (scroll_offset < max_scroll) {
        scroll_offset++;
        update_display();
    }
}

// Scroll down through history
void scroll_down(void)
{
    if (scroll_offset > 0) {
        scroll_offset--;
        update_display();

        if (scroll_offset == 0) {
            state_saved = false;
        }
    }
}


// Update display based on scroll offset
void update_display(void)
{
    if (scroll_offset == 0) {
        // Restore the current terminal state
        if (state_saved) {
            for (size_t i = 0; i < VGA_HEIGHT * VGA_WIDTH; i++) {
                terminal_buffer[i] = current_terminal_state[i];
            }
            terminal_row = saved_terminal_row;
            terminal_column = saved_terminal_column;
            terminal_set_cursor_position(terminal_row * VGA_WIDTH + terminal_column);
        }
        return;
    }

    // Save the current terminal state if it hasn't been saved yet
    if (!state_saved) {
        for (size_t i = 0; i < VGA_HEIGHT * VGA_WIDTH; i++) {
            current_terminal_state[i] = terminal_buffer[i];
        }
        saved_terminal_row = terminal_row;
        saved_terminal_column = terminal_column;
        state_saved = true;
    }

    // Calculate the index of the desired screen in the scroll buffer
    size_t max_scroll = (scroll_buffer_pos < SCROLL_BUFFER_SIZE) ? scroll_buffer_pos : SCROLL_BUFFER_SIZE;
    if (scroll_offset > max_scroll) {
        scroll_offset = max_scroll;
    }

    size_t screen_index = (scroll_buffer_pos + SCROLL_BUFFER_SIZE - scroll_offset) % SCROLL_BUFFER_SIZE;

    // Load the screen from the scroll buffer
    for (size_t i = 0; i < VGA_HEIGHT * VGA_WIDTH; i++) {
        terminal_buffer[i] = scroll_buffer[screen_index][i];
    }

    // Hide the cursor while scrolling
    terminal_set_cursor_position(VGA_HEIGHT * VGA_WIDTH);
}


void terminal_initialize(void)
{
    terminal_row = 0;
    terminal_column = 0;
    terminal_color = vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    terminal_buffer = (uint16_t*) 0xB8000;
    scroll_offset = 0;
    scroll_buffer_pos = 0;
    state_saved = false;

    // Initialize scroll_buffer as an array of screens
    for (size_t i = 0; i < SCROLL_BUFFER_SIZE; i++) {
        for (size_t j = 0; j < VGA_HEIGHT * VGA_WIDTH; j++) {
            scroll_buffer[i][j] = vga_entry(' ', terminal_color);
        }
    }

    // Initialize the main video buffer
    for (size_t y = 0; y < VGA_HEIGHT; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            const size_t index = y * VGA_WIDTH + x;
            terminal_buffer[index] = vga_entry(' ', terminal_color);
        }
    }
}


void terminal_clear(void) 
{
    for (size_t y = 0; y < VGA_HEIGHT; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            terminal_putentryat(' ', terminal_color, x, y);
        }
    }
    scroll_offset     = 0;
    scroll_buffer_pos = 0;
    state_saved       = false;

    terminal_row    = 0;
    terminal_column = 0;
    terminal_set_cursor_position(0);
}

void terminal_setcolor(uint8_t color) 
{
	terminal_color = color;
}

void terminal_set_cursor_position(uint16_t position) {
    outb(0x3D4, 0x0F); 
    outb(0x3D5, (uint8_t)(position & 0xFF));
    outb(0x3D4, 0x0E); 
    outb(0x3D5, (uint8_t)((position >> 8) & 0xFF));
}

void terminal_putentryat(char c, uint8_t color, size_t x, size_t y) 
{
	const size_t index = y * VGA_WIDTH + x;
	terminal_buffer[index] = vga_entry(c, color);
}

void copyright_text()
{
    // Strings to display
    const char* left_text = "IPO_OS.";
    const char* right_text = "Created by IPOleksenko.";
    
    // Calculate padding spaces
    size_t left_text_len = strlen(left_text);
    size_t right_text_len = strlen(right_text);
    size_t padding = VGA_WIDTH - left_text_len - right_text_len;

    // Save the original color to restore later
    uint8_t original_color = terminal_color;

    // Set custom color for header
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));

    // Write left-aligned text, padding, and right-aligned text in one line
    printf(left_text);
    for (size_t i = 0; i < padding; i++) {
        putchar(' ');
    }
    printf(right_text);

    // Restore original color
    terminal_setcolor(original_color);
    printf("\n");
}
