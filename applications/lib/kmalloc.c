#include <memory/kmalloc.h>
#include <stdlib.h>
#include <string.h>

void kmalloc_init(void) {
}

void *kmalloc(size_t size) {
    void *ptr = malloc(size);
    if (ptr && size > 0) {
        memset(ptr, 0, size);
    }
    return ptr;
}

void kfree(void *ptr) {
    free(ptr);
}

void *krealloc(void *ptr, size_t size) {
    return realloc(ptr, size);
}

void kmalloc_get_stats(kmalloc_stats_t *out) {
    if (out) {
        memset(out, 0, sizeof(kmalloc_stats_t));
    }
}

