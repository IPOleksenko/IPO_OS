#include <kernel/terminal.h>
#include <vga.h>
#include <ioport.h>

void kmain(void) {
    terminal_initialize();
    
    for (;;) {
        terminal_console();
    }
}
