#include <storage/storage_pool.h>
#include <driver/storage/block_dev.h>
#include <driver/storage/partition.h>
#include <driver/ata/ata.h>
#include <file_system/ipo_fs.h>
#include <kernel/smp.h>
#include <system/timer.h>
#include <memory/kmalloc.h>
#include <string.h>
#include <stdio.h>

_Static_assert(sizeof(storage_pool_raw_member_t) == 48, "storage_pool_raw_member_t must be exactly 48 bytes");
_Static_assert(sizeof(storage_pool_raw_sb_t) == 68, "storage_pool_raw_sb_t header must be exactly 68 bytes");
_Static_assert(sizeof(storage_raw_disk_tx_entry_t) == 64, "storage_raw_disk_tx_entry_t must be exactly 64 bytes");

#define STORAGE_ENTRIES_PER_SECTOR (512 / sizeof(storage_raw_disk_tx_entry_t))
#define STORAGE_DISK_WAL_SLOTS     (STORAGE_POOL_JOURNAL_SECTORS * STORAGE_ENTRIES_PER_SECTOR)

static storage_pool_t s_storage_pool;
static spinlock_t     s_pool_lock = {0};
static uint32_t       s_next_tx_id = 1;

static int storage_pool_set_pool_uuid(const uint8_t *uuid, size_t len) {
    if (!uuid || len == 0) return 0;
    if (len + 1 > s_storage_pool.pool_uuid_cap || !s_storage_pool.pool_uuid) {
        size_t new_cap = len + 1 < 17 ? 17 : len + 1;
        uint8_t *nb = (uint8_t *)krealloc(s_storage_pool.pool_uuid, new_cap);
        if (!nb) return -1;
        s_storage_pool.pool_uuid = nb;
        s_storage_pool.pool_uuid_cap = new_cap;
    }
    memcpy(s_storage_pool.pool_uuid, uuid, len);
    s_storage_pool.pool_uuid[len] = '\0';
    s_storage_pool.pool_uuid_len = len;
    return 0;
}

static void storage_pool_cleanup_unlocked(void) {
    if (s_storage_pool.pool_uuid) {
        kfree(s_storage_pool.pool_uuid);
        s_storage_pool.pool_uuid = NULL;
    }
    s_storage_pool.pool_uuid_len = 0;
    s_storage_pool.pool_uuid_cap = 0;

    if (s_storage_pool.members) {
        for (uint32_t i = 0; i < s_storage_pool.member_count; i++) {
            pool_member_free(&s_storage_pool.members[i]);
        }
        kfree(s_storage_pool.members);
        s_storage_pool.members = NULL;
    }
    s_storage_pool.member_count = 0;
    s_storage_pool.member_capacity = 0;

    if (s_storage_pool.journal.entries) {
        for (size_t i = 0; i < s_storage_pool.journal.entries_cap; i++) {
            storage_tx_entry_free(&s_storage_pool.journal.entries[i]);
        }
        kfree(s_storage_pool.journal.entries);
        s_storage_pool.journal.entries = NULL;
    }
    memset(&s_storage_pool.journal, 0, sizeof(s_storage_pool.journal));
    s_storage_pool.initialized = false;
}

/* Forward declarations of unlocked helpers (caller must hold s_pool_lock) */
static int  storage_pool_save_metadata_unlocked(void);
static int  storage_pool_load_metadata_unlocked(void);
static int  storage_pool_recover_unlocked(void);
static bool storage_pool_read_unlocked(uint64_t pool_lba, uint16_t count, void *buf);
static bool storage_pool_write_unlocked(uint64_t pool_lba, uint16_t count, const void *buf);

storage_pool_state_t storage_pool_get_state(void) {
    return s_storage_pool.state;
}

const char *storage_pool_get_state_str(void) {
    switch (s_storage_pool.state) {
        case STORAGE_POOL_STATE_ONLINE:   return "ONLINE";
        case STORAGE_POOL_STATE_DEGRADED: return "DEGRADED (Spanned JBOD - Member Missing)";
        case STORAGE_POOL_STATE_CORRUPT:  return "CORRUPT (Metadata Invalid)";
        default:                          return "UNKNOWN";
    }
}

static void in_memory_journal_record(tx_op_type_t op, uint64_t lba, uint16_t count,
                                    tx_state_t state, uint32_t data_crc) {
    storage_journal_t *j = &s_storage_pool.journal;
    if (j->entries_cap == 0 || !j->entries) {
        size_t init_cap = 64;
        storage_tx_entry_t *new_entries = (storage_tx_entry_t *)kmalloc(init_cap * sizeof(storage_tx_entry_t));
        if (!new_entries) return;
        for (size_t i = 0; i < init_cap; i++) storage_tx_entry_init(&new_entries[i]);
        j->entries = new_entries;
        j->entries_cap = init_cap;
    } else if (j->entries_count >= j->entries_cap) {
        size_t new_cap = j->entries_cap * 2;
        storage_tx_entry_t *new_entries = (storage_tx_entry_t *)krealloc(j->entries, new_cap * sizeof(storage_tx_entry_t));
        if (new_entries) {
            for (size_t i = j->entries_cap; i < new_cap; i++) storage_tx_entry_init(&new_entries[i]);
            j->entries = new_entries;
            j->entries_cap = new_cap;
        }
    }
    uint32_t idx = j->head % j->entries_cap;
    storage_tx_entry_t *e = &j->entries[idx];

    e->tx_id = s_next_tx_id++;
    e->op = op;
    e->lba = lba;
    e->count = count;
    e->timestamp = timer_millis();
    e->state = state;
    e->data_crc = data_crc;
    e->crc = partition_crc32(e, offsetof(storage_tx_entry_t, crc));

    j->head++;
    j->total_tx_count++;
    if (j->entries_count < j->entries_cap) j->entries_count++;
    if (state == TX_STATE_PENDING) {
        j->active_tx_count++;
    } else if (j->active_tx_count > 0 && (state == TX_STATE_COMMITTED || state == TX_STATE_FAILED)) {
        j->active_tx_count--;
    }
}

static void in_memory_journal_update_last(tx_state_t state) {
    storage_journal_t *j = &s_storage_pool.journal;
    if (j->head == 0 || j->entries_cap == 0 || !j->entries) return;
    uint32_t idx = (j->head - 1) % j->entries_cap;
    storage_tx_entry_t *e = &j->entries[idx];
    if (e->state == TX_STATE_PENDING && (state == TX_STATE_COMMITTED || state == TX_STATE_FAILED)) {
        if (j->active_tx_count > 0) j->active_tx_count--;
    }
    e->state = state;
    e->crc = partition_crc32(e, offsetof(storage_tx_entry_t, crc));
}

/* CRC of on-disk WAL entry computed over the packed raw representation */
static uint32_t wal_entry_crc(const storage_disk_tx_entry_t *e) {
    storage_raw_disk_tx_entry_t raw;
    storage_disk_tx_entry_to_raw(e, &raw);
    return partition_crc32(&raw, offsetof(storage_raw_disk_tx_entry_t, entry_crc));
}

/* Write WAL journal entry directly to on-disk sector and flush */
static bool disk_journal_write_entry(uint32_t slot, const storage_disk_tx_entry_t *entry) {
    if (s_storage_pool.member_count == 0 || !s_storage_pool.members[0].online || !s_storage_pool.members[0].bdev) {
        return false;
    }

    uint32_t sector_offset = slot / STORAGE_ENTRIES_PER_SECTOR;
    uint32_t entry_in_sector = slot % STORAGE_ENTRIES_PER_SECTOR;
    uint64_t journal_start = s_storage_pool.journal_lba ? s_storage_pool.journal_lba : STORAGE_POOL_JOURNAL_LBA;
    uint64_t target_lba = journal_start + sector_offset;

    uint8_t sector_buf[512];
    /* Read current sector so other entries in sector are preserved */
    if (!block_dev_read(s_storage_pool.members[0].bdev, target_lba, 1, sector_buf)) {
        memset(sector_buf, 0, sizeof(sector_buf));
    }

    storage_raw_disk_tx_entry_t *entries = (storage_raw_disk_tx_entry_t *)sector_buf;
    storage_disk_tx_entry_to_raw(entry, &entries[entry_in_sector]);

    if (!block_dev_write(s_storage_pool.members[0].bdev, target_lba, 1, sector_buf)) {
        serial_printf("[storage_pool] WAL journal write failed at LBA %llu\n", target_lba);
        return false;
    }

    block_dev_flush(s_storage_pool.members[0].bdev);
    return true;
}

