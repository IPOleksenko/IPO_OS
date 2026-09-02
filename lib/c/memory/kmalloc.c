#include <memory/kmalloc.h>
#include <stdint.h>
#include <string.h>

#define KMALLOC_HEAP_START  (0x1000000)  // 16 MB (arbitrary kernel heap start)
#define KMALLOC_HEAP_SIZE   (0x4000000)  // 64 MB max heap
#define KMALLOC_MAGIC       (0xDEADBEEF)
#define KMALLOC_FREED_MAGIC (0xDEADC0DE)

typedef struct {
    size_t size;           // Total size including this header
    uint32_t magic;        // Magic number for validation
    uint8_t is_free;       // 1 if free, 0 if allocated
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
 * Find or create a free block suitable for allocation
 */
static kmalloc_block_t* find_free_block(size_t size) {
    // Simple linear scan through allocated blocks
    uint8_t *ptr = heap_start;
    
    while ((size_t)(ptr - heap_start) < heap_used) {
        kmalloc_block_t *block = (kmalloc_block_t *)ptr;
        
        // Validate block
        if (block->magic != KMALLOC_MAGIC && block->magic != KMALLOC_FREED_MAGIC) {
            return NULL;  // Heap corruption
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
    size_t total_needed = needed_size + BLOCK_HEADER_SIZE;
    
    // Remainder must be able to hold a full block header plus at least 4 bytes of data
    if (block->size < total_needed + BLOCK_HEADER_SIZE + 4u) {
        return;  // Not enough space to split safely
    }
    
    // Create new free block for the remainder
    size_t remainder_size = block->size - total_needed;
    kmalloc_block_t *remainder = (kmalloc_block_t *)(
        (uint8_t *)block + total_needed
    );
    
    remainder->size = remainder_size;
    remainder->magic = KMALLOC_FREED_MAGIC;
    remainder->is_free = 1;
    
    block->size = total_needed;
}

/**
 * Allocate kernel memory
 */
void* kmalloc(size_t size) {
    if (size == 0) {
        return NULL;
    }
    
    if (heap_start == NULL) {
        kmalloc_init();
    }
    
    // Try to find an existing free block
    kmalloc_block_t *block = find_free_block(size);
    
    if (block == NULL) {
        // No suitable free block; allocate from heap end
        size_t total_needed = size + BLOCK_HEADER_SIZE;
        
        if (heap_used + total_needed >= KMALLOC_HEAP_SIZE) {
            return NULL;  // Out of memory
        }
        
        block = (kmalloc_block_t *)(heap_start + heap_used);
        block->size = total_needed;
        block->magic = KMALLOC_MAGIC;
        block->is_free = 0;
        
        heap_used += total_needed;
    } else {
        // Use existing free block
        split_block(block, size);
        block->magic = KMALLOC_MAGIC;
        block->is_free = 0;
    }
    
    // Zero-initialize the allocated memory
    void *user_ptr = (void *)((uint8_t *)block + BLOCK_HEADER_SIZE);
    memset(user_ptr, 0, size);
    
    return user_ptr;
}

/**
 * Free previously allocated memory
 */
void kfree(void* ptr) {
    if (ptr == NULL) {
        return;
    }
    
    // Get the block header (located before user data)
    kmalloc_block_t *block = (kmalloc_block_t *)ptr - 1;
    
    // Validate block
    if (block->magic != KMALLOC_MAGIC) {
        return;  // Invalid pointer or already freed
    }
    
    if (block->is_free) {
        return;  // Already freed
    }
    
    // Mark as free
    block->is_free = 1;
    block->magic = KMALLOC_FREED_MAGIC;
}

/**
 * kmalloc_get_stats - fills stat structure with heap usage info
 */
void kmalloc_get_stats(kmalloc_stats_t *out) {
    if (out == NULL) return;

    out->heap_total   = KMALLOC_HEAP_SIZE;
    out->heap_used    = heap_used;
    out->alloc_bytes  = 0;
    out->free_bytes   = 0;
    out->alloc_blocks = 0;
    out->free_blocks  = 0;
    out->block_header = BLOCK_HEADER_SIZE;

    if (heap_start == NULL) return;

    uint8_t *ptr = heap_start;
    while ((size_t)(ptr - heap_start) < heap_used) {
        kmalloc_block_t *block = (kmalloc_block_t *)ptr;
        if (block->magic != KMALLOC_MAGIC && block->magic != KMALLOC_FREED_MAGIC) {
            break; /* corruption guard */
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
}
