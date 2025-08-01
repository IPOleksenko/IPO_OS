#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <kernel/drv/tty.h>
#include <kernel/sys/ioports.h>

size_t terminal_row = 0;
size_t terminal_column = 0;
uint8_t terminal_color = 0;
uint16_t* terminal_buffer = NULL;
uint16_t scroll_buffer[SCROLL_BUFFER_SIZE * VGA_WIDTH];
size_t scroll_offset = 0;
size_t scroll_buffer_pos = 0;

// Store current terminal state
static uint16_t current_terminal_state[VGA_HEIGHT * VGA_WIDTH];
static size_t saved_terminal_row = 0;
static size_t saved_terminal_column = 0;
static bool state_saved = false;

void scroll_terminal(void)
{
    // Save the top line to scroll buffer before scrolling
    for (size_t x = 0; x < VGA_WIDTH; x++) {
        size_t buffer_index = (scroll_buffer_pos * VGA_WIDTH) + x;
        scroll_buffer[buffer_index] = terminal_buffer[x];
    }
    
    // Move to next position in circular buffer
    scroll_buffer_pos = (scroll_buffer_pos + 1) % SCROLL_BUFFER_SIZE;
    
    // Move each row of the buffer up by one
    for (size_t y = 1; y < VGA_HEIGHT; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            const size_t from_index = y * VGA_WIDTH + x;
            const size_t to_index = (y - 1) * VGA_WIDTH + x;
            terminal_buffer[to_index] = terminal_buffer[from_index];
        }
    }

    // Clear the last row of the buffer
    for (size_t x = 0; x < VGA_WIDTH; x++) {
        const size_t index = (VGA_HEIGHT - 1) * VGA_WIDTH + x;
        terminal_buffer[index] = vga_entry(' ', terminal_color);
    }

    // Adjust the row position to stay within bounds
    terminal_row = VGA_HEIGHT - 1;
}

// Scroll up through history
void scroll_up(void)
{
    // Check if we have history to scroll through
    size_t max_scroll = (scroll_buffer_pos < SCROLL_BUFFER_SIZE) ? scroll_buffer_pos : SCROLL_BUFFER_SIZE;
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
        
        // Reset state saved flag when returning to current view
        if (scroll_offset == 0) {
            state_saved = false;
        }
    }
}


// Update display based on scroll offset
void update_display(void)
{
    if (scroll_offset == 0) {
        // Restore current terminal state
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
    
    // Save current terminal state if not already saved
    if (!state_saved) {
        for (size_t i = 0; i < VGA_HEIGHT * VGA_WIDTH; i++) {
            current_terminal_state[i] = terminal_buffer[i];
        }
        saved_terminal_row = terminal_row;
        saved_terminal_column = terminal_column;
        state_saved = true;
    }
    
    // Calculate how many lines to show from scroll buffer
    size_t lines_from_buffer = (scroll_offset < VGA_HEIGHT) ? scroll_offset : VGA_HEIGHT;
    
    // Calculate starting position in scroll buffer
    size_t start_pos = (scroll_buffer_pos - lines_from_buffer + SCROLL_BUFFER_SIZE) % SCROLL_BUFFER_SIZE;
    
    // Display lines from scroll buffer
    for (size_t y = 0; y < lines_from_buffer; y++) {
        size_t buffer_line = (start_pos + y) % SCROLL_BUFFER_SIZE;
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            size_t buffer_index = buffer_line * VGA_WIDTH + x;
            size_t screen_index = y * VGA_WIDTH + x;
            terminal_buffer[screen_index] = scroll_buffer[buffer_index];
        }
    }
    
    // Fill remaining lines with current terminal content
    for (size_t y = lines_from_buffer; y < VGA_HEIGHT; y++) {
        size_t current_line = y - lines_from_buffer;
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            size_t screen_index = y * VGA_WIDTH + x;
            size_t current_index = current_line * VGA_WIDTH + x;
            terminal_buffer[screen_index] = current_terminal_state[current_index];
        }
    }
    
    // Hide cursor during scroll
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
	
	// Initialize scroll buffer
	for (size_t i = 0; i < SCROLL_BUFFER_SIZE * VGA_WIDTH; i++) {
		scroll_buffer[i] = vga_entry(' ', terminal_color);
	}
	
	// Initialize terminal buffer
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
    terminal_row = 0;
    terminal_column = 0;
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

void terminal_putchar(char c)
{
    // Reset scroll offset when new content is added
    if (scroll_offset > 0) {
        scroll_offset = 0;
        // Restore current terminal state before adding new content
        if (state_saved) {
            for (size_t i = 0; i < VGA_HEIGHT * VGA_WIDTH; i++) {
                terminal_buffer[i] = current_terminal_state[i];
            }
            terminal_row = saved_terminal_row;
            terminal_column = saved_terminal_column;
        }
        state_saved = false;
    }
    
    switch (c) {
        case '\n': // Newline
            terminal_column = 0;
            if (++terminal_row == VGA_HEIGHT){
                terminal_row = 0; // Reset to the top if the end of the screen is reached
                scroll_terminal();
                }
            break;
        case '\r': // Carriage return
            terminal_column = 0;
            break;
        case '\t': { // Tab
            size_t tab_size = 4; // Tab size
            size_t next_tab_stop = (terminal_column + tab_size) & ~(tab_size - 1);
            while (terminal_column < next_tab_stop) {
                terminal_putentryat(' ', terminal_color, terminal_column, terminal_row);
                if (++terminal_column == VGA_WIDTH) {
                    terminal_column = 0;
                    if (++terminal_row == VGA_HEIGHT){
                        terminal_row = 0;
                        scroll_terminal();
                        }
                }
            }
            break;
        }
        case '\b': // Backspace
            if (terminal_column > 0) {
                --terminal_column;
                terminal_putentryat(' ', terminal_color, terminal_column, terminal_row);
            } else if (terminal_row > 0) {
                --terminal_row;
                terminal_column = VGA_WIDTH - 1;
                terminal_putentryat(' ', terminal_color, terminal_column, terminal_row);
            }
            break;
        default: // Regular characters
            terminal_putentryat(c, terminal_color, terminal_column, terminal_row);
            if (++terminal_column == VGA_WIDTH) {
                terminal_column = 0;
                if (++terminal_row == VGA_HEIGHT){
                    terminal_row = 0;
                    scroll_terminal();
                    }
            }
            break;
    }
    // Update the hardware cursor
    terminal_set_cursor_position(terminal_row * VGA_WIDTH + terminal_column);
}

void terminal_write(const char* data, size_t size) 
{
	for (size_t i = 0; i < size; i++)
		terminal_putchar(data[i]);
}

void terminal_writestring(const char* data) 
{
	terminal_write(data, strlen(data));
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
    terminal_writestring(left_text);
    for (size_t i = 0; i < padding; i++) {
        terminal_putchar(' ');
    }
    terminal_writestring(right_text);

    // Restore original color
    terminal_setcolor(original_color);
    terminal_writestring("\n");
}