/* Save persistent metadata (Primary SB and Backup SB) - Unlocked */
static int storage_pool_save_metadata_unlocked(void) {
    if (!s_storage_pool.initialized || s_storage_pool.member_count == 0) return -1;

    s_storage_pool.generation++;
    uint64_t sb_lba = s_storage_pool.sb_lba ? s_storage_pool.sb_lba : STORAGE_POOL_PRIMARY_SB_LBA;
    uint64_t backup_sb_lba = s_storage_pool.backup_sb_lba ? s_storage_pool.backup_sb_lba : STORAGE_POOL_BACKUP_SB_LBA;

    for (uint32_t m = 0; m < s_storage_pool.member_count; m++) {
        pool_member_t *mem = &s_storage_pool.members[m];
        if (!mem->online || !mem->bdev) continue;

        storage_pool_disk_sb_t sb;
        storage_pool_disk_sb_init(&sb);
        sb.magic = STORAGE_POOL_MAGIC;
        sb.version = STORAGE_POOL_SB_VERSION;
        if (s_storage_pool.pool_uuid && s_storage_pool.pool_uuid_len > 0) {
            storage_pool_disk_sb_set_pool_uuid(&sb, s_storage_pool.pool_uuid, s_storage_pool.pool_uuid_len);
        }
        if (mem->uuid && mem->uuid_len > 0) {
            storage_pool_disk_sb_set_local_member_uuid(&sb, mem->uuid, mem->uuid_len);
        }
        sb.member_index = m;
        sb.member_count = s_storage_pool.member_count;
        sb.total_capacity_sectors = s_storage_pool.total_capacity_sectors;
        sb.chunk_size_sectors = s_storage_pool.chunk_size_sectors;
        sb.generation = s_storage_pool.generation;
        sb.state = (uint32_t)s_storage_pool.state;
        sb.recovery_count = s_storage_pool.recovery_count;

        for (uint32_t i = 0; i < s_storage_pool.member_count; i++) {
            pool_member_t *mi = &s_storage_pool.members[i];
            storage_pool_disk_member_t dm;
            storage_pool_disk_member_init(&dm);
            if (mi->uuid && mi->uuid_len > 0) {
                storage_pool_disk_member_set_uuid(&dm, mi->uuid, mi->uuid_len);
            }
            dm.member_index = (uint8_t)i;
            dm.flags = mi->online ? 0x01 : 0x02;
            if (mi->bdev_name) {
                storage_pool_disk_member_set_bdev_name(&dm, mi->bdev_name);
            }
            dm.start_pool_lba = mi->start_pool_lba;
            dm.capacity_sectors = mi->capacity_sectors;

            storage_pool_disk_sb_add_member(&sb, &dm);
            storage_pool_disk_member_free(&dm);
        }

        uint8_t raw_sb_buf[512];
        storage_pool_raw_sb_t *raw_sb = (storage_pool_raw_sb_t *)raw_sb_buf;
        storage_pool_disk_sb_to_raw(&sb, raw_sb);
        storage_pool_disk_sb_free(&sb);

        /* Calculate CRC32 of first 508 bytes */
        uint32_t crc = partition_crc32(raw_sb_buf, 508);
        *(uint32_t *)(raw_sb_buf + 508) = crc;

        /* Write Primary Superblock */
        if (!block_dev_write(mem->bdev, sb_lba, 1, raw_sb_buf)) {
            serial_printf("[storage_pool] Failed to write Primary SB to '%s' LBA %llu\n",
                          mem->bdev_name ? mem->bdev_name : "unknown", sb_lba);
        }

        /* Write Backup Superblock */
        if (!block_dev_write(mem->bdev, backup_sb_lba, 1, raw_sb_buf)) {
            serial_printf("[storage_pool] Failed to write Backup SB to '%s' LBA %llu\n",
                          mem->bdev_name ? mem->bdev_name : "unknown", backup_sb_lba);
        }

        block_dev_flush(mem->bdev);
    }

    return 0;
}

int storage_pool_save_metadata(void) {
    spin_lock(&s_pool_lock);
    int res = storage_pool_save_metadata_unlocked();
    spin_unlock(&s_pool_lock);
    return res;
}

/* Load persistent metadata and recover from backup superblock if necessary - Unlocked */
static int storage_pool_load_metadata_unlocked(void) {
    block_device_t *bdev = block_dev_get_list();
    if (!bdev) return -1;

    storage_pool_disk_sb_t best_sb;
    storage_pool_disk_sb_init(&best_sb);
    bool found_sb = false;
    uint32_t max_generation = 0;
    uint64_t found_sb_lba = 0;
    uint64_t found_backup_sb_lba = 0;
    uint64_t found_journal_lba = 0;
    block_device_t *sb_host_dev = NULL;

    block_device_t *curr = bdev;
    while (curr) {
        if (curr->total_sectors < 100) {
            curr = curr->next;
            continue;
        }

        uint8_t raw_buf[512];
        storage_pool_raw_sb_t *raw = (storage_pool_raw_sb_t *)raw_buf;
        bool sb_ok = false;
        bool from_backup = false;
        uint64_t curr_sb_lba = STORAGE_POOL_PRIMARY_SB_LBA;
        uint64_t curr_backup_lba = STORAGE_POOL_BACKUP_SB_LBA;
        uint64_t curr_journal_lba = STORAGE_POOL_JOURNAL_LBA;

        if (curr->total_sectors > curr_backup_lba) {
            /* Check Primary Superblock */
            if (block_dev_read(curr, curr_sb_lba, 1, raw_buf)) {
                if (raw->magic == STORAGE_POOL_MAGIC && raw->version == STORAGE_POOL_SB_VERSION) {
                    uint32_t calc_crc = partition_crc32(raw_buf, 508);
                    uint32_t stored_crc = *(uint32_t *)(raw_buf + 508);
                    if (calc_crc == stored_crc) {
                        sb_ok = true;
                    } else {
                        serial_printf("[storage_pool] '%s': Primary SB (LBA %llu) CRC mismatch\n",
                                      curr->name, curr_sb_lba);
                    }
                }
            }

            /* Check Backup Superblock if Primary failed */
            if (!sb_ok) {
                uint8_t backup_buf[512];
                storage_pool_raw_sb_t *backup_raw = (storage_pool_raw_sb_t *)backup_buf;
                if (block_dev_read(curr, curr_backup_lba, 1, backup_buf)) {
                    if (backup_raw->magic == STORAGE_POOL_MAGIC && backup_raw->version == STORAGE_POOL_SB_VERSION) {
                        uint32_t b_crc = partition_crc32(backup_buf, 508);
                        uint32_t stored_b_crc = *(uint32_t *)(backup_buf + 508);
                        if (b_crc == stored_b_crc) {
                            serial_printf("[storage_pool] '%s': Primary SB corrupted. Recovered from Backup SB (LBA %llu)!\n",
                                          curr->name, curr_backup_lba);
                            memcpy(raw_buf, backup_buf, 512);
                            sb_ok = true;
                            from_backup = true;

                            /* Repair corrupted Primary Superblock */
                            block_dev_write(curr, curr_sb_lba, 1, raw_buf);
                            block_dev_flush(curr);
                        }
                    }
                }
            }
        }

        if (sb_ok) {
            if (!found_sb || raw->generation > max_generation) {
                storage_pool_disk_sb_free(&best_sb);
                storage_pool_disk_sb_init(&best_sb);
                storage_pool_disk_sb_from_raw(&best_sb, raw);
                if (from_backup) best_sb.recovery_count++;
                max_generation = raw->generation;
                found_sb_lba = curr_sb_lba;
                found_backup_sb_lba = curr_backup_lba;
                found_journal_lba = curr_journal_lba;
                sb_host_dev = curr;
                found_sb = true;
            }
        }

        curr = curr->next;
    }

    if (!found_sb) {
        storage_pool_disk_sb_free(&best_sb);
        return -2; /* No valid superblocks found */
    }

    /* Clean up any old allocated state to prevent memory leaks */
    storage_pool_cleanup_unlocked();

    /* Restore storage pool state from loaded superblock */
    s_storage_pool.magic = best_sb.magic;
    s_storage_pool.version = best_sb.version;
    storage_pool_set_pool_uuid(best_sb.pool_uuid, best_sb.pool_uuid_len);
    s_storage_pool.total_capacity_sectors = best_sb.total_capacity_sectors;
    s_storage_pool.chunk_size_sectors = best_sb.chunk_size_sectors;
    s_storage_pool.generation = best_sb.generation;
    s_storage_pool.recovery_count = best_sb.recovery_count;
    s_storage_pool.sb_lba = found_sb_lba;
    s_storage_pool.backup_sb_lba = found_backup_sb_lba;
    s_storage_pool.journal_lba = found_journal_lba;

    uint32_t missing_members = 0;
    uint32_t loaded = (uint32_t)best_sb.members_count;

    /* Self-expand member array to fit all described members */
    if (loaded > s_storage_pool.member_capacity) {
        pool_member_t *nm = krealloc(s_storage_pool.members, loaded * sizeof(pool_member_t));
        if (!nm) {
            storage_pool_disk_sb_free(&best_sb);
            return -1;
        }
        s_storage_pool.members = nm;
        s_storage_pool.member_capacity = loaded;
    }
    s_storage_pool.member_count = loaded;

    for (uint32_t i = 0; i < loaded; i++) {
        pool_member_t *mem = &s_storage_pool.members[i];
        pool_member_init(mem);
        storage_pool_disk_member_t *dm = &best_sb.members[i];

        pool_member_set_name(mem, dm->bdev_name ? dm->bdev_name : "");
        if (dm->uuid && dm->uuid_len > 0) {
            pool_member_set_uuid(mem, dm->uuid, dm->uuid_len);
        }
        mem->start_pool_lba = dm->start_pool_lba;
        mem->capacity_sectors = dm->capacity_sectors;

        /* Look up physical block device */
        block_device_t *target_bdev = block_dev_get(mem->bdev_name);
        /* Portable fallback: if single-member pool moved to different device slot (e.g. ata1 -> ata0) */
        if (!target_bdev && i == 0 && loaded == 1 && sb_host_dev) {
            target_bdev = sb_host_dev;
            pool_member_set_name(mem, sb_host_dev->name);
        }

        if (target_bdev && target_bdev->total_sectors > 0) {
            mem->bdev = target_bdev;
            mem->online = true;
            target_bdev->flags |= BLOCK_DEV_POOL_MEMBER;
        } else {
            mem->bdev = NULL;
            mem->online = false;
            missing_members++;
            serial_printf("[storage_pool] WARNING: Member [%u] '%s' is MISSING / OFFLINE!\n",
                          i, mem->bdev_name);
        }
    }

    uint32_t total_members = loaded;
    storage_pool_disk_sb_free(&best_sb);

    if (missing_members > 0) {
        s_storage_pool.state = STORAGE_POOL_STATE_DEGRADED;
        serial_printf("[storage_pool] Pool status: DEGRADED (%u of %u members missing)\n",
                      missing_members, total_members);
    } else {
        s_storage_pool.state = STORAGE_POOL_STATE_ONLINE;
    }

    return 0;
}

