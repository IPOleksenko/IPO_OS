#include <vga.h>
#include <ioport.h>

// Sets the cursor position
// offset = row * VGA_WIDTH + col
void vga_set_cursor(uint16_t offset) {
    // Write directly to hardware (no VGA_WIDTH addition!)
    outb(0x3D4, 0x0E);
    outb(0x3D5, (offset >> 8) & 0xFF);
    
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

uint16_t vga_get_cursor_position(void) {
    uint16_t cursor_hw = 0;
    outb(0x3D4, 0x0E);
    cursor_hw = (uint16_t)inb(0x3D5) << 8;
    outb(0x3D4, 0x0F);
    cursor_hw |= inb(0x3D5);
    return cursor_hw;
}

uint16_t vga_increment_cursor_position(void) {
    uint16_t cursor = vga_get_cursor_position();
    cursor++;
    if (cursor >= VGA_WIDTH * VGA_HEIGHT) {
        cursor = VGA_WIDTH * VGA_HEIGHT - 1;
    }
    vga_set_cursor(cursor);
    return cursor;
}

uint16_t vga_decrement_cursor_position(void) {
    uint16_t cursor = vga_get_cursor_position();
    if (cursor > 0) {
        cursor--;
    }
    vga_set_cursor(cursor);
    return cursor;
}