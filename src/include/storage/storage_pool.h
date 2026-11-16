#ifndef _STORAGE_POOL_H
#define _STORAGE_POOL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <driver/storage/block_dev.h>

#define STORAGE_POOL_MAGIC               0x504F4F4C /* 'POOL' */

/* Pool metadata lives in LBA 2030..2039 and extended members in 2040..2047
 * (MBR gap before FS_START_LBA = 2048). Kernel image starts at LBA 1. */
#define STORAGE_POOL_PRIMARY_SB_LBA      2030
#define STORAGE_POOL_JOURNAL_LBA         2031
#define STORAGE_POOL_JOURNAL_SECTORS     8
#define STORAGE_POOL_BACKUP_SB_LBA       2039
#define STORAGE_POOL_EXT_MEMBERS_LBA     2040
#define STORAGE_POOL_EXT_MEMBERS_SECTORS 7
#define STORAGE_POOL_BACKUP_EXT_LBA      2047

#define STORAGE_POOL_EXT_MAGIC           0x58544D42 /* 'XTMB' */
#define STORAGE_POOL_MEMBERS_PER_EXT_SEC 10

#define STORAGE_JOURNAL_ENTRY_MAGIC 0x54584E4C /* 'TXNL' */

typedef enum {
    STORAGE_POOL_STATE_ONLINE = 0,
    STORAGE_POOL_STATE_DEGRADED,
    STORAGE_POOL_STATE_CORRUPT
} storage_pool_state_t;

typedef enum {
    TX_OP_READ = 0,
    TX_OP_WRITE,
    TX_OP_FLUSH,
    TX_OP_CHECK
} tx_op_type_t;

typedef enum {
    TX_STATE_INIT = 0,
    TX_STATE_PENDING,
    TX_STATE_COMMITTED,
    TX_STATE_FAILED
} tx_state_t;

/* 48 B packed member descriptor */
typedef struct __attribute__((packed)) {
    uint8_t  uuid[16];
    uint8_t  member_index;
    uint8_t  flags;                   /* 0x01 = Online, 0x02 = Failed */
    char     bdev_name[14];           /* e.g. "ata0" */
    uint64_t start_pool_lba;
    uint64_t capacity_sectors;
} storage_pool_raw_member_t;

/* 512 B extended member sector (holds up to 10 members past the 8 inline superblock slots) */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t sector_index;
    uint32_t start_member_index;
    uint32_t member_count;             /* <= 10 */
    uint64_t next_lba;                 /* 0 if last */
    uint32_t reserved;
    storage_pool_raw_member_t members[10];
    uint32_t crc32;                    /* CRC32 of bytes 0..507 */
} storage_pool_ext_sector_t;

/* 52 B extended header stored in reserved[52] at offset 456 of the raw superblock */
typedef struct __attribute__((packed)) {
    uint32_t ext_magic;
    uint32_t total_members;
    uint32_t ext_lba;
    uint32_t ext_sectors;
    uint32_t backup_ext_lba;
    uint32_t reserved[8];
} storage_pool_ext_header_t;

typedef struct {
    uint8_t  *uuid;
    size_t   uuid_len;
    size_t   uuid_cap;
    uint8_t  member_index;
    uint8_t  flags;                   /* 0x01 = Online, 0x02 = Failed */
    char     *bdev_name;
    size_t   bdev_name_len;
    size_t   bdev_name_cap;
    uint64_t start_pool_lba;
    uint64_t capacity_sectors;
} storage_pool_disk_member_t;

void storage_pool_disk_member_init(storage_pool_disk_member_t *m);
void storage_pool_disk_member_free(storage_pool_disk_member_t *m);
int  storage_pool_disk_member_set_uuid(storage_pool_disk_member_t *m, const uint8_t *uuid, size_t len);
int  storage_pool_disk_member_set_bdev_name(storage_pool_disk_member_t *m, const char *name);
int  storage_pool_disk_member_copy(storage_pool_disk_member_t *dst, const storage_pool_disk_member_t *src);
void storage_pool_disk_member_from_raw(storage_pool_disk_member_t *m, const storage_pool_raw_member_t *raw);
void storage_pool_disk_member_to_raw(const storage_pool_disk_member_t *m, storage_pool_raw_member_t *raw);

/* 512 B packed superblock; inline members fit before recovery/reserved tail */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  pool_uuid[16];
    uint8_t  local_member_uuid[16];
    uint32_t member_index;
    uint32_t member_count;
    uint64_t total_capacity_sectors;
    uint32_t chunk_size_sectors;
    uint32_t generation;
    uint32_t state;                    /* storage_pool_state_t */
    storage_pool_raw_member_t members[];
} storage_pool_raw_sb_t;

typedef struct {
    uint32_t magic;
    uint8_t  *pool_uuid;
    size_t   pool_uuid_len;
    size_t   pool_uuid_cap;
    uint8_t  *local_member_uuid;
    size_t   local_member_uuid_len;
    size_t   local_member_uuid_cap;
    uint32_t member_index;
    uint32_t member_count;
    uint64_t total_capacity_sectors;
    uint32_t chunk_size_sectors;
    uint32_t generation;
    uint32_t state;
    storage_pool_disk_member_t *members;
    size_t   members_count;
    size_t   members_cap;
    uint32_t recovery_count;
    uint8_t  *reserved;
    size_t   reserved_len;
    size_t   reserved_cap;
    uint32_t crc32;
} storage_pool_disk_sb_t;