int storage_pool_load_metadata(void) {
    spin_lock(&s_pool_lock);
    int res = storage_pool_load_metadata_unlocked();
    spin_unlock(&s_pool_lock);
    return res;
}

/* Crash recovery: scan on-disk WAL journal, identify uncommitted writes, replay or rollback - Unlocked */
static int storage_pool_recover_unlocked(void) {
    if (!s_storage_pool.members[0].online || !s_storage_pool.members[0].bdev) {
        return -1;
    }

    block_device_t *bdev0 = s_storage_pool.members[0].bdev;
    uint32_t recovered_count = 0;
    uint8_t sector_buf[512];

    uint64_t journal_start = s_storage_pool.journal_lba ? s_storage_pool.journal_lba : STORAGE_POOL_JOURNAL_LBA;

    serial_printf("[storage_pool] Scanning WAL journal (LBA %llu..%llu) for crash recovery...\n",
                  journal_start,
                  journal_start + STORAGE_POOL_JOURNAL_SECTORS - 1);

    for (uint32_t s = 0; s < STORAGE_POOL_JOURNAL_SECTORS; s++) {
        uint64_t sector_lba = journal_start + s;
        if (!block_dev_read(bdev0, sector_lba, 1, sector_buf)) {
            continue;
        }

        storage_raw_disk_tx_entry_t *entries = (storage_raw_disk_tx_entry_t *)sector_buf;
        bool sector_modified = false;

        for (uint32_t e = 0; e < STORAGE_ENTRIES_PER_SECTOR; e++) {
            storage_raw_disk_tx_entry_t *tx = &entries[e];
            if (tx->magic != STORAGE_JOURNAL_ENTRY_MAGIC) continue;

            uint32_t calc_crc = partition_crc32(tx, offsetof(storage_raw_disk_tx_entry_t, entry_crc));
            if (calc_crc != tx->entry_crc) {
                serial_printf("[storage_pool] Corrupted journal entry detected at sector %llu slot %u\n",
                              sector_lba, e);
                continue;
            }

            if (tx->tx_id >= s_next_tx_id) {
                s_next_tx_id = tx->tx_id + 1;
            }

            s_storage_pool.journal.total_tx_count++;

            /* Interrupted write detected! */
            if (tx->state == TX_STATE_PENDING) {
                serial_printf("[storage_pool] CRASH RECOVERY: Found uncommitted TX #%u (LBA=%llu, count=%u, state=PENDING)\n",
                              tx->tx_id, tx->lba, tx->count);

                /* Verify whether data on media matches transaction data CRC using unlocked read */
                bool data_intact = false;
                if (tx->count == 1 && s_storage_pool.state != STORAGE_POOL_STATE_CORRUPT) {
                    uint8_t check_buf[512];
                    if (storage_pool_read_unlocked(tx->lba, 1, check_buf)) {
                        uint32_t disk_crc = partition_crc32(check_buf, 512);
                        if (disk_crc == tx->data_crc) {
                            data_intact = true;
                        }
                    }
                }

                if (data_intact) {
                    tx->state = TX_STATE_COMMITTED;
                    serial_printf("[storage_pool] CRASH RECOVERY: Data intact on disk. Committed TX #%u.\n", tx->tx_id);
                } else {
                    tx->state = TX_STATE_FAILED;
                    serial_printf("[storage_pool] CRASH RECOVERY: Write was incomplete. Rolled back TX #%u.\n", tx->tx_id);
                }

                tx->entry_crc = partition_crc32(tx, offsetof(storage_raw_disk_tx_entry_t, entry_crc));
                sector_modified = true;
                recovered_count++;
            }
        }

        if (sector_modified) {
            block_dev_write(bdev0, sector_lba, 1, sector_buf);
            block_dev_flush(bdev0);
        }
    }

    if (recovered_count > 0) {
        s_storage_pool.recovery_count += recovered_count;
        serial_printf("[storage_pool] Crash recovery completed: %u interrupted transaction(s) resolved.\n",
                      recovered_count);
        storage_pool_save_metadata_unlocked();
    }

    return (int)recovered_count;
}

int storage_pool_recover(void) {
    spin_lock(&s_pool_lock);
    int res = storage_pool_recover_unlocked();
    spin_unlock(&s_pool_lock);
    return res;
}

int storage_pool_create_ex(const char *pool_uuid, const char *bdev_name) {
    spin_lock(&s_pool_lock);

    /* Free any previous allocations to avoid memory leaks */
    storage_pool_cleanup_unlocked();

    memset(&s_storage_pool, 0, sizeof(s_storage_pool));
    s_storage_pool.member_capacity = 4;
    s_storage_pool.members = (pool_member_t *)kmalloc(s_storage_pool.member_capacity * sizeof(pool_member_t));
    if (!s_storage_pool.members) {
        spin_unlock(&s_pool_lock);
        return -1;
    }
    memset(s_storage_pool.members, 0, s_storage_pool.member_capacity * sizeof(pool_member_t));
    s_storage_pool.magic = STORAGE_POOL_MAGIC;
    s_storage_pool.version = STORAGE_POOL_SB_VERSION;
    s_storage_pool.chunk_size_sectors = 2048; /* 1MB extents */
    s_storage_pool.generation = 1;
    s_storage_pool.recovery_count = 0;
    s_storage_pool.state = STORAGE_POOL_STATE_ONLINE;

    s_storage_pool.sb_lba = STORAGE_POOL_PRIMARY_SB_LBA;
    s_storage_pool.journal_lba = STORAGE_POOL_JOURNAL_LBA;
    s_storage_pool.backup_sb_lba = STORAGE_POOL_BACKUP_SB_LBA;

    if (pool_uuid && *pool_uuid) {
        uint8_t ub[16];
        memset(ub, 0, sizeof(ub));
        strncpy((char *)ub, pool_uuid, 16);
        storage_pool_set_pool_uuid(ub, 16);
    } else {
        storage_pool_set_pool_uuid((const uint8_t *)"IPO_STORAGE_POOL", 16);
    }

    /* Live USB / Portable Architecture:
     * If specific device name given, look it up; otherwise bind to the system boot drive
     * (prefer ata0 if >= 2048 + 10 sectors, or first suitable drive).
     * Never automatically annex host PC internal drives without user consent. */
    block_device_t *target_bdev = NULL;
    if (bdev_name && *bdev_name) {
        target_bdev = block_dev_get(bdev_name);
    }
    if (!target_bdev) {
        block_device_t *bdev = block_dev_get_list();
        while (bdev) {
            if (strcmp(bdev->name, "ata0") == 0 && bdev->total_sectors >= 2048 + 10) {
                target_bdev = bdev;
                break;
            }
            bdev = bdev->next;
        }
    }
    if (!target_bdev) {
        block_device_t *bdev = block_dev_get_list();
        while (bdev) {
            if (bdev->total_sectors >= 2048 + 10) {
                target_bdev = bdev;
                break;
            }
            bdev = bdev->next;
        }
    }
    if (!target_bdev) {
        target_bdev = block_dev_get_list();
    }

    if (!target_bdev || target_bdev->total_sectors == 0) {
        serial_printf("[storage_pool] Cannot create storage pool: no suitable block device found.\n");
        storage_pool_cleanup_unlocked();
        spin_unlock(&s_pool_lock);
        return -2;
    }

    pool_member_t *m = &s_storage_pool.members[0];
    pool_member_init(m);
    pool_member_set_name(m, target_bdev->name);
    m->bdev = target_bdev;
    m->start_pool_lba = 0;
    m->capacity_sectors = target_bdev->total_sectors;
    m->online = true;
    {
        char ubuf[16];
        memset(ubuf, 0, sizeof(ubuf));
        snprintf(ubuf, sizeof(ubuf), "MEMBER-%02u", 0);
        pool_member_set_uuid(m, (const uint8_t *)ubuf, 16);
    }

    target_bdev->flags |= BLOCK_DEV_POOL_MEMBER;
    s_storage_pool.total_capacity_sectors = target_bdev->total_sectors;
    s_storage_pool.member_count = 1;
    s_storage_pool.initialized = true;

    /* Initialize clear WAL journal on member 0 */
    if (s_storage_pool.members[0].bdev) {
        uint8_t clear_buf[512];
        memset(clear_buf, 0, sizeof(clear_buf));
        for (uint32_t s = 0; s < STORAGE_POOL_JOURNAL_SECTORS; s++) {
            block_dev_write(s_storage_pool.members[0].bdev, s_storage_pool.journal_lba + s, 1, clear_buf);
        }
        block_dev_flush(s_storage_pool.members[0].bdev);
    }

    /* Write persistent superblocks to all members */
    storage_pool_save_metadata_unlocked();

    spin_unlock(&s_pool_lock);

    serial_printf("[storage_pool] Created persistent pool '%.16s' on '%s': %u members, %llu sectors (%llu MB)\n",
                  s_storage_pool.pool_uuid,
                  target_bdev->name,
                  s_storage_pool.member_count,
                  s_storage_pool.total_capacity_sectors,
                  s_storage_pool.total_capacity_sectors / 2048ull);

    return 0;
}

