#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdlib.h>
#include <kernel/sys/ioports.h>
#include <kernel/multiboot.h>
#include <kernel/sys/gdt.h>
#include <kernel/sys/idt.h>
#include <kernel/sys/isr.h>
#include <kernel/sys/paging.h>
#include <kernel/sys/kheap.h>
#include <kernel/sys/pic.h>
#include <kernel/panic.h>
#include <kernel/sys/pit.h>

#define HEAP_MAGIC 0x12345678
#define HEAP_MIN_SIZE 0x400000
#define HEAP_SIZE_PERCENTAGE 75

static void* heap_start;
size_t heap_size;
static header_t* free_list;

size_t heap_used = 0;

size_t calculate_heap_size(multiboot_info_t* multiboot_info) {
    // Calculate available physical memory
    size_t mem_lower = multiboot_info->mem_lower << 10;
    size_t mem_upper = multiboot_info->mem_upper << 10;

    // Total memory in bytes
    size_t total_memory_kb = mem_lower + mem_upper;

    // Calculate heap size
    size_t heap_size = (total_memory_kb / 100) * HEAP_SIZE_PERCENTAGE;

    return heap_size;
}

void kheap_init(void* start, size_t size) {
    heap_start = start;
    heap_size = size;
    free_list = (header_t*)heap_start;

    free_list->size = heap_size - sizeof(header_t) - sizeof(footer_t);
    free_list->magic = HEAP_MAGIC;
    free_list->next = NULL;

    footer_t* initial_footer = (footer_t*)((char*)heap_start + heap_size - sizeof(footer_t));
    initial_footer->size = free_list->size;
    initial_footer->magic = HEAP_MAGIC;
}

void* kmalloc(size_t size) {
    if (size == 0) return NULL;

    header_t* current = free_list;
    while (current) {
        if (current->magic != HEAP_MAGIC) {
            panic("Heap corruption detected!", "kheap.c", __LINE__);
            return NULL;
        }

        if (current->size >= size) {
            if (current->size > size + sizeof(header_t) + sizeof(footer_t)) {
                header_t* new_block = (header_t*)((char*)current + sizeof(header_t) + size);
                new_block->size = current->size - size - sizeof(header_t) - sizeof(footer_t);
                new_block->magic = HEAP_MAGIC;
                new_block->next = current->next;

                footer_t* new_footer = (footer_t*)((char*)new_block + new_block->size + sizeof(header_t));
                new_footer->size = new_block->size;
                new_footer->magic = HEAP_MAGIC;

                current->size = size;
                current->next = new_block;
            }

            if (current == free_list) {
                free_list = current->next;
            } else {
                header_t* prev = free_list;
                while (prev->next != current) {
                    prev = prev->next;
                }
                prev->next = current->next;
            }

            heap_used += size;

            return (char*)current + sizeof(header_t);
        }

        current = current->next;
    }

    return NULL; 
}

void kfree(void* ptr) {
    if (ptr == NULL) return;

    header_t* header = (header_t*)((char*)ptr - sizeof(header_t));
    footer_t* footer = (footer_t*)((char*)ptr + header->size);

    if (header->magic != HEAP_MAGIC || footer->magic != HEAP_MAGIC) {
        panic("Heap corruption detected!", "kheap.c", 92);
    }

    heap_used -= header->size;

    header->next = free_list;
    free_list = header;
}

void kheap_dump() {
    header_t* hdr = (void*)heap_start;

    do {
        hdr = hdr->next;
    } while(hdr);
}
