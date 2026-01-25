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

// Resets cursor to the top-left corner
void vga_reset_cursor(void) {
    vga_set_cursor(0);
}
