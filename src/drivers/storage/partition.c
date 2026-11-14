#include <driver/storage/partition.h>
#include <memory/kmalloc.h>
#include <string.h>
#include <stdio.h>

/* IEEE 802.3 CRC32 Implementation */
uint32_t partition_crc32(const void *data, size_t length) {
    static uint32_t *table = NULL;

    if (!table) {
        uint32_t *allocated = (uint32_t *)kmalloc(256u * sizeof(uint32_t));
        if (allocated) {
            for (uint32_t i = 0; i < 256u; i++) {
                uint32_t rem = i;
                for (int j = 0; j < 8; j++) {
                    if (rem & 1u)
                        rem = (rem >> 1) ^ 0xEDB88320u;
                    else
                        rem >>= 1;
                }
                allocated[i] = rem;
            }
            table = allocated;
        }
    }

    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    if (table) {
        for (size_t i = 0; i < length; i++) {
            uint8_t octet = p[i];
            crc = (crc >> 8) ^ table[(crc & 0xFFu) ^ octet];
        }
    } else {
        for (size_t i = 0; i < length; i++) {
            crc ^= p[i];
            for (int j = 0; j < 8; j++) {
                if (crc & 1u)
                    crc = (crc >> 1) ^ 0xEDB88320u;
                else
                    crc >>= 1;
            }
        }
    }
    return ~crc;
}

typedef struct {
    uint8_t  status;
    uint8_t  type;
    uint32_t lba_start;
    uint32_t sector_count;
} mbr_entry_raw_t;

typedef struct {
    uint32_t header_size;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint64_t partition_entries_lba;
    uint32_t num_partition_entries;
    uint32_t sizeof_partition_entry;
} gpt_header_raw_t;

static int part_buf_assign(void **dst, size_t *dst_len, size_t *dst_cap,
                           const void *src, size_t elem_size, size_t count, bool nul_term) {
    if (!dst || !dst_len || !dst_cap) return -1;
    if (*dst) {
        kfree(*dst);
        *dst = NULL;
    }
    *dst_len = 0;
    *dst_cap = 0;
    if (!src || (count == 0 && !nul_term)) return 0;

    size_t alloc_bytes = (count + (nul_term ? 1u : 0u)) * elem_size;
    if (alloc_bytes == 0) return 0;

    void *buf = kmalloc(alloc_bytes);
    if (!buf) return -1;
    if (count > 0) {
        memcpy(buf, src, count * elem_size);
    }
    if (nul_term) {
        ((char *)buf)[count] = '\0';
    }
    *dst = buf;
    *dst_len = count;
    *dst_cap = alloc_bytes;
    return 0;
}

static bool check_overlap(uint64_t s1, uint64_t c1, uint64_t s2, uint64_t c2) {
    if (c1 == 0 || c2 == 0) return false;
    uint64_t e1 = ((c1 - 1u) <= UINT64_MAX - s1) ? (s1 + c1 - 1u) : UINT64_MAX;
    uint64_t e2 = ((c2 - 1u) <= UINT64_MAX - s2) ? (s2 + c2 - 1u) : UINT64_MAX;
    return !(e1 < s2 || e2 < s1);
}