int storage_pool_create(const char *pool_uuid) {
    return storage_pool_create_ex(pool_uuid, NULL);
}

int storage_pool_init(void) {
    if (s_storage_pool.initialized) return 0;

    spinlock_init(&s_pool_lock);
    spin_lock(&s_pool_lock);

    /* Try to load existing persistent metadata from disk */
    int load_res = storage_pool_load_metadata_unlocked();

    if (load_res == 0) {
        /* Successfully loaded persistent metadata from disk */
        s_storage_pool.initialized = true;

        /* Run crash recovery on WAL journal */
        storage_pool_recover_unlocked();

        spin_unlock(&s_pool_lock);

        serial_printf("[storage_pool] Restored from persistent disk metadata: state=%s, %u member(s), %llu sectors (%llu MB)\n",
                      storage_pool_get_state_str(),
                      s_storage_pool.member_count,
                      s_storage_pool.total_capacity_sectors,
                      s_storage_pool.total_capacity_sectors / 2048ull);
        return 0;
    }

    spin_unlock(&s_pool_lock);

    /* First-time pool creation (no existing persistent superblocks) */
    serial_printf("[storage_pool] No persistent superblock found on disk. Initializing default pool...\n");
    return storage_pool_create("IPO_STORAGE_POOL");
}

bool storage_pool_is_initialized(void) {
    return s_storage_pool.initialized;
}

uint64_t storage_pool_get_capacity(void) {
    if (!s_storage_pool.initialized) storage_pool_init();
    return s_storage_pool.total_capacity_sectors;
}

static void get_fs_stats(uint32_t *out_blk_used, uint32_t *out_blk_total,
                         uint32_t *out_ino_used, uint32_t *out_ino_total) {
    if (out_blk_used) *out_blk_used = 0;
    if (out_blk_total) *out_blk_total = 0;
    if (out_ino_used) *out_ino_used = 0;
    if (out_ino_total) *out_ino_total = 0;
    if (!fs_mounted) return;

    uint32_t data_start = (uint32_t)sb.data_blocks_start;
    uint32_t total = (sb.fs_size_blocks > data_start)
                     ? (uint32_t)(sb.fs_size_blocks - data_start) : 0;
    if (out_blk_total) *out_blk_total = total;
    if (out_blk_used && total > 0) {
        *out_blk_used = bitmap_count_set(sb.block_bitmap_start, total);
    }
    if (out_ino_total) *out_ino_total = (uint32_t)sb.inode_count;
    if (out_ino_used && sb.inode_count > 0) {
        *out_ino_used = bitmap_count_set(sb.inode_bitmap_start, sb.inode_count);
    }
}

uint64_t storage_pool_get_used(void) {
    if (fs_mounted) {
        uint32_t blk_used = 0;
        get_fs_stats(&blk_used, NULL, NULL, NULL);
        return (uint64_t)blk_used;
    }
    return 0;
}

uint64_t storage_pool_get_free(void) {
    uint64_t cap = storage_pool_get_capacity();
    uint64_t used = storage_pool_get_used();
    return (cap > used) ? (cap - used) : 0;
}

uint32_t storage_pool_get_member_count(void) {
    if (!s_storage_pool.initialized) storage_pool_init();
    return s_storage_pool.member_count;
}

const storage_pool_t *storage_pool_get_info(void) {
    if (!s_storage_pool.initialized) storage_pool_init();
    return &s_storage_pool;
}

int storage_pool_add_device(block_device_t *bdev) {
    if (!bdev || bdev->total_sectors == 0) return -1;
    if (!s_storage_pool.initialized) storage_pool_init();

    spin_lock(&s_pool_lock);

    /* Check if already added */
    for (uint32_t i = 0; i < s_storage_pool.member_count; i++) {
        if (strcmp(s_storage_pool.members[i].bdev_name, bdev->name) == 0) {
            if (!s_storage_pool.members[i].online) {
                s_storage_pool.members[i].bdev = bdev;
                s_storage_pool.members[i].online = true;
                /* Check if all members are back online */
                bool all_online = true;
                for (uint32_t k = 0; k < s_storage_pool.member_count; k++) {
                    if (!s_storage_pool.members[k].online) {
                        all_online = false;
                        break;
                    }
                }
                if (all_online) s_storage_pool.state = STORAGE_POOL_STATE_ONLINE;
                storage_pool_save_metadata_unlocked();
                spin_unlock(&s_pool_lock);
                serial_printf("[storage_pool] Re-attached member '%s', state now %s\n",
                              bdev->name, storage_pool_get_state_str());
                return 0;
            }
            spin_unlock(&s_pool_lock);
            return 0;
        }
    }

    if (s_storage_pool.member_count >= s_storage_pool.member_capacity) {
        uint32_t new_cap = s_storage_pool.member_capacity ? s_storage_pool.member_capacity * 2 : 4;
        pool_member_t *nm = (pool_member_t *)krealloc(s_storage_pool.members, new_cap * sizeof(pool_member_t));
        if (!nm) {
            spin_unlock(&s_pool_lock);
            return -2;
        }
        s_storage_pool.members = nm;
        s_storage_pool.member_capacity = new_cap;
    }

    uint32_t idx = s_storage_pool.member_count;
    pool_member_t *m = &s_storage_pool.members[idx];
    pool_member_init(m);
    pool_member_set_name(m, bdev->name);
    m->bdev = bdev;
    m->start_pool_lba = s_storage_pool.total_capacity_sectors;
    m->capacity_sectors = bdev->total_sectors;
    m->online = true;
    {
        char ubuf[16];
        memset(ubuf, 0, sizeof(ubuf));
        snprintf(ubuf, sizeof(ubuf), "MEMBER-%02u", idx);
        pool_member_set_uuid(m, (const uint8_t *)ubuf, 16);
    }

    bdev->flags |= BLOCK_DEV_POOL_MEMBER;
    s_storage_pool.total_capacity_sectors += bdev->total_sectors;
    s_storage_pool.member_count++;

    in_memory_journal_record(TX_OP_CHECK, m->start_pool_lba, 0, TX_STATE_COMMITTED, 0);

    /* Persist updated membership and capacity across all member disks */
    storage_pool_save_metadata_unlocked();

    spin_unlock(&s_pool_lock);

    serial_printf("[storage_pool] Attached member '%s' (+%llu sectors, total %llu)\n",
                  bdev->name, bdev->total_sectors, s_storage_pool.total_capacity_sectors);
    return 0;
}

