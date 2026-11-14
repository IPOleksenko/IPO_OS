#include <memory/kmalloc.h>
#include <memory/memory_map.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <kernel/smp.h>

static spinlock_t s_kmalloc_lock = {0};

#define KMALLOC_MAGIC       (0xDEADBEEF)
#define KMALLOC_FREED_MAGIC (0xDEADC0DE)

size_t kmalloc_get_heap_capacity(void) {
    uint32_t magic = *(volatile uint32_t *)E820_MAGIC_ADDR;
    uint32_t raw_count = *(volatile uint32_t *)E820_COUNT_ADDR;
    uint32_t ptr_val = *(volatile uint32_t *)E820_PTR_ADDR;
    const uint64_t heap_start = KMALLOC_HEAP_START;
    const uint64_t heap_limit = 0x08000000ull;

    if (magic == E820_MAGIC_VAL && raw_count > 0 && ptr_val != 0) {
        const e820_entry_t *entries = (const e820_entry_t *)(uintptr_t)ptr_val;
        uint64_t covered_end = heap_start;
        bool advanced;
        do {
            advanced = false;
            for (uint32_t i = 0; i < raw_count; i++) {
                if (entries[i].type != E820_TYPE_USABLE ||
                    entries[i].base > covered_end ||
                    entries[i].base + entries[i].length <= covered_end) {
                    continue;
                }
                covered_end = entries[i].base + entries[i].length;
                if (covered_end > heap_limit) covered_end = heap_limit;
                advanced = true;
            }
        } while (advanced && covered_end < heap_limit);
        return covered_end > heap_start
            ? (size_t)(covered_end - heap_start)
            : 0u;
    }

    uint64_t fallback_ram = *(volatile uint32_t *)E820_RAM_LOW_ADDR;
    if (fallback_ram <= heap_start) return 0u;
    uint64_t heap_end = fallback_ram < heap_limit ? fallback_ram : heap_limit;
    return (size_t)(heap_end - heap_start);
}

typedef struct {
    size_t size;           // Total size including this header
    uint32_t magic;        // Magic number for validation
    uint32_t is_free;      // 1 if free, 0 if allocated
    uint32_t padding;      // 16-byte alignment
} kmalloc_block_t;

#define BLOCK_HEADER_SIZE (sizeof(kmalloc_block_t))

// Kernel heap state
static uint8_t *heap_start = NULL;
static size_t heap_used = 0;

/**
 * Initialize kernel allocator
 */
void kmalloc_init(void) {
    heap_start = (uint8_t *)KMALLOC_HEAP_START;
    heap_used = 0;
}

/**
 * Coalesce adjacent free blocks and shrink the heap tail
 */
static void kmalloc_coalesce_and_shrink(void) {
    if (heap_start == NULL || heap_used == 0) return;

    uint8_t *ptr = heap_start;
    kmalloc_block_t *last_free = NULL;

    while ((size_t)(ptr - heap_start) < heap_used) {
        kmalloc_block_t *block = (kmalloc_block_t *)ptr;
        if (block->magic != KMALLOC_MAGIC && block->magic != KMALLOC_FREED_MAGIC) {
            break;  // Heap corruption guard
        }
        if (block->size < BLOCK_HEADER_SIZE) {
            break;
        }

        if (block->is_free) {
            if (last_free != NULL) {
                // Merge current free block into last_free
                last_free->size += block->size;
            } else {
                last_free = block;
            }
        } else {
            last_free = NULL;
        }

        ptr += block->size;
    }

    // Shrink heap_used if trailing block(s) at the top of the heap are free
    if (last_free != NULL && ((uint8_t *)last_free + last_free->size == heap_start + heap_used)) {
        heap_used = (size_t)((uint8_t *)last_free - heap_start);
    }
}

/**
 * Find or create a free block suitable for allocation
 */
