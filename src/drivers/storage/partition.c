#include <driver/storage/partition.h>
#include <memory/kmalloc.h>
#include <string.h>
#include <stdio.h>

/* IEEE 802.3 CRC32 Implementation */
uint32_t partition_crc32(const void *data, size_t length) {
    static uint32_t table[256];
    static bool have_table = false;

    if (!have_table) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t rem = i;
            for (int j = 0; j < 8; j++) {
                if (rem & 1)
                    rem = (rem >> 1) ^ 0xEDB88320;
                else
                    rem >>= 1;
            }
            table[i] = rem;
        }
        have_table = true;
    }

    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; i++) {
        uint8_t octet = p[i];
        crc = (crc >> 8) ^ table[(crc & 0xFF) ^ octet];
    }
    return ~crc;
}

/* Self-expanding MBR raw structures */
typedef struct {
    uint8_t  status;          /* 0x80 = active/bootable */
    uint8_t  type;            /* Partition type */
    uint32_t lba_start;       /* LBA start */
    uint32_t sector_count;    /* Number of sectors */

    /* Self-expanding CHS addressing buffers */
    uint8_t  *chs_first;
    size_t   chs_first_len;
    size_t   chs_first_cap;

    uint8_t  *chs_last;
    size_t   chs_last_len;
    size_t   chs_last_cap;
} mbr_entry_raw_t;

static void mbr_entry_init(mbr_entry_raw_t *e) {
    if (e) memset(e, 0, sizeof(*e));
}

static bool mbr_entry_set_chs_first(mbr_entry_raw_t *e, const uint8_t *data, size_t len) {
    if (!e) return false;
    if (len > e->chs_first_cap || !e->chs_first) {
        size_t new_cap = (e->chs_first_cap == 0) ? 4 : e->chs_first_cap;
        while (new_cap < len) new_cap *= 2;
        uint8_t *new_buf = (uint8_t *)krealloc(e->chs_first, new_cap);
        if (!new_buf) return false;
        e->chs_first = new_buf;
        e->chs_first_cap = new_cap;
    }
    if (data && len > 0) memcpy(e->chs_first, data, len);
    e->chs_first_len = len;
    return true;
}

static bool mbr_entry_set_chs_last(mbr_entry_raw_t *e, const uint8_t *data, size_t len) {
    if (!e) return false;
    if (len > e->chs_last_cap || !e->chs_last) {
        size_t new_cap = (e->chs_last_cap == 0) ? 4 : e->chs_last_cap;
        while (new_cap < len) new_cap *= 2;
        uint8_t *new_buf = (uint8_t *)krealloc(e->chs_last, new_cap);
        if (!new_buf) return false;
        e->chs_last = new_buf;
        e->chs_last_cap = new_cap;
    }
    if (data && len > 0) memcpy(e->chs_last, data, len);
    e->chs_last_len = len;
    return true;
}

static void mbr_entry_free(mbr_entry_raw_t *e) {
    if (e) {
        if (e->chs_first) { kfree(e->chs_first); e->chs_first = NULL; }
        if (e->chs_last) { kfree(e->chs_last); e->chs_last = NULL; }
        e->chs_first_len = e->chs_first_cap = 0;
        e->chs_last_len = e->chs_last_cap = 0;
    }
}

/* Self-expanding table of MBR entries */
typedef struct {
    mbr_entry_raw_t *entries;
    uint32_t        count;
    uint32_t        capacity;
} mbr_table_t;

static void mbr_table_init(mbr_table_t *tbl) {
    if (tbl) {
        tbl->entries = NULL;
        tbl->count = tbl->capacity = 0;
    }
}

