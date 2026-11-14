#ifndef IPO_FS_H
#define IPO_FS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define IPO_FS_BLOCK_SIZE 512
#define IPO_FS_MAGIC_STR "IPO_FS"

#define IPO_INODE_EXTENTS 4
#define IPO_EXTENT_NODE_EXTENTS 20

/* inode types/flags */
#define IPO_INODE_TYPE_DIR 0x1
#define IPO_INODE_TYPE_FILE 0x2
#define IPO_INODE_FLAG_PROTECTED 0x80000000u

/* Superblock feature flags */
#define IPO_FS_FLAG_LINKED_EXTENTS 0x0001u
#define IPO_FS_FLAG_DYNAMIC_INODES 0x0002u

/* Filesystem diagnostic error codes */
#define IPO_FS_OK               0
#define IPO_FS_ERR_NO_SPACE     1  /* Volume full: no data blocks available for metadata or data */
#define IPO_FS_ERR_INODE_LIMIT  2  /* Reached maximum addressable inode capacity */
#define IPO_FS_ERR_CORRUPT      3  /* Filesystem structure corruption detected */
#define IPO_FS_ERR_NOT_MOUNTED  4  /* Filesystem is not mounted */
#define IPO_FS_ERR_IO           5  /* Disk I/O read/write error */
#define IPO_FS_ERR_INVALID_ARG  6  /* Invalid argument or file name */

/*
 * Dynamic Inode Chunk Descriptor:
 * Each chunk tracks a contiguous group of dynamically allocated inodes (up to 64 inodes).
 * Exactly 32 bytes.
 */
struct ipo_inode_chunk_desc {
    uint64_t start_inode;      /* 1-based start inode number */
    uint64_t table_block;      /* Physical starting block for this chunk's inodes */
    uint64_t bitmap;           /* Inode allocation bitmap: bit i = 1 if used (supports up to 64 inodes) */
    uint32_t inode_count;      /* Number of inodes in this chunk (typically 4..64) */
    uint32_t free_inodes;      /* Number of currently free inodes in this chunk */
};

#define IPO_CHUNK_NODE_CHUNKS 15

/*
 * Chained dynamic chunk node:
 * Fits in exactly 1 disk block (512 bytes):
 * 4 (count) + 4 (flags) + 8 (next_node) + 15 * 32 (chunks) + 16 (_pad) = 512 bytes.
 */
struct ipo_inode_chunk_node {
    uint32_t count;            /* Number of active chunk descriptors in this node (0..15) */
    uint32_t flags;            /* Reserved flags */
    uint64_t next_node;        /* Physical block of next chunk node (0 if end of chain) */
    struct ipo_inode_chunk_desc chunks[IPO_CHUNK_NODE_CHUNKS]; /* 480 bytes */
    uint8_t  _pad[16];         /* Pad to exactly 512 bytes */
};

/*
 * Root superblock:
 * Fixed 512 bytes (occupies block 0).
 */
struct ipo_superblock {
    char magic[8];             /* "IPO_FS\0\0" */
    uint64_t fs_size_blocks;    /* Total blocks in filesystem partition (64-bit block count) */
    uint32_t block_size;        /* 512 bytes */
    uint32_t flags;             /* Feature flags (1 = linked extents, 2 = dynamic inodes) */
    uint64_t inode_count;       /* Total inodes in filesystem (primary + dynamic) */
    uint64_t inode_bitmap_start;/* Block index of primary inode bitmap */
    uint64_t block_bitmap_start;/* Block index of block bitmap */
    uint64_t inode_table_start; /* Block index of primary inode table */
    uint64_t data_blocks_start; /* Block index of data blocks */

    uint32_t primary_inodes;    /* Inodes in the primary table */
    uint32_t reserved;          /* Reserved for future expansion / alignment */
    uint64_t dynamic_chunk_tree;/* Physical block of first ipo_inode_chunk_node */
    uint64_t dynamic_chunks_count; /* Total number of dynamic chunks allocated */
    uint64_t dynamic_inodes_count; /* Total inodes allocated in dynamic chunks */
    uint64_t dynamic_inodes_used;  /* Currently used dynamic inodes */
    uint8_t  _pad[408];         /* Pad to exactly 512 bytes */
};

struct ipo_extent {
    uint64_t logical_block;   /* Starting logical block in file */
    uint64_t physical_block;  /* Starting physical block in storage pool */
    uint32_t block_count;     /* Number of contiguous blocks */
    uint32_t flags;
};

struct ipo_inode {
    uint32_t mode;            /* Type and flags (IPO_INODE_TYPE_DIR, FILE, PROTECTED) */
    uint32_t links_count;     /* Hard links count */
    uint64_t size;            /* 64-bit file-size field; practical size is storage/API limited */
    struct ipo_extent extents[IPO_INODE_EXTENTS]; /* 4 embedded primary extents (96 bytes) */
    uint64_t next_extent_node;/* Physical block of next extent node (0 if none) */
    uint8_t  _pad[8];         /* Inode size = exactly 128 bytes (4 inodes / 512-byte block) */
};

struct ipo_extent_node {
    uint32_t count;           /* Number of active extents in this node */
    uint32_t flags;
    uint64_t next_node;       /* Physical block of next extent node (0 if none) */
    struct ipo_extent extents[IPO_EXTENT_NODE_EXTENTS]; /* 20 extents * 24 bytes = 480 bytes */
    uint8_t  _pad[16];        /* Exactly 512 bytes */
};