static bool storage_pool_read_unlocked(uint64_t pool_lba, uint16_t count, void *buf) {
    if (count == 0 || buf == NULL) return false;

    if (pool_lba + (uint64_t)count > s_storage_pool.total_capacity_sectors) {
        serial_printf("[storage_pool] Read out of bounds: lba=%llu count=%u total=%llu\n",
                      pool_lba, count, s_storage_pool.total_capacity_sectors);
        return false;
    }

    /* Fast verification: Ensure all target member spans are healthy */
    uint64_t span_lba = pool_lba;
    uint16_t span_rem = count;

    for (uint32_t m = 0; m < s_storage_pool.member_count && span_rem > 0; m++) {
        pool_member_t *mem = &s_storage_pool.members[m];
        uint64_t mem_end = mem->start_pool_lba + mem->capacity_sectors;

        if (span_lba >= mem->start_pool_lba && span_lba < mem_end) {
            if (!mem->online || !mem->bdev) {
                serial_printf("[storage_pool] Read rejected: target sector %llu resides on offline member [%u] '%s'\n",
                              span_lba, m, mem->bdev_name);
                return false;
            }
            uint64_t avail = mem_end - span_lba;
            uint16_t chunk = (span_rem < avail) ? span_rem : (uint16_t)avail;
            span_lba += chunk;
            span_rem -= chunk;
        }
    }

    if (span_rem > 0) return false;

    in_memory_journal_record(TX_OP_READ, pool_lba, count, TX_STATE_PENDING, 0);

    uint64_t curr_lba = pool_lba;
    uint8_t *curr_buf = (uint8_t *)buf;
    uint16_t remaining = count;

    for (uint32_t m = 0; m < s_storage_pool.member_count && remaining > 0; m++) {
        pool_member_t *mem = &s_storage_pool.members[m];
        uint64_t mem_end = mem->start_pool_lba + mem->capacity_sectors;

        if (curr_lba >= mem->start_pool_lba && curr_lba < mem_end) {
            uint64_t offset_in_member = curr_lba - mem->start_pool_lba;
            uint64_t avail_in_member = mem->capacity_sectors - offset_in_member;
            uint16_t chunk = (remaining < avail_in_member) ? remaining : (uint16_t)avail_in_member;

            if (!block_dev_read(mem->bdev, offset_in_member, chunk, curr_buf)) {
                in_memory_journal_update_last(TX_STATE_FAILED);
                serial_printf("[storage_pool] Read failed on member '%s' at lba %llu\n",
                              mem->bdev_name, offset_in_member);
                return false;
            }

            curr_lba += chunk;
            curr_buf += (chunk * 512u);
            remaining -= chunk;
        }
    }

    bool success = (remaining == 0);
    in_memory_journal_update_last(success ? TX_STATE_COMMITTED : TX_STATE_FAILED);
    return success;
}

bool storage_pool_read(uint64_t pool_lba, uint16_t count, void *buf) {
    if (!s_storage_pool.initialized) storage_pool_init();
    spin_lock(&s_pool_lock);
    bool ok = storage_pool_read_unlocked(pool_lba, count, buf);
    spin_unlock(&s_pool_lock);
    return ok;
}

static bool storage_pool_write_unlocked(uint64_t pool_lba, uint16_t count, const void *buf) {
    if (count == 0 || buf == NULL) return false;

    if (pool_lba + (uint64_t)count > s_storage_pool.total_capacity_sectors) {
        serial_printf("[storage_pool] Write out of bounds: lba=%llu count=%u total=%llu\n",
                      pool_lba, count, s_storage_pool.total_capacity_sectors);
        return false;
    }

    /* In DEGRADED mode: strictly reject write if target range touches offline member */
    uint64_t span_lba = pool_lba;
    uint16_t span_rem = count;

    for (uint32_t m = 0; m < s_storage_pool.member_count && span_rem > 0; m++) {
        pool_member_t *mem = &s_storage_pool.members[m];
        uint64_t mem_end = mem->start_pool_lba + mem->capacity_sectors;

        if (span_lba >= mem->start_pool_lba && span_lba < mem_end) {
            if (!mem->online || !mem->bdev) {
                serial_printf("[storage_pool] Write REJECTED: target span [%llu..%llu] touches offline member [%u] '%s'\n",
                              span_lba, span_lba + span_rem - 1, m, mem->bdev_name);
                return false;
            }
            uint64_t avail = mem_end - span_lba;
            uint16_t chunk = (span_rem < avail) ? span_rem : (uint16_t)avail;
            span_lba += chunk;
            span_rem -= chunk;
        }
    }

    if (span_rem > 0) return false;

    /* Compute CRC32 of payload data */
    uint32_t data_crc = partition_crc32(buf, (size_t)count * 512u);

    /* 1. On-Disk Write-Ahead Log (WAL): Write PENDING transaction to disk before data */
    storage_journal_t *j = &s_storage_pool.journal;
    uint32_t slot = j->head % (uint32_t)STORAGE_DISK_WAL_SLOTS;
    in_memory_journal_record(TX_OP_WRITE, pool_lba, count, TX_STATE_PENDING, data_crc);

    storage_disk_tx_entry_t d_entry;
    storage_disk_tx_entry_init(&d_entry);
    d_entry.magic = STORAGE_JOURNAL_ENTRY_MAGIC;
    d_entry.tx_id = (j->entries_cap > 0 && j->head > 0) ? j->entries[(j->head - 1) % j->entries_cap].tx_id : s_next_tx_id++;
    d_entry.op = (uint16_t)TX_OP_WRITE;
    d_entry.state = (uint16_t)TX_STATE_PENDING;
    d_entry.lba = pool_lba;
    d_entry.count = count;
    d_entry.timestamp = timer_millis();
    d_entry.data_crc = data_crc;
    d_entry.entry_crc = wal_entry_crc(&d_entry);

    disk_journal_write_entry(slot, &d_entry);

    /* 2. Write data blocks to member device(s) */
    uint64_t curr_lba = pool_lba;
    const uint8_t *curr_buf = (const uint8_t *)buf;
    uint16_t remaining = count;
    bool write_ok = true;

    for (uint32_t m = 0; m < s_storage_pool.member_count && remaining > 0; m++) {
        pool_member_t *mem = &s_storage_pool.members[m];
        uint64_t mem_end = mem->start_pool_lba + mem->capacity_sectors;

        if (curr_lba >= mem->start_pool_lba && curr_lba < mem_end) {
            uint64_t offset_in_member = curr_lba - mem->start_pool_lba;
            uint64_t avail_in_member = mem->capacity_sectors - offset_in_member;
            uint16_t chunk = (remaining < avail_in_member) ? remaining : (uint16_t)avail_in_member;

            if (!block_dev_write(mem->bdev, offset_in_member, chunk, curr_buf)) {
                serial_printf("[storage_pool] Write failed on member '%s' at lba %llu\n",
                              mem->bdev_name, offset_in_member);
                write_ok = false;
                break;
            }

            block_dev_flush(mem->bdev);

            curr_lba += chunk;
            curr_buf += (chunk * 512u);
            remaining -= chunk;
        }
    }

    if (remaining > 0) write_ok = false;

    /* 3. Commit/finalize transaction on-disk and in memory */
    d_entry.state = write_ok ? (uint16_t)TX_STATE_COMMITTED : (uint16_t)TX_STATE_FAILED;
    d_entry.entry_crc = wal_entry_crc(&d_entry);
    disk_journal_write_entry(slot, &d_entry);
    storage_disk_tx_entry_free(&d_entry);

    in_memory_journal_update_last(write_ok ? TX_STATE_COMMITTED : TX_STATE_FAILED);

    return write_ok;
}

bool storage_pool_write(uint64_t pool_lba, uint16_t count, const void *buf) {
    if (!s_storage_pool.initialized) storage_pool_init();
    spin_lock(&s_pool_lock);
    bool ok = storage_pool_write_unlocked(pool_lba, count, buf);
    spin_unlock(&s_pool_lock);
    return ok;
}

bool storage_pool_flush(void) {
    if (!s_storage_pool.initialized) return true;
    spin_lock(&s_pool_lock);
    for (uint32_t m = 0; m < s_storage_pool.member_count; m++) {
        if (s_storage_pool.members[m].online && s_storage_pool.members[m].bdev) {
            block_dev_flush(s_storage_pool.members[m].bdev);
        }
    }
    in_memory_journal_record(TX_OP_FLUSH, 0, 0, TX_STATE_COMMITTED, 0);
    spin_unlock(&s_pool_lock);
    return true;
}

