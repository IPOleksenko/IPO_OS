#include <kernel/terminal.h>

#include <vga.h>
#include <driver/keyboard.h>
#include <driver/input/keymap/keymap.h>

void print_header(void) {
    volatile uint16_t* vga = VGA_MEMORY;

    char os_name[] = "IPO_OS";
    char created_by[] = "Created by IPOleksenko";

    uint8_t os_name_length = sizeof(os_name) - 1;
    uint8_t created_by_length = sizeof(created_by) - 1;

    for (uint8_t i = 0; i < os_name_length; i++) {
        vga[i] = vga_entry(os_name[i], VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    }

    for (uint8_t i = 0; i < created_by_length; i++) {
        vga[VGA_WIDTH - created_by_length + i] = vga_entry(created_by[i], VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    }
}

void terminal_initialize(void) {
    vga_clear(VGA_COLOR_WHITE, VGA_COLOR_BLACK, true, VGA_START_CURSOR_POSITION);

    print_header();
}

void handle_control_char(char c) {
    volatile uint16_t* vga = VGA_MEMORY;
    uint16_t cursor = vga_get_cursor_position();
    
    switch(c) {
        case '\n': {
            uint16_t row = cursor / VGA_WIDTH;
            uint16_t next_row = row + 1;
            
            if (next_row >= VGA_HEIGHT) {
                for (int i = 0; i < VGA_WIDTH * (VGA_HEIGHT - 1); i++) {
                    vga[i] = vga[i + VGA_WIDTH];
                }
                uint16_t blank = vga_entry(0x00, VGA_COLOR_WHITE, VGA_COLOR_BLACK);
                for (int i = VGA_WIDTH * (VGA_HEIGHT - 1); i < VGA_WIDTH * VGA_HEIGHT; i++) {
                    vga[i] = blank;
                }
                vga_set_cursor((VGA_HEIGHT - 1) * VGA_WIDTH);
            } else {
                vga_set_cursor(next_row * VGA_WIDTH);
            }
            break;
        }
        
        case '\t': {
            uint16_t col = cursor % VGA_WIDTH;
            uint16_t next_col = col + 4;
            if (next_col >= VGA_WIDTH) {
                handle_control_char('\n');
            } else {
                vga_set_cursor(cursor + 4);
            }
            break;
        }
        
        case '\b': {
            if (cursor > 0) {
                cursor--;
                vga[cursor] = vga_entry(0x00, VGA_COLOR_WHITE, VGA_COLOR_BLACK);
                vga_set_cursor(cursor);
            }
            break;
        }
    }
}

void terminal_console(void){
    uint8_t scancode = keyboard_get_scancode();
    volatile uint16_t* vga = VGA_MEMORY;
        
    if (scancode != 0x00) {
        update_hot_key_state(scancode);
        hot_key_handler(scancode);
        
        bool is_break_code = (scancode & 0x80) != 0;
        
        if (!is_break_code) {
            char c = get_char(scancode);
            if (c != 0x00) {
                if (c == '\n' || c == '\t' || c == '\b') {
                    handle_control_char(c);
                } else {
                    vga[vga_get_cursor_position()] = vga_entry(c, VGA_COLOR_WHITE, VGA_COLOR_BLACK);
                    vga_increment_cursor_position();
                }
            }
        }
    }
}