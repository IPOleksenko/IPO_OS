#include <kernel/terminal.h>
#include <kernel/autorun.h>
#include <system/state.h>
#include <vga.h>
#include <vga_gfx.h>
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
#include <memory/memory_map.h>
#include <kernel/smp.h>
#include <kernel/device.h>
#include <driver/storage/block_dev.h>
#include <storage/storage_pool.h>
#include <kernel/kmod.h>
#include <stdio.h>

#define FS_START_LBA (uint32_t)2048
#define STARTUP_SOUND {NOTE_C6, 150}, {NOTE_E6, 150}, {NOTE_G6, 150}, {NOTE_C7, 200}

static void ensure_fs_mounted(void) {
    storage_pool_state_t pool_state = storage_pool_get_state();
    if (pool_state == STORAGE_POOL_STATE_CORRUPT) {
        printf("[ipo_fs] Storage pool metadata is CORRUPTED. Automatic formatting ABORTED to prevent data loss.\n");
        return;
    }

    if (ipo_fs_mount(FS_START_LBA) || ipo_fs_mount(0)) {
        if (pool_state == STORAGE_POOL_STATE_DEGRADED) {
            printf("[ipo_fs] Mounted IPO_FS in DEGRADED storage pool mode.\n");
        } else {
            printf("[ipo_fs] Mounted IPO_FS\n");
        }
        return;
    }

    if (pool_state == STORAGE_POOL_STATE_DEGRADED) {
        printf("[ipo_fs] Storage pool is DEGRADED. Automatic formatting ABORTED to protect surviving data.\n");
        return;
    }

    printf("[ipo_fs] No IPO_FS found, attempting dynamic format...\n");

    uint64_t pool_capacity = storage_pool_get_capacity();
    if (pool_capacity == 0) {
        pool_capacity = ata_get_pool_capacity();
    }
    if (pool_capacity == 0) {
        printf("[ipo_fs] Disk not detected: running in diskless mode\n");
        return;
    }

    /* Disk too small (< 10 sectors / ~5 KB) */
    if (pool_capacity < 10) {
        printf("[ipo_fs] Disk too small for filesystem (< 5 KB)\n");
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
        printf("[ipo_fs] Mounted IPO_FS at LBA %u\n", (uint32_t)start_lba);
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

    printf("[kernel] Initializing memory map (BIOS E820)...\n");
    memory_map_init();
    printf("[kernel] Memory map initialized.\n");

    printf("[kernel] Initializing process manager...\n");
    process_init();
    printf("[kernel] Process manager initialized.\n");

    printf("[kernel] Initializing system calls...\n");
    syscall_init();
    printf("[kernel] System calls initialized.\n");

    printf("[kernel] Initializing timer & PIT...\n");
    timer_init();
    printf("[kernel] Timer initialized.\n");

    printf("[kernel] Initializing Symmetric Multiprocessing (SMP)...\n");
    smp_init();
    printf("[kernel] SMP initialization complete.\n");

    printf("[kernel] Initializing async scheduler...\n");
    async_scheduler_init();
    printf("[kernel] Async scheduler initialized.\n");

    printf("[kernel] Initializing sound...\n");
    sound_init();
    printf("[kernel] Sound initialized.\n");
    
    printf("[kernel] Initializing block devices...\n");
    block_dev_init();
    printf("[kernel] Block devices initialized.\n");

    printf("[kernel] Initializing ATA storage...\n");
    ata_init();
    printf("[kernel] ATA storage initialized.\n");

    printf("[kernel] Initializing unified storage pool...\n");
    storage_pool_init();
    printf("[kernel] Unified storage pool initialized.\n");

    printf("[kernel] Initializing dynamic driver subsystem (kmod)...\n");
    kmod_init();
    printf("[kernel] Dynamic driver subsystem initialized.\n");
    
    printf("[kernel] Saving text mode hardware state...\n");
    vga_save_text_state();
    printf("[kernel] Text mode hardware state saved.\n");

    printf("[kernel] Initializing PS/2 mouse...\n");
    mouse_init();
    printf("[kernel] PS/2 mouse initialized.\n");

    const net_config_t net_config = {
        .icmp_queue_capacity = 8,
        .arp_table_capacity = 16,
        .udp_sockets_capacity = 16,
        .dns_queue_capacity = 8,
        .loopback_queue_capacity = 4
    };
    if (!net_init(&net_config)) {
        printf("[net] Failed to initialize network buffers\n");
    }

    printf("[kernel] Initializing system drivers...\n");
    driver_init_system_drivers();
    printf("[kernel] System drivers initialized.\n");

    printf("[kernel] Initializing unified device registry...\n");
    device_init();
    printf("[kernel] Unified device registry initialized.\n");

    printf("[kernel] Initializing IPO_FS internal state...\n");
    ipo_fs_init();
    printf("[kernel] IPO_FS internal state initialized.\n");

    printf("[kernel] Mounting filesystem...\n");
    ensure_fs_mounted();
    printf("[kernel] Filesystem mount check complete.\n");

    /* Automatically load system console font database if available */
    vga_load_cyrillic_font("/fonts/default.fnt");

    play_startup_sound();
    terminal_initialize();
    printf("Type \"help\" on the keyboard to view commands, shortcuts, and OS features.\n\n");

    system_set_state(SYSTEM_STATE_TERMINAL_IDLE);

    autorun_init();
    
    for (;;) {
        timer_tick();
        async_scheduler_tick();
        net_poll();
        terminal_console();
    }
}
