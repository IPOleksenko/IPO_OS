#include <stdio.h>
#include <string.h>

#include <kernel/drv/tty.h>
#include <kernel/drv/keyboard.h>
#include <kernel/sys/gdt.h>
#include <kernel/sys/idt.h>
#include <kernel/sys/kheap.h>
#include <kernel/sys/fs.h>
#include <kernel/drv/ata.h>
#include <kernel/sys/pic.h>
#include <kernel/sys/pit.h>
#include <kernel/sys/paging.h>
#include <kernel/sys/isr.h>
#include <kernel/multiboot.h>
#include <kernel/apps/handle_command.h>

#define HEAP_START_ADDRESS 0x1000000

void irq_disable();
void irq_enable();

extern size_t heap_used;
extern size_t heap_size;

void kernel_main(__attribute__((unused)) multiboot_info_t* multiboot_info) {
	terminal_initialize();
    
    // Calculate heap size
    size_t heap_size = calculate_heap_size(multiboot_info);

    // Initialize heap
    kheap_init((void*)HEAP_START_ADDRESS, heap_size);
    
    copyright_text();

    irq_disable();
    
    gdt_init();
    idt_init(GDT_CODE_SEL_1);

    pic_init();
    timer_init();
    
    // Initialize ATA driver
    ata_init();
    
    // Initialize filesystem
    fs_init();

    irq_enable();
	sleep(200);
	terminal_clear();

	copyright_text();
	
	while (1) {
        handle_command(keyboard_input());
    }
}