int storage_pool_check(void) {
    if (!s_storage_pool.initialized) storage_pool_init();

    printf("=== Storage Pool Integrity Check ===\n");
    printf("  Pool UUID:       %.16s\n", s_storage_pool.pool_uuid);
    printf("  Pool Status:     %s\n", storage_pool_get_state_str());
    printf("  Generation:      %u\n", s_storage_pool.generation);
    printf("  Recoveries:      %u\n", s_storage_pool.recovery_count);
    printf("  Member Count:    %u\n\n", s_storage_pool.member_count);

    int errors = 0;
    uint8_t test_buf[512];

    for (uint32_t m = 0; m < s_storage_pool.member_count; m++) {
        pool_member_t *mem = &s_storage_pool.members[m];
        printf("  [Member %u: %s]\n", m, mem->bdev_name);

        if (!mem->bdev || !mem->online) {
            printf("    Status:        OFFLINE / MISSING (FAIL)\n");
            errors++;
            continue;
        }

        /* 1. Sector 0 Read Test */
        if (!block_dev_read(mem->bdev, 0, 1, test_buf)) {
            printf("    Sector 0:      I/O Error (FAIL)\n");
            errors++;
        } else {
            printf("    Sector 0:      Read Verified (OK)\n");
        }

        /* 2. Primary Superblock Test */
        uint64_t sb_lba = s_storage_pool.sb_lba ? s_storage_pool.sb_lba : STORAGE_POOL_PRIMARY_SB_LBA;
        uint64_t backup_sb_lba = s_storage_pool.backup_sb_lba ? s_storage_pool.backup_sb_lba : STORAGE_POOL_BACKUP_SB_LBA;
        uint64_t journal_start = s_storage_pool.journal_lba ? s_storage_pool.journal_lba : STORAGE_POOL_JOURNAL_LBA;

        uint8_t psb_buf[512];
        storage_pool_raw_sb_t *psb = (storage_pool_raw_sb_t *)psb_buf;
        if (block_dev_read(mem->bdev, sb_lba, 1, psb_buf)) {
            uint32_t c_crc = partition_crc32(psb_buf, 508);
            uint32_t stored_crc = *(uint32_t *)(psb_buf + 508);
            if (psb->magic == STORAGE_POOL_MAGIC && psb->version == STORAGE_POOL_SB_VERSION && c_crc == stored_crc) {
                printf("    Primary SB:    LBA %llu Verified (OK, Gen %u, CRC 0x%08X)\n",
                       sb_lba, psb->generation, stored_crc);
            } else {
                printf("    Primary SB:    LBA %llu CORRUPT (FAIL, magic=0x%08X, crc=0x%08X, expected=0x%08X)\n",
                       sb_lba, psb->magic, stored_crc, c_crc);
                errors++;
            }
        } else {
            printf("    Primary SB:    LBA %llu Read Error (FAIL)\n", sb_lba);
            errors++;
        }

        /* 3. Backup Superblock Test */
        uint8_t bsb_buf[512];
        storage_pool_raw_sb_t *bsb = (storage_pool_raw_sb_t *)bsb_buf;
        if (block_dev_read(mem->bdev, backup_sb_lba, 1, bsb_buf)) {
            uint32_t b_crc = partition_crc32(bsb_buf, 508);
            uint32_t stored_b_crc = *(uint32_t *)(bsb_buf + 508);
            if (bsb->magic == STORAGE_POOL_MAGIC && bsb->version == STORAGE_POOL_SB_VERSION && b_crc == stored_b_crc) {
                printf("    Backup SB:     LBA %llu Verified (OK, Gen %u, CRC 0x%08X)\n",
                       backup_sb_lba, bsb->generation, stored_b_crc);
            } else {
                printf("    Backup SB:     LBA %llu CORRUPT (FAIL, crc=0x%08X, expected=0x%08X)\n",
                       backup_sb_lba, stored_b_crc, b_crc);
                errors++;
            }
        } else {
            printf("    Backup SB:     LBA %llu Read Error (FAIL)\n", backup_sb_lba);
            errors++;
        }

        /* 4. Partition Table Scan */
        partition_table_t ptable;
        int parts = partition_scan(mem->bdev, &ptable);
        if (parts >= 0) {
            const char *ptype = (ptable.type == PARTITION_TABLE_MBR) ? "MBR" :
                                (ptable.type == PARTITION_TABLE_GPT) ? "GPT" : "Raw";
            printf("    Partitions:    %s (%d partition%s detected)\n",
                   ptype, parts, parts == 1 ? "" : "s");
        }
        partition_free_table(&ptable);
    }

    /* 5. WAL On-Disk Journal Verification */
    uint64_t journal_start = s_storage_pool.journal_lba ? s_storage_pool.journal_lba : STORAGE_POOL_JOURNAL_LBA;
    printf("\n  [WAL On-Disk Journal (LBA %llu..%llu)]\n",
           journal_start, journal_start + STORAGE_POOL_JOURNAL_SECTORS - 1);

    uint32_t valid_entries = 0;
    uint32_t pending_entries = 0;
    uint32_t committed_entries = 0;
    uint32_t failed_entries = 0;

    if (s_storage_pool.members[0].online && s_storage_pool.members[0].bdev) {
        for (uint32_t s = 0; s < STORAGE_POOL_JOURNAL_SECTORS; s++) {
            if (block_dev_read(s_storage_pool.members[0].bdev, journal_start + s, 1, test_buf)) {
                storage_raw_disk_tx_entry_t *entries = (storage_raw_disk_tx_entry_t *)test_buf;
                for (uint32_t e = 0; e < STORAGE_ENTRIES_PER_SECTOR; e++) {
                    if (entries[e].magic == STORAGE_JOURNAL_ENTRY_MAGIC) {
                        uint32_t c_crc = partition_crc32(&entries[e], offsetof(storage_raw_disk_tx_entry_t, entry_crc));
                        if (c_crc == entries[e].entry_crc) {
                            valid_entries++;
                            if (entries[e].state == TX_STATE_PENDING) pending_entries++;
                            else if (entries[e].state == TX_STATE_COMMITTED) committed_entries++;
                            else if (entries[e].state == TX_STATE_FAILED) failed_entries++;
                        } else {
                            printf("    Journal Slot %u: CRC Mismatch (FAIL)\n", s * STORAGE_ENTRIES_PER_SECTOR + e);
                            errors++;
                        }
                    }
                }
            }
        }
        printf("    Total Valid Entries: %u (Committed: %u, Pending: %u, Failed: %u)\n",
               valid_entries, committed_entries, pending_entries, failed_entries);
    } else {
        printf("    Primary member unavailable to read journal.\n");
        errors++;
    }

    printf("\n=== Check Summary ===\n");
    if (errors == 0 && s_storage_pool.state == STORAGE_POOL_STATE_ONLINE) {
        printf("  Result: Storage Pool is Healthy, Persistent, and Consistent.\n");
    } else if (s_storage_pool.state == STORAGE_POOL_STATE_DEGRADED) {
        printf("  Result: Storage Pool is in DEGRADED State (%d issue(s)). Survived data accessible.\n", errors);
    } else {
        printf("  Result: Storage Pool has %d issue(s). Action required.\n", errors);
    }

    return errors;
}

void storage_pool_print_status(void) {
    if (!s_storage_pool.initialized) storage_pool_init();

    uint64_t cap = s_storage_pool.total_capacity_sectors;
    uint32_t cap_mb = (uint32_t)((cap * 512ull) / (1024ull * 1024ull));

    printf("=== Unified Storage Pool Status ===\n");
    printf("  Pool Name:        pool0\n");
    printf("  Pool UUID:        %.16s\n", s_storage_pool.pool_uuid);
    printf("  Status:           %s\n", storage_pool_get_state_str());
    printf("  Generation:       %u\n", s_storage_pool.generation);
    uint64_t sb_lba = s_storage_pool.sb_lba ? s_storage_pool.sb_lba : STORAGE_POOL_PRIMARY_SB_LBA;
    uint64_t backup_sb_lba = s_storage_pool.backup_sb_lba ? s_storage_pool.backup_sb_lba : STORAGE_POOL_BACKUP_SB_LBA;
    uint64_t journal_start = s_storage_pool.journal_lba ? s_storage_pool.journal_lba : STORAGE_POOL_JOURNAL_LBA;

    printf("  Primary SB:       Sector %llu (Backup: Sector %llu)\n",
           sb_lba, backup_sb_lba);
    printf("  WAL Journal:      Sectors %llu..%llu (%u sectors, %u entries)\n",
           journal_start,
           journal_start + STORAGE_POOL_JOURNAL_SECTORS - 1,
           STORAGE_POOL_JOURNAL_SECTORS, (uint32_t)s_storage_pool.journal.entries_cap);
    printf("  Total Capacity:   %llu sectors (%u MB)\n", cap, cap_mb);
    printf("  Chunk/Extent:     %u sectors (%u KB)\n",
           s_storage_pool.chunk_size_sectors, (s_storage_pool.chunk_size_sectors * 512u) / 1024u);
    printf("  Member Disks:     %u\n", s_storage_pool.member_count);

    for (uint32_t m = 0; m < s_storage_pool.member_count; m++) {
        pool_member_t *mem = &s_storage_pool.members[m];
        uint32_t mem_mb = (uint32_t)((mem->capacity_sectors * 512ull) / (1024ull * 1024ull));
        printf("    [%u] %-10s Offset: %-10llu Sectors: %-10llu Size: %-6u MB Status: %s\n",
               m, mem->bdev_name, mem->start_pool_lba, mem->capacity_sectors,
               mem_mb, mem->online ? "ONLINE" : "OFFLINE / MISSING");
    }

    printf("  Transaction Journal Stats:\n");
    printf("    Total Transactions:  %u\n", s_storage_pool.journal.total_tx_count);
    printf("    Active Transactions: %u\n", s_storage_pool.journal.active_tx_count);
    printf("    Ring Buffer Size:    %u entries\n", (uint32_t)s_storage_pool.journal.entries_cap);
}

