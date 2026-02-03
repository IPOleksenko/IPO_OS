#include <kernel/terminal.h>
#include <kernel/autorun.h>
#include <vga.h>
#include <ioport.h>
#include <driver/sound.h>
#include <driver/ata/ata.h>
#include <file_system/ipo_fs.h>
#include <kernel/process.h>
#include <stdio.h>

#define FS_START_LBA (uint32_t)2048

static void ensure_fs_mounted(void) {
    if (ipo_fs_mount(FS_START_LBA)) {
        printf("Mounted IPO_FS at LBA %u\n", FS_START_LBA);
    } else {
        printf("No IPO_FS at LBA %u, formatting...\n", FS_START_LBA);
        if (!ipo_fs_format(FS_START_LBA, 10000, 1024)) {
            printf("ipo_fs_format failed\n");
        } else if (!ipo_fs_mount(FS_START_LBA)) {
            printf("ipo_fs_mount failed after format\n");
        } else {
            printf("Mounted IPO_FS at LBA %u\n", FS_START_LBA);
        }
    }
}

void kmain(void) {
    terminal_initialize();
    
    process_init();

    sound_init();
    
    ata_init();

    ipo_fs_init();

    ensure_fs_mounted();

    sound_beep(1000, 200);

    autorun_init();

    for (;;) {
        terminal_console();
    }
}
