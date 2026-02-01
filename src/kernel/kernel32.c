#include <kernel/terminal.h>
#include <vga.h>
#include <ioport.h>
#include <driver/sound.h>

void kmain(void) {
    terminal_initialize();
    sound_init();
    
    sound_beep(10000, 2000);
    
    for (;;) {
        terminal_console();
    }
}