void storage_pool_print_usage(void) {
    if (!s_storage_pool.initialized) storage_pool_init();

    uint64_t cap_sectors = s_storage_pool.total_capacity_sectors;
    uint32_t cap_mb = (uint32_t)((cap_sectors * 512ull) / (1024ull * 1024ull));

    uint32_t blk_used = 0, blk_total = 0;
    uint32_t ino_used = 0, ino_total = 0;
    if (fs_mounted) {
        get_fs_stats(&blk_used, &blk_total, &ino_used, &ino_total);
    }

    uint64_t used_sectors = (uint64_t)blk_used;
    uint64_t free_sectors = (cap_sectors > used_sectors) ? (cap_sectors - used_sectors) : 0;
    uint32_t used_mb = (uint32_t)((used_sectors * 512ull) / (1024ull * 1024ull));
    uint32_t free_mb = (uint32_t)((free_sectors * 512ull) / (1024ull * 1024ull));
    uint32_t pct = (cap_sectors > 0) ? (uint32_t)((used_sectors * 100ull) / cap_sectors) : 0;

    printf("=== Storage Pool Usage ===\n");
    printf("  Total Volume Size: %u MB (%llu sectors)\n", cap_mb, cap_sectors);
    printf("  Used Volume Space: %u MB (%llu sectors, %u%%)\n", used_mb, used_sectors, pct);
    printf("  Free Volume Space: %u MB (%llu sectors)\n", free_mb, free_sectors);
    printf("  Pool Status:       %s\n", storage_pool_get_state_str());

    if (fs_mounted) {
        printf("  Active Filesystem: IPO_FS (Block size: %u bytes, Total inodes: %u, Used inodes: %u)\n",
               sb.block_size, ino_total, ino_used);
    } else {
        printf("  Active Filesystem: None / Unformatted\n");
    }

    printf("\n  Member Drive Breakdown:\n");
    for (uint32_t m = 0; m < s_storage_pool.member_count; m++) {
        pool_member_t *mem = &s_storage_pool.members[m];
        uint32_t m_mb = (uint32_t)((mem->capacity_sectors * 512ull) / (1024ull * 1024ull));
        uint32_t m_pct = (cap_sectors > 0) ? (uint32_t)((mem->capacity_sectors * 100ull) / cap_sectors) : 0;
        printf("    %-10s: %-6u MB (Pool Extent [%llu..%llu], %u%% of Pool) [%s]\n",
               mem->bdev_name, m_mb, mem->start_pool_lba,
               mem->start_pool_lba + mem->capacity_sectors - 1, m_pct,
               mem->online ? "ONLINE" : "OFFLINE");
    }
}

void storage_pool_disk_member_init(storage_pool_disk_member_t *m) {
    if (!m) return;
    memset(m, 0, sizeof(*m));
}

void storage_pool_disk_member_free(storage_pool_disk_member_t *m) {
    if (!m) return;
    if (m->uuid) {
        kfree(m->uuid);
        m->uuid = NULL;
    }
    m->uuid_len = 0;
    m->uuid_cap = 0;

    if (m->bdev_name) {
        kfree(m->bdev_name);
        m->bdev_name = NULL;
    }
    m->bdev_name_len = 0;
    m->bdev_name_cap = 0;
}

int storage_pool_disk_member_set_uuid(storage_pool_disk_member_t *m, const uint8_t *uuid, size_t len) {
    if (!m) return -1;
    if (m->uuid) {
        kfree(m->uuid);
        m->uuid = NULL;
    }
    m->uuid_len = 0;
    m->uuid_cap = 0;
    if (!uuid || len == 0) return 0;
    m->uuid = (uint8_t *)kmalloc(len);
    if (!m->uuid) return -1;
    memcpy(m->uuid, uuid, len);
    m->uuid_len = len;
    m->uuid_cap = len;
    return 0;
}

int storage_pool_disk_member_set_bdev_name(storage_pool_disk_member_t *m, const char *name) {
    if (!m) return -1;
    if (m->bdev_name) {
        kfree(m->bdev_name);
        m->bdev_name = NULL;
    }
    m->bdev_name_len = 0;
    m->bdev_name_cap = 0;
    if (!name) return 0;
    size_t len = strlen(name);
    size_t cap = len + 1;
    m->bdev_name = (char *)kmalloc(cap);
    if (!m->bdev_name) return -1;
    memcpy(m->bdev_name, name, len);
    m->bdev_name[len] = '\0';
    m->bdev_name_len = len;
    m->bdev_name_cap = cap;
    return 0;
}

int storage_pool_disk_member_copy(storage_pool_disk_member_t *dst, const storage_pool_disk_member_t *src) {
    if (!dst) return -1;
    storage_pool_disk_member_free(dst);
    if (!src) return 0;
    dst->member_index = src->member_index;
    dst->flags = src->flags;
    dst->start_pool_lba = src->start_pool_lba;
    dst->capacity_sectors = src->capacity_sectors;
    if (src->uuid && src->uuid_len > 0) {
        storage_pool_disk_member_set_uuid(dst, src->uuid, src->uuid_len);
    }
    if (src->bdev_name) {
        storage_pool_disk_member_set_bdev_name(dst, src->bdev_name);
    }
    return 0;
}

void storage_pool_disk_member_from_raw(storage_pool_disk_member_t *m, const storage_pool_raw_member_t *raw) {
    if (!m || !raw) return;
    storage_pool_disk_member_free(m);
    storage_pool_disk_member_set_uuid(m, raw->uuid, 16);
    m->member_index = raw->member_index;
    m->flags = raw->flags;
    char name_buf[15];
    memcpy(name_buf, raw->bdev_name, 14);
    name_buf[14] = '\0';
    storage_pool_disk_member_set_bdev_name(m, name_buf);
    m->start_pool_lba = raw->start_pool_lba;
    m->capacity_sectors = raw->capacity_sectors;
}

void storage_pool_disk_member_to_raw(const storage_pool_disk_member_t *m, storage_pool_raw_member_t *raw) {
    if (!raw) return;
    memset(raw, 0, sizeof(*raw));
    if (!m) return;
    if (m->uuid && m->uuid_len > 0) {
        memcpy(raw->uuid, m->uuid, m->uuid_len < 16 ? m->uuid_len : 16);
    }
    raw->member_index = m->member_index;
    raw->flags = m->flags;
    if (m->bdev_name) {
        strncpy(raw->bdev_name, m->bdev_name, sizeof(raw->bdev_name) - 1);
    }
    raw->start_pool_lba = m->start_pool_lba;
    raw->capacity_sectors = m->capacity_sectors;
}


/* ---- pool_member_t ---- */
void pool_member_init(pool_member_t *m) {
    if (!m) return;
    memset(m, 0, sizeof(*m));
}

void pool_member_free(pool_member_t *m) {
    if (!m) return;
    if (m->bdev_name) kfree(m->bdev_name);
    if (m->uuid) kfree(m->uuid);
    memset(m, 0, sizeof(*m));
}

int pool_member_set_name(pool_member_t *m, const char *name) {
    if (!m || !name) return -1;
    size_t len = strlen(name);
    if (len + 1 > m->bdev_name_cap || !m->bdev_name) {
        char *nb = (char *)krealloc(m->bdev_name, len + 1);
        if (!nb) return -1;
        m->bdev_name = nb;
        m->bdev_name_cap = len + 1;
    }
    memcpy(m->bdev_name, name, len + 1);
    m->bdev_name_len = len;
    return 0;
}

int pool_member_set_uuid(pool_member_t *m, const uint8_t *uuid, size_t len) {
    if (!m || (!uuid && len)) return -1;
    if (len > m->uuid_cap || !m->uuid) {
        size_t cap = len ? len : 1;
        uint8_t *nb = (uint8_t *)krealloc(m->uuid, cap);
        if (!nb) return -1;
        m->uuid = nb;
        m->uuid_cap = cap;
    }
    if (len) memcpy(m->uuid, uuid, len);
    m->uuid_len = len;
    return 0;
}