void storage_pool_disk_sb_init(storage_pool_disk_sb_t *sb);
void storage_pool_disk_sb_free(storage_pool_disk_sb_t *sb);
int  storage_pool_disk_sb_set_pool_uuid(storage_pool_disk_sb_t *sb, const uint8_t *uuid, size_t len);
int  storage_pool_disk_sb_set_local_member_uuid(storage_pool_disk_sb_t *sb, const uint8_t *uuid, size_t len);
int  storage_pool_disk_sb_set_reserved(storage_pool_disk_sb_t *sb, const uint8_t *data, size_t len);
int  storage_pool_disk_sb_add_member(storage_pool_disk_sb_t *sb, const storage_pool_disk_member_t *m);
void storage_pool_disk_sb_from_raw(storage_pool_disk_sb_t *sb, const storage_pool_raw_sb_t *raw);
void storage_pool_disk_sb_to_raw(const storage_pool_disk_sb_t *sb, storage_pool_raw_sb_t *raw);

/* 64 B WAL entry (8 per 512 B sector) */
typedef struct __attribute__((packed)) {
    uint32_t magic;                    /* 'TXNL' */
    uint32_t tx_id;
    uint16_t op;                       /* tx_op_type_t */
    uint16_t state;                    /* tx_state_t */
    uint64_t lba;
    uint32_t count;
    uint32_t timestamp;                /* ms tick */
    uint32_t data_crc;
    uint32_t entry_crc;                /* CRC32 of bytes 0..31 */
    uint8_t  reserved[28];
} storage_raw_disk_tx_entry_t;

typedef struct {
    uint32_t magic;
    uint32_t tx_id;
    uint16_t op;
    uint16_t state;
    uint64_t lba;
    uint32_t count;
    uint32_t timestamp;
    uint32_t data_crc;
    uint32_t entry_crc;
    uint8_t  *reserved;
    size_t   reserved_len;
    size_t   reserved_cap;
} storage_disk_tx_entry_t;

void storage_disk_tx_entry_init(storage_disk_tx_entry_t *e);
void storage_disk_tx_entry_free(storage_disk_tx_entry_t *e);
int  storage_disk_tx_entry_set_reserved(storage_disk_tx_entry_t *e, const uint8_t *data, size_t len);
void storage_disk_tx_entry_from_raw(storage_disk_tx_entry_t *e, const storage_raw_disk_tx_entry_t *raw);
void storage_disk_tx_entry_to_raw(const storage_disk_tx_entry_t *e, storage_raw_disk_tx_entry_t *raw);

typedef struct {
    uint32_t     tx_id;
    tx_op_type_t op;
    uint64_t     lba;
    uint16_t     count;
    uint32_t     timestamp;
    tx_state_t   state;
    uint32_t     data_crc;
    uint32_t     crc;
    uint8_t      *extra_data;
    size_t       extra_data_len;
    size_t       extra_data_cap;
} storage_tx_entry_t;

void storage_tx_entry_init(storage_tx_entry_t *e);
void storage_tx_entry_free(storage_tx_entry_t *e);

typedef struct {
    uint32_t           head;
    uint32_t           total_tx_count;
    uint32_t           active_tx_count;
    storage_tx_entry_t *entries;
    size_t             entries_count;
    size_t             entries_cap;
} storage_journal_t;

typedef struct {
    char            *bdev_name;
    size_t          bdev_name_len;
    size_t          bdev_name_cap;
    block_device_t  *bdev;
    uint64_t        start_pool_lba;
    uint64_t        capacity_sectors;
    uint8_t         *uuid;
    size_t          uuid_len;
    size_t          uuid_cap;
    bool            online;
} pool_member_t;

void pool_member_init(pool_member_t *m);
void pool_member_free(pool_member_t *m);
int  pool_member_set_name(pool_member_t *m, const char *name);
int  pool_member_set_uuid(pool_member_t *m, const uint8_t *uuid, size_t len);

typedef struct {
    uint32_t             magic;
    uint8_t              *pool_uuid;
    size_t               pool_uuid_len;
    size_t               pool_uuid_cap;
    uint32_t             member_count;
    uint32_t             member_capacity;
    uint64_t             total_capacity_sectors;
    uint32_t             chunk_size_sectors;
    uint32_t             generation;
    storage_pool_state_t state;
    uint32_t             recovery_count;
    pool_member_t        *members;
    storage_journal_t    journal;
    bool                 initialized;
    uint64_t             sb_lba;
    uint64_t             journal_lba;
    uint64_t             backup_sb_lba;
} storage_pool_t;

int  storage_pool_init(void);
bool storage_pool_is_initialized(void);
storage_pool_state_t storage_pool_get_state(void);
const char *storage_pool_get_state_str(void);
uint64_t storage_pool_get_capacity(void);
uint64_t storage_pool_get_used(void);
uint64_t storage_pool_get_free(void);
uint32_t storage_pool_get_member_count(void);

bool storage_pool_read(uint64_t pool_lba, uint16_t count, void *buf);
bool storage_pool_write(uint64_t pool_lba, uint16_t count, const void *buf);
bool storage_pool_write_raw(uint64_t pool_lba, uint16_t count, const void *buf);
bool storage_pool_flush(void);

int  storage_pool_add_device(block_device_t *bdev);
int  storage_pool_check(void);
void storage_pool_print_status(void);
void storage_pool_print_usage(void);

int  storage_pool_create(const char *pool_uuid);
int  storage_pool_create_ex(const char *pool_uuid, const char *bdev_name);
int  storage_pool_recover(void);
int  storage_pool_save_metadata(void);

#endif /* _STORAGE_POOL_H */
