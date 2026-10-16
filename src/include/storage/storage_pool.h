#ifndef _STORAGE_POOL_H
#define _STORAGE_POOL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <driver/storage/block_dev.h>

#define STORAGE_POOL_MAGIC          0x504F4F4C /* 'POOL' */
#define STORAGE_POOL_SB_VERSION     2

/* Safe primary locations in the MBR boot gap immediately preceding FS_START_LBA (2048).
 * This ensures storage pool metadata never collides with the kernel code (LBA 1..~1024)
 * when running from a single bootable Live USB drive. */
#define STORAGE_POOL_PRIMARY_SB_LBA      2030
#define STORAGE_POOL_JOURNAL_LBA         2031
#define STORAGE_POOL_JOURNAL_SECTORS     8
#define STORAGE_POOL_BACKUP_SB_LBA       2039

/* Default Primary Superblock LBA */
#define STORAGE_POOL_SB_LBA              STORAGE_POOL_PRIMARY_SB_LBA

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

/* Packed On-Disk Member Descriptor (48 bytes) */
typedef struct __attribute__((packed)) {
    uint8_t  uuid[16];                /* 16 bytes: Member UUID */
    uint8_t  member_index;            /* 1 byte: Index in pool */
    uint8_t  flags;                   /* 1 byte: Flags (0x01 = Online, 0x02 = Failed) */
    char     bdev_name[14];           /* 14 bytes: e.g. "ata0", "ata1" */
    uint64_t start_pool_lba;          /* 8 bytes: Start LBA offset in pool */
    uint64_t capacity_sectors;        /* 8 bytes: Member capacity in sectors */
} storage_pool_raw_member_t;

/* Dynamic In-Memory Member Descriptor (self-expanding, no fixed array limits) */
typedef struct {
    uint8_t  *uuid;                   /* Member UUID */
    size_t   uuid_len;
    size_t   uuid_cap;
    uint8_t  member_index;            /* Index in pool */
    uint8_t  flags;                   /* Flags (0x01 = Online, 0x02 = Failed) */
    char     *bdev_name;              /* Dynamic device name, e.g. "ata0", "ata1" */
    size_t   bdev_name_len;
    size_t   bdev_name_cap;
    uint64_t start_pool_lba;          /* Start LBA offset in pool */
    uint64_t capacity_sectors;        /* Member capacity in sectors */
} storage_pool_disk_member_t;

/* Dynamic storage_pool_disk_member_t lifecycle methods */
void storage_pool_disk_member_init(storage_pool_disk_member_t *m);
void storage_pool_disk_member_free(storage_pool_disk_member_t *m);
int  storage_pool_disk_member_set_uuid(storage_pool_disk_member_t *m, const uint8_t *uuid, size_t len);
int  storage_pool_disk_member_set_bdev_name(storage_pool_disk_member_t *m, const char *name);
int  storage_pool_disk_member_copy(storage_pool_disk_member_t *dst, const storage_pool_disk_member_t *src);
void storage_pool_disk_member_from_raw(storage_pool_disk_member_t *m, const storage_pool_raw_member_t *raw);
void storage_pool_disk_member_to_raw(const storage_pool_disk_member_t *m, storage_pool_raw_member_t *raw);

/* Packed On-Disk Raw Superblock (flexible on-disk member array, no fixed slot limit) */
typedef struct __attribute__((packed)) {
    uint32_t magic;                    /* 4 bytes: 0x504F4F4C */
    uint32_t version;                  /* 4 bytes: 2 */
    uint8_t  pool_uuid[16];            /* 16 bytes: Pool UUID */
    uint8_t  local_member_uuid[16];    /* 16 bytes: Local member UUID */
    uint32_t member_index;             /* 4 bytes: Local member index */
    uint32_t member_count;             /* 4 bytes: Configured members count */
    uint64_t total_capacity_sectors;   /* 8 bytes: Total capacity of pool */
    uint32_t chunk_size_sectors;       /* 4 bytes: Extent chunk size */
    uint32_t generation;               /* 4 bytes: Generation counter */
    uint32_t state;                    /* 4 bytes: storage_pool_state_t */
    storage_pool_raw_member_t members[]; /* Unlimited/dynamically sized on-disk member array */
} storage_pool_raw_sb_t;

/* Dynamic In-Memory Superblock Descriptor (self-expanding, no fixed array limits) */
typedef struct {
    uint32_t magic;
    uint32_t version;
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
    storage_pool_disk_member_t *members; /* Self-expanding array of member descriptors */
    size_t   members_count;
    size_t   members_cap;
    uint32_t recovery_count;
    uint8_t  *reserved;
    size_t   reserved_len;
    size_t   reserved_cap;
    uint32_t crc32;
} storage_pool_disk_sb_t;

