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
    vga_clear(VGA_COLOR_WHITE, VGA_COLOR_BLACK, true, VGA_WIDTH * 2);

    print_header();
}

void terminal_console(void){
    uint8_t scancode = keyboard_get_scancode();
    volatile uint16_t* vga = VGA_MEMORY;
        
    if (scancode != 0x00) {
        update_hot_key_state(scancode);
        hot_key_handler(scancode);
           
        vga[80] = vga_entry(get_char(scancode), VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    }
}