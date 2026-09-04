#include <kernel/terminal.h>
#include <kernel/autorun.h>
#include <system/state.h>
#include <vga.h>
#include <ioport.h>
#include <driver/sound.h>
#include <driver/ata/ata.h>
#include <file_system/ipo_fs.h>
#include <kernel/process.h>
#include <syscall.h>
#include <system/timer.h>
#include <kernel/driver.h>
#include <driver/input/mouse.h>
#include <net/net.h>
#include <stdio.h>

#define FS_START_LBA (uint32_t)2048
#define STARTUP_SOUND {NOTE_C6, 150}, {NOTE_E6, 150}, {NOTE_G6, 150}, {NOTE_C7, 200}

static void ensure_fs_mounted(void) {
    if (ipo_fs_mount(FS_START_LBA) || ipo_fs_mount(0)) {
        printf("Mounted IPO_FS\n");
        return;
    }

    printf("No IPO_FS found, attempting dynamic format...\n");

    uint64_t pool_capacity = ata_get_pool_capacity();
    if (pool_capacity == 0) {
        printf("Disk not detected: running in diskless mode\n");
        return;
    }

    /* Disk too small (< 10 sectors / ~5 KB) */
    if (pool_capacity < 10) {
        printf("Disk too small for filesystem (< 5 KB)\n");
        return;
    }

    /* Adaptive partition offset:
     * - Disks > 1 MB use standard partition offset FS_START_LBA (2048)
     * - Disks <= 1 MB use LBA 0 (whole-disk / floppy mode) */
    uint64_t start_lba = 0;
    uint64_t available_sectors = pool_capacity;
    if (pool_capacity > FS_START_LBA + 10) {
        start_lba = FS_START_LBA;
        available_sectors -= FS_START_LBA;
    }

    uint64_t total_blocks = available_sectors;
    uint64_t total_inodes = total_blocks / 16;
    if (total_inodes == 0) total_inodes = 1;

    printf("Storage Pool: %u sectors.\nFormatting %u blocks (%u inodes) at LBA %u...\n",
           (uint32_t)pool_capacity, (uint32_t)total_blocks, (uint32_t)total_inodes, (uint32_t)start_lba);

    if (!ipo_fs_format(start_lba, total_blocks, total_inodes)) {
        printf("ipo_fs_format failed\n");
    } else if (!ipo_fs_mount(start_lba)) {
        printf("ipo_fs_mount failed after format\n");
    } else {
        printf("Mounted IPO_FS at LBA %u\n", (uint32_t)start_lba);
    }
}

static void play_note_smooth(uint16_t freq, uint16_t duration_ms) {
    if (freq == NOTE_REST) {
        sound_stop();
    } else {
        sound_play(freq);
    }

    for (volatile uint32_t i = 0; i < duration_ms * 10000; i++) {
        io_wait();
    }

    sound_stop();

    for (volatile uint32_t i = 0; i < 20000; i++) io_wait();
}

void play_startup_sound(void) {
    typedef struct {
        uint16_t note;
        uint16_t duration;
    } NoteDuration;

    NoteDuration startup_sound[] = { STARTUP_SOUND };
    size_t notes_count = sizeof(startup_sound) / sizeof(startup_sound[0]);

    for (size_t i = 0; i < notes_count; i++) {
        play_note_smooth(startup_sound[i].note, startup_sound[i].duration);
    }
}


void kmain(void) {
    system_set_state(SYSTEM_STATE_BOOT);

    terminal_initialize();

    process_init();

    syscall_init();

    timer_init();

    async_scheduler_init();

    sound_init();
    
    ata_init();

    ipo_fs_init();

    ensure_fs_mounted();

    vga_load_cyrillic_font("/system/fonts.bin");

    play_startup_sound();

    terminal_initialize();

    mouse_init();

    net_init();

    printf("Type \"help\" or \"?\" on the keyboard to view commands, shortcuts, and OS features.\n\n");

    system_set_state(SYSTEM_STATE_TERMINAL_IDLE);

    autorun_init();
    
    for (;;) {
        timer_tick();
        async_scheduler_tick();
        net_poll();
        terminal_console();
    }
}