/* Dynamic storage_pool_disk_sb_t lifecycle methods */
void storage_pool_disk_sb_init(storage_pool_disk_sb_t *sb);
void storage_pool_disk_sb_free(storage_pool_disk_sb_t *sb);
int  storage_pool_disk_sb_set_pool_uuid(storage_pool_disk_sb_t *sb, const uint8_t *uuid, size_t len);
int  storage_pool_disk_sb_set_local_member_uuid(storage_pool_disk_sb_t *sb, const uint8_t *uuid, size_t len);
int  storage_pool_disk_sb_set_reserved(storage_pool_disk_sb_t *sb, const uint8_t *data, size_t len);
int  storage_pool_disk_sb_add_member(storage_pool_disk_sb_t *sb, const storage_pool_disk_member_t *m);
void storage_pool_disk_sb_from_raw(storage_pool_disk_sb_t *sb, const storage_pool_raw_sb_t *raw);
void storage_pool_disk_sb_to_raw(const storage_pool_disk_sb_t *sb, storage_pool_raw_sb_t *raw);

/* Packed On-Disk 64-Byte Journal / WAL Entry (8 entries per 512-byte sector) */
typedef struct __attribute__((packed)) {
    uint32_t magic;                    /* 4 bytes: 0x54584E4C ('TXNL') */
    uint32_t tx_id;                    /* 4 bytes: Monotonic transaction ID */
    uint16_t op;                       /* 2 bytes: tx_op_type_t */
    uint16_t state;                    /* 2 bytes: tx_state_t */
    uint64_t lba;                      /* 8 bytes: Pool LBA target */
    uint32_t count;                    /* 4 bytes: Sector count */
    uint32_t timestamp;                /* 4 bytes: Milliseconds tick */
    uint32_t data_crc;                 /* 4 bytes: CRC32 of data written */
    uint32_t entry_crc;                /* 4 bytes: CRC32 of first 28 bytes */
    uint8_t  reserved[28];             /* 28 bytes: Padding */
} storage_raw_disk_tx_entry_t;

/* Dynamic In-Memory Disk Journal / WAL Entry (self-expanding, no fixed array limits) */
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

/* Dynamic storage_disk_tx_entry_t lifecycle methods */
void storage_disk_tx_entry_init(storage_disk_tx_entry_t *e);
void storage_disk_tx_entry_free(storage_disk_tx_entry_t *e);
int  storage_disk_tx_entry_set_reserved(storage_disk_tx_entry_t *e, const uint8_t *data, size_t len);
void storage_disk_tx_entry_from_raw(storage_disk_tx_entry_t *e, const storage_raw_disk_tx_entry_t *raw);
void storage_disk_tx_entry_to_raw(const storage_disk_tx_entry_t *e, storage_raw_disk_tx_entry_t *raw);

/* Dynamic In-Memory Transaction Entry (self-expanding, no fixed array limits) */
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

/* Dynamic storage_tx_entry_t lifecycle methods */
void storage_tx_entry_init(storage_tx_entry_t *e);
void storage_tx_entry_free(storage_tx_entry_t *e);
int  storage_tx_entry_set_extra_data(storage_tx_entry_t *e, const uint8_t *data, size_t len);
int  storage_tx_entry_copy(storage_tx_entry_t *dst, const storage_tx_entry_t *src);

/* In-Memory Journal Structure (self-expanding, no fixed array limits) */
typedef struct {
    uint32_t           head;
    uint32_t           tail;
    uint32_t           total_tx_count;
    uint32_t           active_tx_count;
    storage_tx_entry_t *entries;
    size_t             entries_count;
    size_t             entries_cap;
} storage_journal_t;

/* In-Memory Pool Member (self-expanding, no fixed array limits) */
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

/* In-Memory Storage Pool Runtime State (self-expanding, no fixed array limits) */
typedef struct {
    uint32_t             magic;
    uint32_t             version;
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

/* Storage Pool Public API */
int  storage_pool_init(void);
bool storage_pool_is_initialized(void);
storage_pool_state_t storage_pool_get_state(void);
const char *storage_pool_get_state_str(void);
uint64_t storage_pool_get_capacity(void);
uint64_t storage_pool_get_used(void);
uint64_t storage_pool_get_free(void);
uint32_t storage_pool_get_member_count(void);
const storage_pool_t *storage_pool_get_info(void);

bool storage_pool_read(uint64_t pool_lba, uint16_t count, void *buf);
bool storage_pool_write(uint64_t pool_lba, uint16_t count, const void *buf);
bool storage_pool_flush(void);

int  storage_pool_add_device(block_device_t *bdev);
int  storage_pool_check(void);
void storage_pool_print_status(void);
void storage_pool_print_usage(void);

int  storage_pool_create(const char *pool_uuid);
int  storage_pool_create_ex(const char *pool_uuid, const char *bdev_name);
int  storage_pool_recover(void);
int  storage_pool_save_metadata(void);
int  storage_pool_load_metadata(void);

#endif /* _STORAGE_POOL_H */