/* ---- generic dynamic byte buffer helper ---- */
static int sp_buf_set(uint8_t **buf, size_t *len, size_t *cap, const uint8_t *data, size_t n) {
    if (n > *cap || !*buf) {
        size_t nc = n ? n : 1;
        uint8_t *nb = (uint8_t *)krealloc(*buf, nc);
        if (!nb) return -1;
        *buf = nb;
        *cap = nc;
    }
    if (n) memcpy(*buf, data, n);
    *len = n;
    return 0;
}

/* ---- storage_tx_entry_t ---- */
void storage_tx_entry_init(storage_tx_entry_t *e) {
    if (e) memset(e, 0, sizeof(*e));
}

void storage_tx_entry_free(storage_tx_entry_t *e) {
    if (!e) return;
    if (e->extra_data) kfree(e->extra_data);
    memset(e, 0, sizeof(*e));
}

int storage_tx_entry_set_extra_data(storage_tx_entry_t *e, const uint8_t *data, size_t len) {
    if (!e) return -1;
    return sp_buf_set(&e->extra_data, &e->extra_data_len, &e->extra_data_cap, data, len);
}

int storage_tx_entry_copy(storage_tx_entry_t *dst, const storage_tx_entry_t *src) {
    if (!dst || !src) return -1;
    uint8_t *eb = dst->extra_data; size_t ec = dst->extra_data_cap;
    *dst = *src;
    dst->extra_data = eb; dst->extra_data_cap = ec; dst->extra_data_len = 0;
    return sp_buf_set(&dst->extra_data, &dst->extra_data_len, &dst->extra_data_cap,
                      src->extra_data, src->extra_data_len);
}

/* ---- storage_disk_tx_entry_t ---- */
void storage_disk_tx_entry_init(storage_disk_tx_entry_t *e) {
    if (e) memset(e, 0, sizeof(*e));
}

void storage_disk_tx_entry_free(storage_disk_tx_entry_t *e) {
    if (!e) return;
    if (e->reserved) kfree(e->reserved);
    memset(e, 0, sizeof(*e));
}

int storage_disk_tx_entry_set_reserved(storage_disk_tx_entry_t *e, const uint8_t *data, size_t len) {
    if (!e) return -1;
    return sp_buf_set(&e->reserved, &e->reserved_len, &e->reserved_cap, data, len);
}

void storage_disk_tx_entry_from_raw(storage_disk_tx_entry_t *e, const storage_raw_disk_tx_entry_t *raw) {
    if (!e || !raw) return;
    e->magic = raw->magic;
    e->tx_id = raw->tx_id;
    e->op = raw->op;
    e->state = raw->state;
    e->lba = raw->lba;
    e->count = raw->count;
    e->timestamp = raw->timestamp;
    e->data_crc = raw->data_crc;
    e->entry_crc = raw->entry_crc;
    storage_disk_tx_entry_set_reserved(e, raw->reserved, sizeof(raw->reserved));
}

void storage_disk_tx_entry_to_raw(const storage_disk_tx_entry_t *e, storage_raw_disk_tx_entry_t *raw) {
    if (!e || !raw) return;
    memset(raw, 0, sizeof(*raw));
    raw->magic = e->magic;
    raw->tx_id = e->tx_id;
    raw->op = e->op;
    raw->state = e->state;
    raw->lba = e->lba;
    raw->count = e->count;
    raw->timestamp = e->timestamp;
    raw->data_crc = e->data_crc;
    raw->entry_crc = e->entry_crc;
    if (e->reserved && e->reserved_len) {
        size_t n = e->reserved_len < sizeof(raw->reserved) ? e->reserved_len : sizeof(raw->reserved);
        memcpy(raw->reserved, e->reserved, n);
    }
}

/* ---- storage_pool_disk_sb_t ---- */
void storage_pool_disk_sb_init(storage_pool_disk_sb_t *sb) {
    if (sb) memset(sb, 0, sizeof(*sb));
}

void storage_pool_disk_sb_free(storage_pool_disk_sb_t *sb) {
    if (!sb) return;
    for (size_t i = 0; i < sb->members_count; i++) storage_pool_disk_member_free(&sb->members[i]);
    if (sb->members) kfree(sb->members);
    if (sb->pool_uuid) kfree(sb->pool_uuid);
    if (sb->local_member_uuid) kfree(sb->local_member_uuid);
    if (sb->reserved) kfree(sb->reserved);
    memset(sb, 0, sizeof(*sb));
}

int storage_pool_disk_sb_set_pool_uuid(storage_pool_disk_sb_t *sb, const uint8_t *uuid, size_t len) {
    if (!sb) return -1;
    return sp_buf_set(&sb->pool_uuid, &sb->pool_uuid_len, &sb->pool_uuid_cap, uuid, len);
}

int storage_pool_disk_sb_set_local_member_uuid(storage_pool_disk_sb_t *sb, const uint8_t *uuid, size_t len) {
    if (!sb) return -1;
    return sp_buf_set(&sb->local_member_uuid, &sb->local_member_uuid_len, &sb->local_member_uuid_cap, uuid, len);
}

int storage_pool_disk_sb_set_reserved(storage_pool_disk_sb_t *sb, const uint8_t *data, size_t len) {
    if (!sb) return -1;
    return sp_buf_set(&sb->reserved, &sb->reserved_len, &sb->reserved_cap, data, len);
}

int storage_pool_disk_sb_add_member(storage_pool_disk_sb_t *sb, const storage_pool_disk_member_t *m) {
    if (!sb || !m) return -1;
    if (sb->members_count >= sb->members_cap) {
        size_t nc = sb->members_cap ? sb->members_cap * 2 : 4;
        storage_pool_disk_member_t *nm = (storage_pool_disk_member_t *)krealloc(sb->members, nc * sizeof(*nm));
        if (!nm) return -1;
        sb->members = nm;
        sb->members_cap = nc;
    }
    storage_pool_disk_member_t *d = &sb->members[sb->members_count];
    storage_pool_disk_member_init(d);
    if (storage_pool_disk_member_copy(d, m) != 0) return -1;
    sb->members_count++;
    return 0;
}

void storage_pool_disk_sb_from_raw(storage_pool_disk_sb_t *sb, const storage_pool_raw_sb_t *raw) {
    if (!sb || !raw) return;
    sb->magic = raw->magic;
    sb->version = raw->version;
    storage_pool_disk_sb_set_pool_uuid(sb, raw->pool_uuid, sizeof(raw->pool_uuid));
    storage_pool_disk_sb_set_local_member_uuid(sb, raw->local_member_uuid, sizeof(raw->local_member_uuid));
    sb->member_index = raw->member_index;
    sb->member_count = raw->member_count;
    sb->total_capacity_sectors = raw->total_capacity_sectors;
    sb->chunk_size_sectors = raw->chunk_size_sectors;
    sb->generation = raw->generation;
    sb->state = raw->state;

    const uint8_t *sec = (const uint8_t *)raw;
    sb->recovery_count = *(const uint32_t *)(sec + 452);
    storage_pool_disk_sb_set_reserved(sb, sec + 456, 52);
    sb->crc32 = *(const uint32_t *)(sec + 508);

    /* Unlimited: add all members configured in on-disk header */
    for (uint32_t i = 0; i < raw->member_count; i++) {
        storage_pool_disk_member_t dm;
        storage_pool_disk_member_init(&dm);
        storage_pool_disk_member_from_raw(&dm, &raw->members[i]);
        storage_pool_disk_sb_add_member(sb, &dm);
        storage_pool_disk_member_free(&dm);
    }
}

void storage_pool_disk_sb_to_raw(const storage_pool_disk_sb_t *sb, storage_pool_raw_sb_t *raw) {
    if (!sb || !raw) return;
    uint8_t *sec = (uint8_t *)raw;
    memset(sec, 0, 512);
    raw->magic = sb->magic;
    raw->version = sb->version;
    if (sb->pool_uuid) memcpy(raw->pool_uuid, sb->pool_uuid, sb->pool_uuid_len < 16 ? sb->pool_uuid_len : 16);
    if (sb->local_member_uuid)
        memcpy(raw->local_member_uuid, sb->local_member_uuid,
               sb->local_member_uuid_len < 16 ? sb->local_member_uuid_len : 16);
    raw->member_index = sb->member_index;
    raw->member_count = (uint32_t)sb->members_count;
    raw->total_capacity_sectors = sb->total_capacity_sectors;
    raw->chunk_size_sectors = sb->chunk_size_sectors;
    raw->generation = sb->generation;
    raw->state = sb->state;

    /* Unlimited: write all members without arbitrary fixed slot cap */
    for (size_t i = 0; i < sb->members_count; i++) {
        storage_pool_disk_member_to_raw(&sb->members[i], &raw->members[i]);
    }

    *(uint32_t *)(sec + 452) = sb->recovery_count;
    if (sb->reserved && sb->reserved_len) {
        size_t r = sb->reserved_len < 52 ? sb->reserved_len : 52;
        memcpy(sec + 456, sb->reserved, r);
    }
    *(uint32_t *)(sec + 508) = sb->crc32;
}
