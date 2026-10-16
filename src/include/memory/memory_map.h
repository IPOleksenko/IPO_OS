#ifndef MEMORY_MAP_H
#define MEMORY_MAP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define E820_MAGIC_ADDR    0x8FE0
#define E820_COUNT_ADDR    0x8FE4
#define E820_PTR_ADDR      0x8FE8
#define E820_RAM_LOW_ADDR  0x8FF0
#define E820_RAM_HIGH_ADDR 0x8FF4

#define E820_MAGIC_VAL     0x45383230  /* 'E820' */

#define E820_TYPE_USABLE        1
#define E820_TYPE_RESERVED      2
#define E820_TYPE_ACPI_RECLAIM  3
#define E820_TYPE_ACPI_NVS      4
#define E820_TYPE_BAD           5

typedef struct {
    uint64_t base;
    uint64_t length;
    uint32_t type;
} __attribute__((packed)) e820_entry_t;

typedef struct {
    uint32_t base;
    uint32_t size;
    uint32_t used;
    bool     is_usable;
} memory_region_t;

void memory_map_init(void);
uint32_t memory_map_get_region_count(void);
const memory_region_t *memory_map_get_region(uint32_t index);
uint64_t memory_map_get_total_ram(void);
void *memory_map_alloc(size_t size, size_t alignment);
bool memory_map_is_valid_ram(uint32_t addr, uint32_t len);

/* Raw BIOS E820 map entry access */
uint32_t memory_map_get_raw_e820_count(void);
const e820_entry_t *memory_map_get_raw_e820_entry(uint32_t index);
const char *memory_map_e820_type_name(uint32_t type);

#endif /* MEMORY_MAP_H */