static int parse_gpt(block_device_t *bdev, partition_table_t *table) {
    uint8_t *sector_buf = (uint8_t *)kmalloc(512u);
    if (!sector_buf) return -1;

    /* Read LBA 1 (GPT Header) */
    if (!block_dev_read(bdev, 1, 1, sector_buf)) {
        kfree(sector_buf);
        return -1;
    }

    if (memcmp(sector_buf, "EFI PART", 8) != 0) {
        kfree(sector_buf);
        return -2;
    }

    /* Verify Header CRC32 */
    uint32_t orig_crc = *(uint32_t *)(sector_buf + 16);
    uint32_t hdr_size = *(uint32_t *)(sector_buf + 12);
    if (hdr_size < 92 || hdr_size > 512) {
        kfree(sector_buf);
        return -3;
    }
    *(uint32_t *)(sector_buf + 16) = 0;
    uint32_t calc_crc = partition_crc32(sector_buf, hdr_size);
    *(uint32_t *)(sector_buf + 16) = orig_crc;

    if (calc_crc != orig_crc) {
        serial_printf("[partition] GPT header CRC mismatch: calc 0x%08x vs hdr 0x%08x\n",
                      calc_crc, orig_crc);
        kfree(sector_buf);
        return -3;
    }

    gpt_header_raw_t hdr;
    hdr.header_size = hdr_size;
    hdr.first_usable_lba = *(uint64_t *)(sector_buf + 40);
    hdr.last_usable_lba = *(uint64_t *)(sector_buf + 48);
    hdr.partition_entries_lba = *(uint64_t *)(sector_buf + 72);
    hdr.num_partition_entries = *(uint32_t *)(sector_buf + 80);
    hdr.sizeof_partition_entry = *(uint32_t *)(sector_buf + 84);

    /* Bounds check partition entry parameters (min 128 bytes per UEFI spec) */
    if (hdr.num_partition_entries == 0 || hdr.sizeof_partition_entry < 128 ||
        hdr.sizeof_partition_entry > 512 || hdr.first_usable_lba > hdr.last_usable_lba) {
        kfree(sector_buf);
        return -4;
    }

    table->type = PARTITION_TABLE_GPT;
    table->count = 0;

    uint32_t entries_per_sector = 512u / hdr.sizeof_partition_entry;
    uint32_t total_sectors = (hdr.num_partition_entries - 1u) / entries_per_sector + 1u;
    uint32_t scanned = 0;

    for (uint32_t s = 0; s < total_sectors && scanned < hdr.num_partition_entries; s++) {
        if (!block_dev_read(bdev, hdr.partition_entries_lba + s, 1, sector_buf)) {
            break;
        }

        for (uint32_t e = 0; e < entries_per_sector && scanned < hdr.num_partition_entries; e++) {
            const uint8_t *raw_ent = sector_buf + (e * hdr.sizeof_partition_entry);
            scanned++;

            const uint8_t *type_guid = raw_ent;
            const uint8_t *unique_guid = raw_ent + 16;
            uint64_t start_lba = *(const uint64_t *)(raw_ent + 32);
            uint64_t end_lba = *(const uint64_t *)(raw_ent + 40);
            uint64_t attributes = *(const uint64_t *)(raw_ent + 48);

            bool is_empty = true;
            for (size_t b = 0; b < 16; b++) {
                if (type_guid[b] != 0) {
                    is_empty = false;
                    break;
                }
            }
            if (is_empty) {
                continue;
            }

            if (start_lba > end_lba ||
                start_lba < hdr.first_usable_lba ||
                end_lba > hdr.last_usable_lba) {
                serial_printf("[partition] GPT entry %u invalid bounds: [%llu..%llu]\n",
                              scanned, start_lba, end_lba);
                continue;
            }

            uint64_t count = end_lba - start_lba + 1u;

            bool overlap = false;
            for (uint32_t p = 0; p < table->count; p++) {
                if (check_overlap(start_lba, count, table->partitions[p].start_lba, table->partitions[p].sector_count)) {
                    serial_printf("[partition] GPT partition %u overlaps with partition %u\n", scanned, p + 1);
                    overlap = true;
                    break;
                }
            }
            if (overlap) {
                continue;
            }

            partition_info_t p;
            partition_info_init(&p);
            p.part_number = table->count + 1;
            p.start_lba = start_lba;
            p.sector_count = count;
            partition_info_set_gpt_type_guid(&p, type_guid, 16);
            partition_info_set_unique_guid(&p, unique_guid, 16);
            p.flags = (uint32_t)(attributes & 0xFFFFFFFFu);
            p.valid = true;

            size_t name_units = (hdr.sizeof_partition_entry > 56) ? (hdr.sizeof_partition_entry - 56) / 2u : 0;
            if (name_units > 36) name_units = 36;
            if (name_units > 0) {
                char tmp_name[37];
                const uint16_t *wname = (const uint16_t *)(raw_ent + 56);
                size_t c;
                for (c = 0; c < name_units; c++) {
                    uint16_t ch = wname[c];
                    if (ch == 0) break;
                    tmp_name[c] = (ch < 128) ? (char)ch : '?';
                }
                tmp_name[c] = '\0';
                if (tmp_name[0] != '\0') {
                    partition_info_set_name(&p, tmp_name);
                }
            }

            partition_table_add(table, &p);
            partition_info_free(&p);
        }
    }

    kfree(sector_buf);
    return (table->count > 0) ? 0 : -5;
}