static bool mbr_table_add(mbr_table_t *tbl, const mbr_entry_raw_t *e) {
    if (!tbl || !e) return false;
    if (tbl->count >= tbl->capacity) {
        uint32_t new_cap = (tbl->capacity == 0) ? 4 : (tbl->capacity * 2);
        mbr_entry_raw_t *new_entries = (mbr_entry_raw_t *)krealloc(tbl->entries, new_cap * sizeof(mbr_entry_raw_t));
        if (!new_entries) return false;
        tbl->entries = new_entries;
        tbl->capacity = new_cap;
    }
    mbr_entry_raw_t *dst = &tbl->entries[tbl->count++];
    mbr_entry_init(dst);
    dst->status = e->status;
    dst->type = e->type;
    dst->lba_start = e->lba_start;
    dst->sector_count = e->sector_count;
    mbr_entry_set_chs_first(dst, e->chs_first, e->chs_first_len);
    mbr_entry_set_chs_last(dst, e->chs_last, e->chs_last_len);
    return true;
}

static void mbr_table_free(mbr_table_t *tbl) {
    if (tbl) {
        for (uint32_t i = 0; i < tbl->count; i++) {
            mbr_entry_free(&tbl->entries[i]);
        }
        if (tbl->entries) {
            kfree(tbl->entries);
            tbl->entries = NULL;
        }
        tbl->count = tbl->capacity = 0;
    }
}

/* Self-expanding GPT header raw structure */
typedef struct {
    char     *signature;          /* Self-expanding signature string ("EFI PART") */
    size_t   signature_len;
    size_t   signature_cap;

    uint32_t revision;            /* 0x00010000 */
    uint32_t header_size;         /* Typically 92 */
    uint32_t header_crc32;
    uint32_t reserved;
    uint64_t current_lba;
    uint64_t backup_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;

    uint8_t  *disk_guid;          /* Self-expanding disk GUID buffer */
    size_t   disk_guid_len;
    size_t   disk_guid_cap;

    uint64_t partition_entries_lba;
    uint32_t num_partition_entries;
    uint32_t sizeof_partition_entry;
    uint32_t partition_array_crc32;
} gpt_header_raw_t;

static void gpt_header_init(gpt_header_raw_t *hdr) {
    if (hdr) memset(hdr, 0, sizeof(*hdr));
}

static bool gpt_header_set_signature(gpt_header_raw_t *hdr, const char *sig, size_t len) {
    if (!hdr) return false;
    if (len + 1 > hdr->signature_cap || !hdr->signature) {
        size_t new_cap = (hdr->signature_cap == 0) ? 16 : hdr->signature_cap;
        while (new_cap < len + 1) new_cap *= 2;
        char *new_buf = (char *)krealloc(hdr->signature, new_cap);
        if (!new_buf) return false;
        hdr->signature = new_buf;
        hdr->signature_cap = new_cap;
    }
    if (sig && len > 0) memcpy(hdr->signature, sig, len);
    hdr->signature[len] = '\0';
    hdr->signature_len = len;
    return true;
}

static bool gpt_header_set_disk_guid(gpt_header_raw_t *hdr, const uint8_t *guid, size_t len) {
    if (!hdr) return false;
    if (len > hdr->disk_guid_cap || !hdr->disk_guid) {
        size_t new_cap = (hdr->disk_guid_cap == 0) ? 16 : hdr->disk_guid_cap;
        while (new_cap < len) new_cap *= 2;
        uint8_t *new_buf = (uint8_t *)krealloc(hdr->disk_guid, new_cap);
        if (!new_buf) return false;
        hdr->disk_guid = new_buf;
        hdr->disk_guid_cap = new_cap;
    }
    if (guid && len > 0) memcpy(hdr->disk_guid, guid, len);
    hdr->disk_guid_len = len;
    return true;
}

static void gpt_header_free(gpt_header_raw_t *hdr) {
    if (hdr) {
        if (hdr->signature) { kfree(hdr->signature); hdr->signature = NULL; }
        if (hdr->disk_guid) { kfree(hdr->disk_guid); hdr->disk_guid = NULL; }
        hdr->signature_len = hdr->signature_cap = 0;
        hdr->disk_guid_len = hdr->disk_guid_cap = 0;
    }
}

