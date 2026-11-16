#ifndef MEMORY_MAP_H
#define MEMORY_MAP_H

#include <types.h>
#include <arch/exec_mode.h>

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

/* 20-byte BIOS INT 15h E820 entry */
typedef struct {
    uint64_t base;
    uint64_t length;
    uint32_t type;
} __attribute__((packed)) e820_entry_t;

IPO_STATIC_ASSERT(sizeof(e820_entry_t) == 20u, "E820 hardware entry must stay 20 bytes");

typedef struct {
    ipo_paddr_t base;
    ipo_uoff_t  size;
    ipo_uoff_t  used;
    bool        is_usable;
} memory_region_t;

void memory_map_init(void);
void memory_map_init_handover(const ipo_boot_handover_t *handover);
uint32_t memory_map_get_region_count(void);
const memory_region_t *memory_map_get_region(uint32_t index);
uint64_t memory_map_get_total_ram(void);
void *memory_map_alloc(ipo_size_t size, ipo_size_t alignment);
bool memory_map_is_valid_ram(ipo_paddr_t addr, ipo_size_t len);
void *memory_page_alloc(void);
bool memory_page_free(void *page);

uint32_t memory_map_get_raw_e820_count(void);
const e820_entry_t *memory_map_get_raw_e820_entry(uint32_t index);
const char *memory_map_e820_type_name(uint32_t type);

#endif /* MEMORY_MAP_H */

