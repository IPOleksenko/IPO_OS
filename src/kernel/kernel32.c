#include <kernel/terminal.h>
#include <kernel/autorun.h>
#include <kernel/async.h>
#include <system/state.h>
#include <driver/sound.h>
#include <driver/audio_core.h>
#include <driver/ata/ata.h>
#include <file_system/ipo_fs.h>
#include <kernel/process.h>
#include <syscall.h>
#include <system/timer.h>
#include <driver/input/mouse.h>
#include <net/net.h>
#include <memory/memory_map.h>
#include <memory/address_space.h>
#include <kernel/smp.h>
#include <kernel/device.h>
#include <kernel/irq.h>
#include <kernel/input_ext.h>
#include <driver/storage/block_dev.h>
#include <storage/storage_pool.h>
#include <kernel/kmod.h>
#include <kernel/utf.h>
#include <stdio.h>

#define FS_START_LBA (uint32_t)2048

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
    if (!ipo_fs_install()) {
        printf("[ipo_fs] Initial filesystem installation failed\n");
    }
}

void kmain(void) {
    serial_init();
    system_set_state(SYSTEM_STATE_BOOT);

    terminal_initialize();

    memory_map_init();

    utf_init();

    process_init();

    syscall_init();
    irq_init();

    timer_init();

    smp_init();

    async_scheduler_init();

    sound_init();
    audio_init();
    
    block_dev_init();

    ata_init();

    storage_pool_init();

    kmod_init();
    
    mouse_init();

    net_init();

    device_init();
    input_ext_init();

    paging_init();
    syscall_update_exception_page_directory();

    ipo_fs_init();

    ensure_fs_mounted();

    play_startup_sound();
    terminal_clear_buffer();
    terminal_clear_command_history();
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