/* Self-expanding GPT entry raw structure */
typedef struct {
    uint8_t  *type_guid;          /* Self-expanding partition type GUID buffer */
    size_t   type_guid_len;
    size_t   type_guid_cap;

    uint8_t  *unique_guid;        /* Self-expanding unique partition GUID buffer */
    size_t   unique_guid_len;
    size_t   unique_guid_cap;

    uint64_t start_lba;
    uint64_t end_lba;
    uint64_t attributes;

    uint16_t *name;               /* Self-expanding UTF-16LE label buffer */
    size_t   name_len;
    size_t   name_cap;
} gpt_entry_raw_t;

static void gpt_entry_init(gpt_entry_raw_t *entry) {
    if (entry) memset(entry, 0, sizeof(*entry));
}

static bool gpt_entry_set_type_guid(gpt_entry_raw_t *entry, const uint8_t *guid, size_t len) {
    if (!entry) return false;
    if (len > entry->type_guid_cap || !entry->type_guid) {
        size_t new_cap = (entry->type_guid_cap == 0) ? 16 : entry->type_guid_cap;
        while (new_cap < len) new_cap *= 2;
        uint8_t *new_buf = (uint8_t *)krealloc(entry->type_guid, new_cap);
        if (!new_buf) return false;
        entry->type_guid = new_buf;
        entry->type_guid_cap = new_cap;
    }
    if (guid && len > 0) memcpy(entry->type_guid, guid, len);
    entry->type_guid_len = len;
    return true;
}

static bool gpt_entry_set_unique_guid(gpt_entry_raw_t *entry, const uint8_t *guid, size_t len) {
    if (!entry) return false;
    if (len > entry->unique_guid_cap || !entry->unique_guid) {
        size_t new_cap = (entry->unique_guid_cap == 0) ? 16 : entry->unique_guid_cap;
        while (new_cap < len) new_cap *= 2;
        uint8_t *new_buf = (uint8_t *)krealloc(entry->unique_guid, new_cap);
        if (!new_buf) return false;
        entry->unique_guid = new_buf;
        entry->unique_guid_cap = new_cap;
    }
    if (guid && len > 0) memcpy(entry->unique_guid, guid, len);
    entry->unique_guid_len = len;
    return true;
}

static bool gpt_entry_set_name(gpt_entry_raw_t *entry, const uint16_t *name, size_t len) {
    if (!entry) return false;
    if (len + 1 > entry->name_cap || !entry->name) {
        size_t new_cap = (entry->name_cap == 0) ? 36 : entry->name_cap;
        while (new_cap < len + 1) new_cap *= 2;
        uint16_t *new_buf = (uint16_t *)krealloc(entry->name, new_cap * sizeof(uint16_t));
        if (!new_buf) return false;
        entry->name = new_buf;
        entry->name_cap = new_cap;
    }
    if (name && len > 0) memcpy(entry->name, name, len * sizeof(uint16_t));
    entry->name[len] = 0;
    entry->name_len = len;
    return true;
}

static void gpt_entry_free(gpt_entry_raw_t *entry) {
    if (entry) {
        if (entry->type_guid) { kfree(entry->type_guid); entry->type_guid = NULL; }
        if (entry->unique_guid) { kfree(entry->unique_guid); entry->unique_guid = NULL; }
        if (entry->name) { kfree(entry->name); entry->name = NULL; }
        entry->type_guid_len = entry->type_guid_cap = 0;
        entry->unique_guid_len = entry->unique_guid_cap = 0;
        entry->name_len = entry->name_cap = 0;
    }
}

/* Self-expanding table of GPT entries */
typedef struct {
    gpt_entry_raw_t *entries;
    uint32_t        count;
    uint32_t        capacity;
} gpt_table_t;

static void gpt_table_init(gpt_table_t *tbl) {
    if (tbl) {
        tbl->entries = NULL;
        tbl->count = tbl->capacity = 0;
    }
}

