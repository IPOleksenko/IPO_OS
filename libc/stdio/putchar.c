#include <stdio.h>

#if defined(__is_libk)
#include <kernel/drv/tty.h>
#endif

int putchar(int ic) {
#if defined(__is_libk)
	char c = (char) ic;
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
#else
    // TODO: Implement putchar for user-space
#endif
	return ic;
}
