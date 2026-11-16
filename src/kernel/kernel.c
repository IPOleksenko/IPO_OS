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
#include <system/rtc.h>
#include <driver/input/mouse.h>
#include <memory/memory_map.h>
#include <memory/address_space.h>
#include <kernel/smp.h>
#include <kernel/device.h>
#include <kernel/driver.h>
#include <kernel/irq.h>
#include <kernel/input_ext.h>
#include <kernel/ext.h>
#include <kernel/dynmod.h>
#include <driver/storage/block_dev.h>
#include <storage/storage_pool.h>
#include <kernel/kmod.h>
#include <kernel/utf.h>
#include <kernel/cpu_info.h>
#include <vga_gfx.h>
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

void kmain(ipo_paddr_t handover_paddr) {
    arch_cpu_backend_init();
    serial_init();
    system_set_state(SYSTEM_STATE_BOOT);

    terminal_initialize();
    vga_gfx_init();

    const ipo_boot_handover_t *handover =
        (handover_paddr != 0u) ? (const ipo_boot_handover_t *)(uintptr_t)handover_paddr : NULL;
    if (handover) {
        memory_map_init_handover(handover);
    } else {
        memory_map_init();
    }

    cpu_info_init(handover);

    utf_init();

    process_init();

    syscall_init();
    irq_init();

    timer_init();
    rtc_init();

    smp_init();

    async_scheduler_init();

    sound_init();
    audio_init();
    
    block_dev_init();

    ata_init();

    storage_pool_init();

    kmod_init();
    ipo_ext_init();
    dynmod_init();
    
    mouse_init();

    device_init();
    input_ext_init();

    paging_init();
    syscall_update_exception_page_directory();
    cpu_info_get_summary();
    cpu_info_print_boot_summary();

    ipo_fs_init();

    ensure_fs_mounted();

    __asm__ volatile("sti" ::: "memory");

    play_startup_sound();
    terminal_clear_buffer();
    terminal_clear_command_history();
    printf("Type \"help\" on the keyboard to view commands, shortcuts, and OS features.\n\n");

    system_set_state(SYSTEM_STATE_TERMINAL_IDLE);

    autorun_init();
    
    for (;;) {
        timer_tick();
        rtc_poll();
        async_scheduler_tick();
        driver_dispatch_tick();
        ipo_chan_poll_all();
        smp_poll_local_tasks();
        terminal_console();
    }
}