static bool gpt_table_add(gpt_table_t *tbl, const gpt_entry_raw_t *entry) {
    if (!tbl || !entry) return false;
    if (tbl->count >= tbl->capacity) {
        uint32_t new_cap = (tbl->capacity == 0) ? 8 : (tbl->capacity * 2);
        gpt_entry_raw_t *new_entries = (gpt_entry_raw_t *)krealloc(tbl->entries, new_cap * sizeof(gpt_entry_raw_t));
        if (!new_entries) return false;
        tbl->entries = new_entries;
        tbl->capacity = new_cap;
    }
    gpt_entry_raw_t *dst = &tbl->entries[tbl->count++];
    gpt_entry_init(dst);
    dst->start_lba = entry->start_lba;
    dst->end_lba = entry->end_lba;
    dst->attributes = entry->attributes;
    gpt_entry_set_type_guid(dst, entry->type_guid, entry->type_guid_len);
    gpt_entry_set_unique_guid(dst, entry->unique_guid, entry->unique_guid_len);
    gpt_entry_set_name(dst, entry->name, entry->name_len);
    return true;
}

static void gpt_table_free(gpt_table_t *tbl) {
    if (tbl) {
        for (uint32_t i = 0; i < tbl->count; i++) {
            gpt_entry_free(&tbl->entries[i]);
        }
        if (tbl->entries) {
            kfree(tbl->entries);
            tbl->entries = NULL;
        }
        tbl->count = tbl->capacity = 0;
    }
}

/* Helper to verify non-overlapping ranges */
static bool check_overlap(uint64_t s1, uint64_t c1, uint64_t s2, uint64_t c2) {
    if (c1 == 0 || c2 == 0) return false;
    uint64_t e1 = s1 + c1 - 1;
    uint64_t e2 = s2 + c2 - 1;
    return !(e1 < s2 || e2 < s1);
}

