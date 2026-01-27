#include <vga.h>
#include <ioport.h>

// Sets the cursor position
// offset = row * VGA_WIDTH + col
void vga_set_cursor(uint16_t offset) {
    // 0x3D4 is the cursor location register high byte
    // 0x3D5 is the cursor location register low byte
    
    // Set high byte
    outb(0x3D4, 0x0E);
    outb(0x3D5, (offset >> 8) & 0xFF);
    
    // Set low byte
    outb(0x3D4, 0x0F);
    outb(0x3D5, offset & 0xFF);
}

// Shows the cursor
void vga_show_cursor(void) {
    // Cursor start register (0x0A) - enable cursor
    outb(0x3D4, 0x0A);
    uint8_t val = inb(0x3D5);
    outb(0x3D5, val & ~0x20);  // Clear bit 5 to show cursor
}

// Hides the cursor
void vga_hide_cursor(void) {
    // Cursor start register (0x0A) - disable cursor
    outb(0x3D4, 0x0A);
    uint8_t val = inb(0x3D5);
    outb(0x3D5, val | 0x20);  // Set bit 5 to hide cursor
}

void vga_clear(enum vga_color fg, enum vga_color bg, bool show_cursor, int cursor_position) {
    volatile uint16_t* vga = VGA_MEMORY;
    uint16_t blank = vga_entry(0x00, fg, bg);

    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        vga[i] = blank;
    }

    if (show_cursor) {
        vga_set_cursor(cursor_position);
        vga_show_cursor();
    } else {
        vga_hide_cursor();
    }
}