int partition_scan(block_device_t *bdev, partition_table_t *out_table) {
    if (!bdev || !out_table) return -1;

    memset(out_table, 0, sizeof(*out_table));
    if (bdev->name) {
        size_t blen = strlen(bdev->name);
        out_table->bdev_name = (char *)kmalloc(blen + 1u);
        if (out_table->bdev_name) {
            memcpy(out_table->bdev_name, bdev->name, blen + 1u);
            out_table->bdev_name_len = blen;
            out_table->bdev_name_cap = blen + 1u;
        }
    }
    out_table->type = PARTITION_TABLE_NONE;

    uint8_t *mbr_buf = (uint8_t *)kmalloc(512u);
    if (!mbr_buf) return -2;
    if (!block_dev_read(bdev, 0, 1, mbr_buf)) {
        kfree(mbr_buf);
        serial_printf("[partition] Failed to read LBA 0 from '%s'\n", bdev->name ? bdev->name : "");
        return -2;
    }

    if (mbr_buf[510] != 0x55 || mbr_buf[511] != 0xAA) {
        kfree(mbr_buf);
        serial_printf("[partition] Device '%s': No MBR signature 0xAA55\n", bdev->name ? bdev->name : "");
        return 0;
    }

    mbr_entry_raw_t mbr_entries[4];
    for (int i = 0; i < 4; i++) {
        const uint8_t *raw = mbr_buf + 446 + (i * 16);
        mbr_entries[i].status = raw[0];
        mbr_entries[i].type = raw[4];
        mbr_entries[i].lba_start = (uint32_t)raw[8] | ((uint32_t)raw[9] << 8) |
                                   ((uint32_t)raw[10] << 16) | ((uint32_t)raw[11] << 24);
        mbr_entries[i].sector_count = (uint32_t)raw[12] | ((uint32_t)raw[13] << 8) |
                                      ((uint32_t)raw[14] << 16) | ((uint32_t)raw[15] << 24);
    }
    kfree(mbr_buf);

    if (mbr_entries[0].type == 0xEE) {
        if (parse_gpt(bdev, out_table) == 0 && out_table->count > 0) {
            return (int)out_table->count;
        }
    }

    out_table->type = PARTITION_TABLE_MBR;
    out_table->count = 0;

    for (uint32_t i = 0; i < 4; i++) {
        mbr_entry_raw_t *e = &mbr_entries[i];
        if (e->type == 0 || e->sector_count == 0) continue;

        if (bdev->total_sectors > 0 &&
            ((uint64_t)e->lba_start + (uint64_t)e->sector_count > bdev->total_sectors)) {
            serial_printf("[partition] MBR entry %u out of bounds: lba=%u count=%u total=%llu\n",
                          i + 1, e->lba_start, e->sector_count, bdev->total_sectors);
            continue;
        }

        bool overlap = false;
        for (uint32_t p = 0; p < out_table->count; p++) {
            if (check_overlap(e->lba_start, e->sector_count,
                              out_table->partitions[p].start_lba,
                              out_table->partitions[p].sector_count)) {
                serial_printf("[partition] MBR entry %u overlaps with partition %u\n", i + 1, p + 1);
                overlap = true;
                break;
            }
        }
        if (overlap) continue;

        partition_info_t p;
        partition_info_init(&p);
        p.part_number = out_table->count + 1;
        p.start_lba = e->lba_start;
        p.sector_count = e->sector_count;
        p.mbr_type = e->type;
        p.flags = (e->status & 0x80) ? 1 : 0;
        p.valid = true;

        /* Probe partition first sector for an ASCII filesystem signature */
        uint8_t *pbuf = (uint8_t *)kmalloc(512u);
        if (pbuf && block_dev_read(bdev, e->lba_start, 1, pbuf)) {
            size_t sig_len = 0;
            while (sig_len < 16 && pbuf[sig_len] >= 0x21 && pbuf[sig_len] <= 0x7E) {
                sig_len++;
            }
            if (sig_len >= 3) {
                char sig[17];
                memcpy(sig, pbuf, sig_len);
                sig[sig_len] = '\0';
                partition_info_set_name(&p, sig);
            }
        }
        if (pbuf) kfree(pbuf);

        partition_table_add(out_table, &p);
        partition_info_free(&p);
    }

    return (int)out_table->count;
}