static int parse_gpt(block_device_t *bdev, partition_table_t *table) {
    uint8_t sector_buf[512];

    /* Read LBA 1 (GPT Header) */
    if (!block_dev_read(bdev, 1, 1, sector_buf)) {
        return -1;
    }

    if (memcmp(sector_buf, "EFI PART", 8) != 0) {
        return -2;
    }

    /* Verify Header CRC32 */
    uint32_t orig_crc = *(uint32_t *)(sector_buf + 16);
    uint32_t hdr_size = *(uint32_t *)(sector_buf + 12);
    if (hdr_size < 92 || hdr_size > 512) {
        return -3;
    }
    *(uint32_t *)(sector_buf + 16) = 0;
    uint32_t calc_crc = partition_crc32(sector_buf, hdr_size);
    *(uint32_t *)(sector_buf + 16) = orig_crc;

    if (calc_crc != orig_crc) {
        serial_printf("[partition] GPT header CRC mismatch: calc 0x%08x vs hdr 0x%08x\n",
                      calc_crc, orig_crc);
        return -3;
    }

    /* Initialize self-expanding GPT header */
    gpt_header_raw_t hdr;
    gpt_header_init(&hdr);
    gpt_header_set_signature(&hdr, (const char *)sector_buf, 8);
    hdr.revision = *(uint32_t *)(sector_buf + 8);
    hdr.header_size = hdr_size;
    hdr.header_crc32 = orig_crc;
    hdr.reserved = *(uint32_t *)(sector_buf + 20);
    hdr.current_lba = *(uint64_t *)(sector_buf + 24);
    hdr.backup_lba = *(uint64_t *)(sector_buf + 32);
    hdr.first_usable_lba = *(uint64_t *)(sector_buf + 40);
    hdr.last_usable_lba = *(uint64_t *)(sector_buf + 48);
    gpt_header_set_disk_guid(&hdr, sector_buf + 56, 16);
    hdr.partition_entries_lba = *(uint64_t *)(sector_buf + 72);
    hdr.num_partition_entries = *(uint32_t *)(sector_buf + 80);
    hdr.sizeof_partition_entry = *(uint32_t *)(sector_buf + 84);
    hdr.partition_array_crc32 = *(uint32_t *)(sector_buf + 88);

    /* Bounds check partition entry parameters (min 128 bytes per UEFI spec) */
    if (hdr.num_partition_entries == 0 || hdr.sizeof_partition_entry < 128 ||
        hdr.sizeof_partition_entry > 512) {
        gpt_header_free(&hdr);
        return -4;
    }

    table->type = PARTITION_TABLE_GPT;
    table->count = 0;

    uint32_t entries_per_sector = 512 / hdr.sizeof_partition_entry;
    uint32_t total_sectors = (hdr.num_partition_entries + entries_per_sector - 1) / entries_per_sector;
    uint32_t scanned = 0;

    gpt_table_t gpt_tbl;
    gpt_table_init(&gpt_tbl);

    for (uint32_t s = 0; s < total_sectors && scanned < hdr.num_partition_entries; s++) {
        if (!block_dev_read(bdev, hdr.partition_entries_lba + s, 1, sector_buf)) {
            break;
        }

        for (uint32_t e = 0; e < entries_per_sector && scanned < hdr.num_partition_entries; e++) {
            const uint8_t *raw_ent = sector_buf + (e * hdr.sizeof_partition_entry);
            scanned++;

            /* Unpack into self-expanding GPT entry */
            gpt_entry_raw_t entry;
            gpt_entry_init(&entry);
            gpt_entry_set_type_guid(&entry, raw_ent, 16);
            gpt_entry_set_unique_guid(&entry, raw_ent + 16, 16);
            entry.start_lba = *(uint64_t *)(raw_ent + 32);
            entry.end_lba = *(uint64_t *)(raw_ent + 40);
            entry.attributes = *(uint64_t *)(raw_ent + 48);

            size_t name_units = (hdr.sizeof_partition_entry > 56) ? (hdr.sizeof_partition_entry - 56) / 2 : 0;
            if (name_units > 36) name_units = 36;
            gpt_entry_set_name(&entry, (const uint16_t *)(raw_ent + 56), name_units);

            /* Check if entry is unused (all zero type GUID) */
            bool is_empty = true;
            for (size_t b = 0; b < entry.type_guid_len; b++) {
                if (entry.type_guid[b] != 0) {
                    is_empty = false;
                    break;
                }
            }
            if (is_empty) {
                gpt_entry_free(&entry);
                continue;
            }

            /* Bounds check within disk */
            if (entry.start_lba > entry.end_lba ||
                entry.start_lba < hdr.first_usable_lba ||
                entry.end_lba > hdr.last_usable_lba) {
                serial_printf("[partition] GPT entry %u invalid bounds: [%llu..%llu]\n",
                              scanned, entry.start_lba, entry.end_lba);
                gpt_entry_free(&entry);
                continue;
            }

            uint64_t count = entry.end_lba - entry.start_lba + 1;

            /* Check overlap with already parsed partitions */
            bool overlap = false;
            for (uint32_t p = 0; p < table->count; p++) {
                if (check_overlap(entry.start_lba, count, table->partitions[p].start_lba, table->partitions[p].sector_count)) {
                    serial_printf("[partition] GPT partition %u overlaps with partition %u\n", scanned, p + 1);
                    overlap = true;
                    break;
                }
            }
            if (overlap) {
                gpt_entry_free(&entry);
                continue;
            }

            /* Add to self-expanding GPT table */
            gpt_table_add(&gpt_tbl, &entry);

            /* Normalize into self-expanding partition table */
            partition_info_t p;
            partition_info_init(&p);
            p.part_number = table->count + 1;
            p.start_lba = entry.start_lba;
            p.sector_count = count;
            if (entry.type_guid && entry.type_guid_len > 0) {
                partition_info_set_gpt_type_guid(&p, entry.type_guid, entry.type_guid_len);
            }
            if (entry.unique_guid && entry.unique_guid_len > 0) {
                partition_info_set_unique_guid(&p, entry.unique_guid, entry.unique_guid_len);
            }
            p.flags = (uint32_t)(entry.attributes & 0xFFFFFFFF);
            p.valid = true;

            /* Convert UTF-16LE label to ASCII */
            if (entry.name && entry.name_len > 0) {
                char *tmp_name = (char *)kmalloc(entry.name_len + 1);
                if (tmp_name) {
                    size_t c;
                    for (c = 0; c < entry.name_len; c++) {
                        uint16_t ch = entry.name[c];
                        tmp_name[c] = (ch > 0 && ch < 128) ? (char)ch : 0;
                        if (ch == 0) break;
                    }
                    tmp_name[c] = '\0';
                    if (tmp_name[0] != '\0') {
                        partition_info_set_name(&p, tmp_name);
                    } else {
                        char fallback[64];
                        snprintf(fallback, sizeof(fallback), "GPT-Part-%u", p.part_number);
                        partition_info_set_name(&p, fallback);
                    }
                    kfree(tmp_name);
                }
            } else {
                char fallback[64];
                snprintf(fallback, sizeof(fallback), "GPT-Part-%u", p.part_number);
                partition_info_set_name(&p, fallback);
            }

            partition_table_add(table, &p);
            partition_info_free(&p);
            gpt_entry_free(&entry);
        }
    }

    gpt_table_free(&gpt_tbl);
    gpt_header_free(&hdr);
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
    out_table->capacity = 0;
    out_table->count = 0;
    out_table->partitions = NULL;

    uint8_t mbr_buf[512];
    if (!block_dev_read(bdev, 0, 1, mbr_buf)) {
        serial_printf("[partition] Failed to read LBA 0 from '%s'\n", bdev->name);
        return -2;
    }

    /* Check MBR signature 0x55, 0xAA */
    if (mbr_buf[510] != 0x55 || mbr_buf[511] != 0xAA) {
        serial_printf("[partition] Device '%s': No MBR signature 0xAA55\n", bdev->name);
        return 0;
    }

    /* Unpack into self-expanding MBR table */
    mbr_table_t mbr_tbl;
    mbr_table_init(&mbr_tbl);

    for (int i = 0; i < 4; i++) {
        const uint8_t *raw = mbr_buf + 446 + (i * 16);
        mbr_entry_raw_t entry;
        mbr_entry_init(&entry);
        entry.status = raw[0];
        mbr_entry_set_chs_first(&entry, &raw[1], 3);
        entry.type = raw[4];
        mbr_entry_set_chs_last(&entry, &raw[5], 3);
        entry.lba_start = (uint32_t)raw[8] | ((uint32_t)raw[9] << 8) |
                          ((uint32_t)raw[10] << 16) | ((uint32_t)raw[11] << 24);
        entry.sector_count = (uint32_t)raw[12] | ((uint32_t)raw[13] << 8) |
                             ((uint32_t)raw[14] << 16) | ((uint32_t)raw[15] << 24);
        mbr_table_add(&mbr_tbl, &entry);
        mbr_entry_free(&entry);
    }

    /* Check for GPT Protective MBR */
    if (mbr_tbl.count > 0 && mbr_tbl.entries[0].type == 0xEE) {
        if (parse_gpt(bdev, out_table) == 0 && out_table->count > 0) {
            mbr_table_free(&mbr_tbl);
            return (int)out_table->count;
        }
    }

    /* Standard MBR parsing */
    out_table->type = PARTITION_TABLE_MBR;
    out_table->count = 0;

    for (uint32_t i = 0; i < mbr_tbl.count; i++) {
        mbr_entry_raw_t *e = &mbr_tbl.entries[i];
        if (e->type == 0 || e->sector_count == 0) continue;

        /* Bounds check against block device capacity */
        if (bdev->total_sectors > 0 &&
            ((uint64_t)e->lba_start + (uint64_t)e->sector_count > bdev->total_sectors)) {
            serial_printf("[partition] MBR entry %u out of bounds: lba=%u count=%u total=%llu\n",
                          i + 1, e->lba_start, e->sector_count, bdev->total_sectors);
            continue;
        }

        /* Overlap check with previous valid partitions */
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

        /* Dynamically probe partition first sector for filesystem signature */
        uint8_t pbuf[512];
        bool probed = false;
        if (block_dev_read(bdev, e->lba_start, 1, pbuf)) {
            if (memcmp(pbuf, "IPO_FS", 6) == 0) {
                char tmp[64];
                snprintf(tmp, sizeof(tmp), "IPO_FS (0x%02X)", e->type);
                partition_info_set_name(&p, tmp);
                probed = true;
            }
        }
        if (!probed) {
            char tmp[64];
            snprintf(tmp, sizeof(tmp), "Partition %u (0x%02X)", p.part_number, e->type);
            partition_info_set_name(&p, tmp);
        }

        partition_table_add(out_table, &p);
        partition_info_free(&p);
    }

    mbr_table_free(&mbr_tbl);
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
    if (p->name) {
        kfree(p->name);
        p->name = NULL;
    }
    p->name_len = 0;
    p->name_cap = 0;
    if (!name) return 0;

    size_t len = strlen(name);
    size_t cap = len + 1;
    p->name = (char *)kmalloc(cap);
    if (!p->name) return -1;
    memcpy(p->name, name, len);
    p->name[len] = '\0';
    p->name_len = len;
    p->name_cap = cap;
    return 0;
}

