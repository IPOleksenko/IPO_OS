#include <kernel/terminal.h>
#include <vga.h>
#include <ioport.h>
#include <driver/sound.h>
#include <driver/ata/ata.h>
#include <stdio.h>

void kmain(void) {
    terminal_initialize();
    sound_init();
    
    ata_init();
    ata_print_devices();
    
    sound_beep(1000, 200);

    for (;;) {
        terminal_console();
    }
}