/*
 * Directory entry:
 * Exactly 16 bytes header + name bytes + padding.
 * Up to 56-bit inode number is encoded across inode and reserved[0..2].
 */
struct ipo_dir_entry {
    uint32_t inode;
    uint32_t rec_len;
    uint32_t name_len;
    uint8_t type;
    uint8_t reserved[3];
};

static inline uint64_t dir_entry_get_inode(const struct ipo_dir_entry *de) {
    uint64_t ino = (uint64_t)de->inode;
    uint64_t hi = (uint64_t)de->reserved[0] |
                  ((uint64_t)de->reserved[1] << 8) |
                  ((uint64_t)de->reserved[2] << 16);
    return ino | (hi << 32);
}

static inline void dir_entry_set_inode(struct ipo_dir_entry *de, uint64_t ino) {
    de->inode = (uint32_t)(ino & 0xFFFFFFFFu);
    de->reserved[0] = (uint8_t)((ino >> 32) & 0xFF);
    de->reserved[1] = (uint8_t)((ino >> 40) & 0xFF);
    de->reserved[2] = (uint8_t)((ino >> 48) & 0xFF);
}

/* file descriptor */
struct ipo_fd {
    int used;
    uint64_t inode;
    uint64_t offset;
    int flags;
};

/* Public state (defined in implementation) */
extern struct ipo_superblock sb;
extern uint64_t fs_start_lba;
extern bool fs_mounted;
extern struct ipo_fd *fds;
extern uint32_t fds_capacity;

/* Diagnostics API */
extern int ipo_fs_last_error;
void ipo_fs_set_last_error(int err);
int ipo_fs_get_last_error(void);
const char *ipo_fs_get_last_error_str(void);
bool ipo_fs_get_inode_stats(uint64_t *out_total, uint64_t *out_used, uint64_t *out_free, uint64_t *out_dynamic_chunks);
bool ipo_fs_get_block_stats(uint64_t *out_total, uint64_t *out_used, uint64_t *out_free);

/* Block layer */
bool block_read(uint64_t fs_block_index, void *buffer);
bool block_read_multi(uint64_t fs_block_index, uint16_t count, void *buffer);
bool block_write(uint64_t fs_block_index, const void *buffer);
bool block_write_multi(uint64_t fs_block_index, uint16_t count, const void *buffer);
int64_t allocate_block(void);
int64_t allocate_contiguous_blocks(uint32_t count);
bool free_block(uint64_t phys_block);

/* Bitmap API */
bool bitmap_get(uint64_t bitmap_start, uint64_t bit_index);
bool bitmap_set(uint64_t bitmap_start, uint64_t bit_index, bool value);
uint32_t bitmap_count_set(uint64_t bitmap_start, uint64_t total_bits);

/* Inode API */
bool read_inode(uint64_t inode_no, struct ipo_inode *out);
bool write_inode(uint64_t inode_no, const struct ipo_inode *in);
int64_t allocate_inode(void);
bool free_inode(uint64_t inode_no);
int64_t get_data_block_for_inode(struct ipo_inode *inode, uint64_t logical_index, bool alloc);
int64_t get_contiguous_data_blocks_for_inode(struct ipo_inode *inode, uint64_t logical_index, uint64_t *out_count);
int inode_read_bytes(struct ipo_inode *inode, void *buffer, uint32_t size, uint64_t offset);
int inode_write_bytes(uint64_t inode_no, struct ipo_inode *inode, const void *buffer, uint32_t size, uint64_t offset);
bool inode_truncate_blocks(uint64_t inode_no, struct ipo_inode *inode, uint64_t new_size);

/* Directory / path */
int dir_find_entry(uint64_t dir_inode_no, const char *name, struct ipo_dir_entry *out_entry, uint64_t *out_offset);
bool is_valid_filename(const char *name);
bool dir_add_entry(uint64_t dir_inode_no, const char *name, uint64_t inode_no, uint8_t type);
bool dir_remove_entry(uint64_t dir_inode_no, const char *name);
/* Returns a kmalloc-allocated absolute, canonical path; release it with kfree. */
char *fs_make_absolute_path(const char *path, const char *base);
bool fs_canonicalize(const char *in, char *out, size_t out_size);
int path_resolve_parent(const char *path, uint64_t *out_parent_inode, char *out_name);
int path_resolve(const char *path, uint64_t *out_inode);

/* Public FS API */
void ipo_fs_init(void);
void ipo_fs_reset_superblock(void);
void ipo_fs_fds_reset(void);
void ipo_fs_reset_state(void);
void ipo_fs_wipe_storage(void);
bool ipo_fs_install(void);
bool ipo_fs_format(uint64_t disk_start_lba, uint64_t total_blocks, uint64_t total_inodes);
bool ipo_fs_mount(uint64_t disk_start_lba);
int64_t ipo_fs_create(const char *path, uint8_t type);
int ipo_fs_open(const char *path);
int ipo_fs_close(int fd);
int ipo_fs_read(int fd, void *buffer, uint32_t size, uint32_t offset);
int ipo_fs_write(int fd, const void *buffer, uint32_t size, uint32_t offset);
bool ipo_fs_delete(const char *path);
bool ipo_fs_stat(const char *path, struct ipo_inode *out);
bool ipo_fs_write_text(const char *path, const char *text, bool append);
bool ipo_fs_rename(const char *oldpath, const char *newpath);
int ipo_fs_list_dir(const char *path, char *out, int out_size);

#endif /* IPO_FS_H */