void partition_info_init(partition_info_t *p) {
    if (!p) return;
    memset(p, 0, sizeof(*p));
}

void partition_info_free(partition_info_t *p) {
    if (!p) return;
    if (p->gpt_type_guid) {
        kfree(p->gpt_type_guid);
        p->gpt_type_guid = NULL;
    }
    p->gpt_type_guid_len = 0;
    p->gpt_type_guid_cap = 0;

    if (p->unique_guid) {
        kfree(p->unique_guid);
        p->unique_guid = NULL;
    }
    p->unique_guid_len = 0;
    p->unique_guid_cap = 0;

    if (p->name) {
        kfree(p->name);
        p->name = NULL;
    }
    p->name_len = 0;
    p->name_cap = 0;
    p->valid = false;
}

int partition_info_set_name(partition_info_t *p, const char *name) {
    if (!p) return -1;
    void *ptr = p->name;
    size_t len = p->name_len;
    size_t cap = p->name_cap;
    int rc = part_buf_assign(&ptr, &len, &cap, name, 1u, name ? strlen(name) : 0u, name != NULL);
    p->name = (char *)ptr;
    p->name_len = len;
    p->name_cap = cap;
    return rc;
}

int partition_info_set_gpt_type_guid(partition_info_t *p, const uint8_t *guid, size_t len) {
    if (!p) return -1;
    void *ptr = p->gpt_type_guid;
    size_t g_len = p->gpt_type_guid_len;
    size_t g_cap = p->gpt_type_guid_cap;
    int rc = part_buf_assign(&ptr, &g_len, &g_cap, guid, 1u, (guid && len > 0) ? len : 0u, false);
    p->gpt_type_guid = (uint8_t *)ptr;
    p->gpt_type_guid_len = g_len;
    p->gpt_type_guid_cap = g_cap;
    return rc;
}

int partition_info_set_unique_guid(partition_info_t *p, const uint8_t *guid, size_t len) {
    if (!p) return -1;
    void *ptr = p->unique_guid;
    size_t g_len = p->unique_guid_len;
    size_t g_cap = p->unique_guid_cap;
    int rc = part_buf_assign(&ptr, &g_len, &g_cap, guid, 1u, (guid && len > 0) ? len : 0u, false);
    p->unique_guid = (uint8_t *)ptr;
    p->unique_guid_len = g_len;
    p->unique_guid_cap = g_cap;
    return rc;
}

