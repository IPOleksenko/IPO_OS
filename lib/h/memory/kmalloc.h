#ifndef LIB_MEM_KMALLOC_H
#define LIB_MEM_KMALLOC_H

#include <stddef.h>
#include <stdint.h>

void* kmalloc(size_t size);

void kfree(void* ptr);

void kmalloc_init(void);

#endif // LIB_MEM_KMALLOC_H
