#include <kernel/terminal.h>

void kmain(void) {
    terminal_initialize();

    for (;;) asm volatile("hlt");
}
