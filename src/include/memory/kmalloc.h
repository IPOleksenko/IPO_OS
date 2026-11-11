#ifndef LIB_MEM_KMALLOC_H
#define LIB_MEM_KMALLOC_H

#include <stddef.h>
#include <stdint.h>

#define KMALLOC_HEAP_START  (0x4000000u)  // 64 MiB kernel heap start
size_t kmalloc_get_heap_capacity(void);
#define KMALLOC_HEAP_SIZE   (kmalloc_get_heap_capacity())

void* kmalloc(size_t size);

void* krealloc(void* ptr, size_t size);

void kfree(void* ptr);

void kmalloc_init(void);

typedef struct {
    size_t heap_total;    /* total heap capacity in bytes */
    size_t heap_used;     /* bytes consumed by heap_used watermark (allocated + headers) */
    size_t alloc_bytes;   /* user bytes currently allocated */
    size_t free_bytes;    /* user bytes in free blocks */
    size_t alloc_blocks;  /* number of allocated blocks */
    size_t free_blocks;   /* number of free blocks */
    size_t block_header;  /* size of one block header */
} kmalloc_stats_t;

void kmalloc_get_stats(kmalloc_stats_t *out);

#endif // LIB_MEM_KMALLOC_H
