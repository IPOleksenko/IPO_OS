#include <kernel/terminal.h>
#include <vga.h>
#include <ioport.h>
#include <driver/sound.h>
#include <driver/ata/ata.h>
#include <file_system/ipo_fs.h>
#include <stdio.h>

static void ensure_fs_mounted(uint32_t fs_start) {
    if (ipo_fs_mount(fs_start)) {
        printf("Mounted IPO_FS at LBA %u\n", fs_start);
    } else {
        printf("No IPO_FS at LBA %u, formatting...\n", fs_start);
        if (!ipo_fs_format(fs_start, 10000, 1024)) {
            printf("ipo_fs_format failed\n");
        } else if (!ipo_fs_mount(fs_start)) {
            printf("ipo_fs_mount failed after format\n");
        } else {
            printf("Mounted IPO_FS at LBA %u\n", fs_start);
        }
    }
}

void kmain(void) {
    terminal_initialize();

    sound_init();
    
    ata_init();
    ata_print_devices();

    ipo_fs_init();
    uint32_t fs_start = 2048;
    ensure_fs_mounted(fs_start);

    sound_beep(1000, 200);

    for (;;) {
        terminal_console();
    }
}
