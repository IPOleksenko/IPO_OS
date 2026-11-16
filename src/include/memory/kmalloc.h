#ifndef LIB_MEM_KMALLOC_H
#define LIB_MEM_KMALLOC_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <types.h>

#define KMALLOC_HEAP_START  ((ipo_vaddr_t)0x4000000u)  /* 64 MiB */
size_t kmalloc_get_heap_capacity(void);

#define KMALLOC_HEAP_SIZE   (kmalloc_get_heap_capacity())

void* kmalloc(size_t size);

void* krealloc(void* ptr, size_t size);

/* Overflow-checked geometric array growth (zeroes grown tail) */
bool kgrow_array(void **ptr, size_t *cap, size_t min_cap, size_t elem_size, size_t init_cap);

void kfree(void* ptr);

void kmalloc_init(void);

typedef struct {
    size_t heap_total;
    size_t heap_used;     /* high-water mark (allocations + headers) */
    size_t alloc_bytes;
    size_t free_bytes;
    size_t alloc_blocks;
    size_t free_blocks;
    size_t block_header;
} kmalloc_stats_t;

void kmalloc_get_stats(kmalloc_stats_t *out);

#endif