int partition_info_set_gpt_type_guid(partition_info_t *p, const uint8_t *guid, size_t len) {
    if (!p) return -1;
    if (p->gpt_type_guid) {
        kfree(p->gpt_type_guid);
        p->gpt_type_guid = NULL;
    }
    p->gpt_type_guid_len = 0;
    p->gpt_type_guid_cap = 0;
    if (!guid || len == 0) return 0;

    p->gpt_type_guid = (uint8_t *)kmalloc(len);
    if (!p->gpt_type_guid) return -1;
    memcpy(p->gpt_type_guid, guid, len);
    p->gpt_type_guid_len = len;
    p->gpt_type_guid_cap = len;
    return 0;
}

int partition_info_set_unique_guid(partition_info_t *p, const uint8_t *guid, size_t len) {
    if (!p) return -1;
    if (p->unique_guid) {
        kfree(p->unique_guid);
        p->unique_guid = NULL;
    }
    p->unique_guid_len = 0;
    p->unique_guid_cap = 0;
    if (!guid || len == 0) return 0;

    p->unique_guid = (uint8_t *)kmalloc(len);
    if (!p->unique_guid) return -1;
    memcpy(p->unique_guid, guid, len);
    p->unique_guid_len = len;
    p->unique_guid_cap = len;
    return 0;
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
        partition_info_set_gpt_type_guid(dst, src->gpt_type_guid, src->gpt_type_guid_len);
    }
    if (src->unique_guid && src->unique_guid_len > 0) {
        partition_info_set_unique_guid(dst, src->unique_guid, src->unique_guid_len);
    }
    if (src->name) {
        partition_info_set_name(dst, src->name);
    }
    return 0;
}

