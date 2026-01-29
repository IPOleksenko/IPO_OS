#include <driver/keyboard.h>

#include <vga.h>
#include <ioport.h>

uint8_t keyboard_get_scancode(void) {
    uint8_t status = inb(KBD_STATUS_PORT);
    if (!(status & KBD_STATUS_OUTPUT_BUFFER))
        return 0x00;
    
    uint8_t scancode = inb(KBD_DATA_PORT);
    return scancode;
}