int partition_info_copy(partition_info_t *dst, const partition_info_t *src) {
    if (!dst) return -1;
    partition_info_free(dst);
    if (!src) return 0;

    dst->part_number = src->part_number;
    dst->start_lba = src->start_lba;
    dst->sector_count = src->sector_count;
    dst->mbr_type = src->mbr_type;
    dst->flags = src->flags;
    dst->valid = src->valid;

    if (src->gpt_type_guid && src->gpt_type_guid_len > 0) {
        if (partition_info_set_gpt_type_guid(dst, src->gpt_type_guid, src->gpt_type_guid_len) != 0) {
            partition_info_free(dst);
            return -1;
        }
    }
    if (src->unique_guid && src->unique_guid_len > 0) {
        if (partition_info_set_unique_guid(dst, src->unique_guid, src->unique_guid_len) != 0) {
            partition_info_free(dst);
            return -1;
        }
    }
    if (src->name) {
        if (partition_info_set_name(dst, src->name) != 0) {
            partition_info_free(dst);
            return -1;
        }
    }
    return 0;
}

void partition_print_table(const partition_table_t *table) {
    if (!table) return;

    const char *type_name = "None";
    if (table->type == PARTITION_TABLE_MBR) type_name = "MBR (Master Boot Record)";
    else if (table->type == PARTITION_TABLE_GPT) type_name = "GPT (GUID Partition Table)";

    printf("  Device: %s  Partition Table: %s  Partitions: %u\n",
           table->bdev_name ? table->bdev_name : "(unknown)", type_name, table->count);

    if (table->count == 0) {
        printf("    No valid partitions detected.\n");
        return;
    }

    printf("    #  Boot  Start LBA       End LBA         Sectors     Size (MiB) Type/Label\n");
    printf("    --------------------------------------------------------------------------\n");

    for (uint32_t i = 0; i < table->count; i++) {
        const partition_info_t *p = &table->partitions[i];
        if (!p->valid || p->sector_count == 0) continue;

        uint64_t end_lba = p->start_lba + p->sector_count - 1;
        uint32_t size_mib = (uint32_t)((p->sector_count * 512ull) / (1024ull * 1024ull));

        printf("    %u   %s   %-15llu %-15llu %-11llu %-10u ",
               p->part_number,
               (p->flags & 1) ? "*" : " ",
               p->start_lba,
               end_lba,
               p->sector_count,
               size_mib);
        if (table->type == PARTITION_TABLE_MBR) {
            if (p->name && p->name[0]) {
                printf("0x%02X (%s)\n", p->mbr_type, p->name);
            } else {
                printf("0x%02X (Unknown)\n", p->mbr_type);
            }
        } else {
            printf("%s\n", (p->name && p->name[0]) ? p->name : "Unknown");
        }
    }
}

void partition_free_table(partition_table_t *table) {
    if (!table) return;
    if (table->bdev_name) {
        kfree(table->bdev_name);
        table->bdev_name = NULL;
    }
    table->bdev_name_len = 0;
    table->bdev_name_cap = 0;
    if (table->partitions) {
        for (uint32_t i = 0; i < table->count; i++) {
            partition_info_free(&table->partitions[i]);
        }
        kfree(table->partitions);
        table->partitions = NULL;
        table->count = 0;
        table->capacity = 0;
    }
}

int partition_table_add(partition_table_t *table, const partition_info_t *part) {
    if (!table || !part) return -1;
    if (table->count >= table->capacity) {
        uint32_t new_cap = (table->capacity == 0) ? 4 : (table->capacity * 2);
        partition_info_t *new_buf = (partition_info_t *)krealloc(table->partitions, new_cap * sizeof(partition_info_t));
        if (!new_buf) return -1;
        memset(new_buf + table->count, 0, (new_cap - table->count) * sizeof(partition_info_t));
        table->partitions = new_buf;
        table->capacity = new_cap;
    }
    partition_info_init(&table->partitions[table->count]);
    if (partition_info_copy(&table->partitions[table->count], part) != 0) {
        return -1;
    }
    table->partitions[table->count].valid = true;
    table->count++;
    return 0;
}