static kmalloc_block_t* find_free_block(size_t size) {
    kmalloc_coalesce_and_shrink();

    uint8_t *ptr = heap_start;

    while ((size_t)(ptr - heap_start) < heap_used) {
        kmalloc_block_t *block = (kmalloc_block_t *)ptr;

        // Validate block
        if (block->magic != KMALLOC_MAGIC && block->magic != KMALLOC_FREED_MAGIC) {
            return NULL;  // Heap corruption
        }

        if (block->size < BLOCK_HEADER_SIZE) {
            return NULL;
        }

        // Check if this free block is large enough
        if (block->is_free && block->size >= (size + BLOCK_HEADER_SIZE)) {
            return block;
        }

        ptr += block->size;
    }

    return NULL;
}

/**
 * Split a free block if it's larger than needed
 */
static void split_block(kmalloc_block_t *block, size_t needed_size) {
    size_t total_needed = (needed_size + BLOCK_HEADER_SIZE + 15u) & ~15u;

    if (block->size < total_needed + BLOCK_HEADER_SIZE + 16u) {
        return;
    }

    size_t remainder_size = block->size - total_needed;
    kmalloc_block_t *remainder = (kmalloc_block_t *)(
        (uint8_t *)block + total_needed
    );

    remainder->size = remainder_size;
    remainder->magic = KMALLOC_FREED_MAGIC;
    remainder->is_free = 1;
    remainder->padding = 0;

    block->size = total_needed;
}

static size_t kmalloc_align_size(size_t size) {
    if (size == 0 || size > SIZE_MAX - 15u) {
        return 0;
    }
    return (size + 15u) & ~15u;
}

static void *kmalloc_locked(size_t aligned_size, size_t *out_user_size) {
    if (aligned_size == 0 || aligned_size > SIZE_MAX - BLOCK_HEADER_SIZE - 15u) {
        return NULL;
    }

    if (heap_start == NULL) {
        kmalloc_init();
    }

    kmalloc_block_t *block = find_free_block(aligned_size);
    if (block == NULL) {
        size_t total_needed = (aligned_size + BLOCK_HEADER_SIZE + 15u) & ~15u;
        size_t heap_cap = KMALLOC_HEAP_SIZE;

        if (total_needed > heap_cap || heap_used > heap_cap - total_needed) {
            return NULL;
        }

        block = (kmalloc_block_t *)(heap_start + heap_used);
        block->size = total_needed;
        block->magic = KMALLOC_MAGIC;
        block->is_free = 0;
        block->padding = 0;

        heap_used += total_needed;
    } else {
        split_block(block, aligned_size);
        block->magic = KMALLOC_MAGIC;
        block->is_free = 0;
    }

    if (out_user_size) {
        *out_user_size = block->size > BLOCK_HEADER_SIZE
                             ? (block->size - BLOCK_HEADER_SIZE)
                             : aligned_size;
    }
    return (void *)((uint8_t *)block + BLOCK_HEADER_SIZE);
}

static void kfree_locked(void *ptr) {
    if (ptr == NULL || heap_start == NULL) {
        return;
    }

    uintptr_t p = (uintptr_t)ptr;
    uintptr_t start = (uintptr_t)heap_start;
    uintptr_t end = start + heap_used;
    if (p < start + BLOCK_HEADER_SIZE || p > end) {
        return;
    }

    kmalloc_block_t *block = (kmalloc_block_t *)ptr - 1;
    if (block->magic != KMALLOC_MAGIC || block->is_free) {
        return;
    }

    block->is_free = 1;
    block->magic = KMALLOC_FREED_MAGIC;
    kmalloc_coalesce_and_shrink();
}

void *kmalloc(size_t size) {
    size_t aligned_size = kmalloc_align_size(size);
    if (aligned_size == 0) {
        return NULL;
    }

    uint32_t irq_flags = spin_lock_irqsave(&s_kmalloc_lock);
    void *user_ptr = kmalloc_locked(aligned_size, NULL);
    spin_unlock_irqrestore(&s_kmalloc_lock, irq_flags);

    if (user_ptr) {
        memset(user_ptr, 0, aligned_size);
    }
    return user_ptr;
}