void partition_print_table(const partition_table_t *table) {
    if (!table) return;

    const char *type_name = "None";
    if (table->type == PARTITION_TABLE_MBR) type_name = "MBR (Master Boot Record)";
    else if (table->type == PARTITION_TABLE_GPT) type_name = "GPT (GUID Partition Table)";

    printf("  Device: %s  Partition Table: %s  Partitions: %u\n",
           table->bdev_name, type_name, table->count);

    if (table->count == 0) {
        printf("    No valid partitions detected.\n");
        return;
    }

    printf("    #  Boot  Start LBA       End LBA         Sectors     Size (MB)  Type/Label\n");
    printf("    --------------------------------------------------------------------------\n");

    for (uint32_t i = 0; i < table->count; i++) {
        const partition_info_t *p = &table->partitions[i];
        if (!p->valid) continue;

        uint64_t end_lba = p->start_lba + p->sector_count - 1;
        uint32_t size_mb = (uint32_t)((p->sector_count * 512ull) / (1024ull * 1024ull));

        printf("    %u   %s   %-15llu %-15llu %-11llu %-10u %s\n",
               p->part_number,
               (p->flags & 1) ? "*" : " ",
               p->start_lba,
               end_lba,
               p->sector_count,
               size_mb,
               p->name ? p->name : "");
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
    partition_info_copy(&table->partitions[table->count], part);
    table->partitions[table->count].valid = true;
    table->count++;
    return 0;
}


