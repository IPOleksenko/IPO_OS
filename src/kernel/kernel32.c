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

#define STARTUP_SOUND {NOTE_C6, 150}, {NOTE_E6, 150}, {NOTE_G6, 150}, {NOTE_C7, 200}

static void play_note_smooth(uint16_t freq, uint16_t duration_ms) {
    if (freq == NOTE_REST) {
        sound_stop();
    } else {
        sound_play(freq);
    }

    for (volatile uint32_t i = 0; i < duration_ms * 10000; i++) {
        io_wait(); // busy-wait loop
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
    terminal_initialize();
    
    process_init();

    sound_init();
    
    ata_init();

    ipo_fs_init();

    ensure_fs_mounted();

    process_init();

    play_startup_sound();

    terminal_initialize();

    autorun_init();

    for (;;) {
        terminal_console();
    }
}