void kfree(void *ptr) {
    if (ptr == NULL) {
        return;
    }

    uint32_t irq_flags = spin_lock_irqsave(&s_kmalloc_lock);
    kfree_locked(ptr);
    spin_unlock_irqrestore(&s_kmalloc_lock, irq_flags);
}

void *krealloc(void *ptr, size_t size) {
    if (ptr == NULL) {
        return kmalloc(size);
    }
    if (size == 0) {
        kfree(ptr);
        return NULL;
    }

    size_t aligned_size = kmalloc_align_size(size);
    if (aligned_size == 0) {
        return NULL;
    }

    uint32_t irq_flags = spin_lock_irqsave(&s_kmalloc_lock);
    if (heap_start == NULL) {
        spin_unlock_irqrestore(&s_kmalloc_lock, irq_flags);
        return NULL;
    }

    uintptr_t p = (uintptr_t)ptr;
    uintptr_t start = (uintptr_t)heap_start;
    uintptr_t end = start + heap_used;
    if (p < start + BLOCK_HEADER_SIZE || p > end) {
        spin_unlock_irqrestore(&s_kmalloc_lock, irq_flags);
        return NULL;
    }

    kmalloc_block_t *block = (kmalloc_block_t *)ptr - 1;
    if (block->magic != KMALLOC_MAGIC || block->is_free) {
        spin_unlock_irqrestore(&s_kmalloc_lock, irq_flags);
        return NULL;
    }

    size_t old_user_size = block->size > BLOCK_HEADER_SIZE ? (block->size - BLOCK_HEADER_SIZE) : 0;
    if (size <= old_user_size) {
        spin_unlock_irqrestore(&s_kmalloc_lock, irq_flags);
        return ptr;
    }

    size_t new_user_size = 0;
    void *new_ptr = kmalloc_locked(aligned_size, &new_user_size);
    if (!new_ptr) {
        spin_unlock_irqrestore(&s_kmalloc_lock, irq_flags);
        return NULL;
    }

    memcpy(new_ptr, ptr, old_user_size);
    if (new_user_size > old_user_size) {
        memset((uint8_t *)new_ptr + old_user_size, 0, new_user_size - old_user_size);
    }
    kfree_locked(ptr);
    spin_unlock_irqrestore(&s_kmalloc_lock, irq_flags);
    return new_ptr;
}

void kmalloc_get_stats(kmalloc_stats_t *out) {
    if (out == NULL) return;

    uint32_t irq_flags = spin_lock_irqsave(&s_kmalloc_lock);

    out->heap_total   = KMALLOC_HEAP_SIZE;
    out->heap_used    = heap_used;
    out->alloc_bytes  = 0;
    out->free_bytes   = 0;
    out->alloc_blocks = 0;
    out->free_blocks  = 0;
    out->block_header = BLOCK_HEADER_SIZE;

    if (heap_start == NULL) {
        spin_unlock_irqrestore(&s_kmalloc_lock, irq_flags);
        return;
    }

    uint8_t *ptr = heap_start;
    while ((size_t)(ptr - heap_start) < heap_used) {
        kmalloc_block_t *block = (kmalloc_block_t *)ptr;
        if (block->magic != KMALLOC_MAGIC && block->magic != KMALLOC_FREED_MAGIC) {
            break;
        }
        if (block->size < BLOCK_HEADER_SIZE) {
            break;
        }
        size_t user_size = block->size > BLOCK_HEADER_SIZE
                           ? block->size - BLOCK_HEADER_SIZE : 0;
        if (block->is_free) {
            out->free_bytes  += user_size;
            out->free_blocks += 1;
        } else {
            out->alloc_bytes  += user_size;
            out->alloc_blocks += 1;
        }
        ptr += block->size;
    }

    spin_unlock_irqrestore(&s_kmalloc_lock, irq_flags);
}
