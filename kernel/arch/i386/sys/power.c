#include <kernel/sys/ioports.h>
#include <kernel/sys/pit.h>

#include <kernel/sys/power.h>


void shutdown_system() {
    printf("Shutting down...\n");
    sleep(1000);
    outw(0x604, 0x2000); // ACPI shutdown
}

void reboot_system(){
    printf("Rebooting...\n");
    sleep(1000);
    outb(0x64, 0xFE);
}