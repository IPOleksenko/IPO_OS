#ifndef IPO_FS_H
#define IPO_FS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define IPO_FS_BLOCK_SIZE 512
#define IPO_FS_MAGIC_STR "IPO_FS"

#define IPO_INODE_EXTENTS 4
#define IPO_EXTENT_NODE_EXTENTS 20

#define IPO_INODE_TYPE_DIR 0x1
#define IPO_INODE_TYPE_FILE 0x2
#define IPO_INODE_FLAG_PROTECTED 0x80000000u

#define IPO_FS_FLAG_LINKED_EXTENTS 0x0001u
#define IPO_FS_FLAG_DYNAMIC_INODES 0x0002u

#define IPO_FS_OK               0
#define IPO_FS_ERR_NO_SPACE     1
#define IPO_FS_ERR_INODE_LIMIT  2
#define IPO_FS_ERR_CORRUPT      3
#define IPO_FS_ERR_NOT_MOUNTED  4
#define IPO_FS_ERR_IO           5
#define IPO_FS_ERR_INVALID_ARG  6

/* 32-byte descriptor tracking up to 64 contiguous dynamic inodes */
struct __attribute__((packed)) ipo_inode_chunk_desc {
    uint64_t start_inode;      /* 1-based */
    uint64_t table_block;
    uint64_t bitmap;           /* bit i = 1 when inode i is used */
    uint32_t inode_count;      /* typically 4..64 */
    uint32_t free_inodes;
};

#define IPO_CHUNK_NODE_CHUNKS 15

/* 512 B chained chunk node: 4 + 4 + 8 + 15*32 + 16 = 512 B */
struct __attribute__((packed)) ipo_inode_chunk_node {
    uint32_t count;            /* 0..15 active entries */
    uint32_t flags;
    uint64_t next_node;        /* 0 at end of chain */
    struct ipo_inode_chunk_desc chunks[IPO_CHUNK_NODE_CHUNKS];
    uint8_t  _pad[16];
};

/* 512 B root superblock at block 0 */
struct __attribute__((packed)) ipo_superblock {
    char magic[8];             /* "IPO_FS\0\0" */
    uint64_t fs_size_blocks;
    uint32_t block_size;        /* 512 B */
    uint32_t flags;             /* IPO_FS_FLAG_* */
    uint64_t inode_count;       /* primary + dynamic */
    uint64_t inode_bitmap_start;
    uint64_t block_bitmap_start;
    uint64_t inode_table_start;
    uint64_t data_blocks_start;

    uint32_t primary_inodes;
    uint32_t reserved;
    uint64_t dynamic_chunk_tree;/* first ipo_inode_chunk_node block */
    uint64_t dynamic_chunks_count;
    uint64_t dynamic_inodes_count;
    uint64_t dynamic_inodes_used;
    uint8_t  _pad[408];
};

struct __attribute__((packed)) ipo_extent {
    uint64_t logical_block;
    uint64_t physical_block;
    uint32_t block_count;
    uint32_t flags;
};

/* 128 B inode (4 per 512 B block) */
struct __attribute__((packed)) ipo_inode {
    uint32_t mode;            /* IPO_INODE_TYPE_* | IPO_INODE_FLAG_* */
    uint32_t links_count;
    uint64_t size;
    struct ipo_extent extents[IPO_INODE_EXTENTS];
    uint64_t next_extent_node;/* 0 if none */
    uint8_t  _pad[8];
};

/* 512 B chained extent node */
struct __attribute__((packed)) ipo_extent_node {
    uint32_t count;
    uint32_t flags;
    uint64_t next_node;       /* 0 if none */
    struct ipo_extent extents[IPO_EXTENT_NODE_EXTENTS];
    uint8_t  _pad[16];
};

/* 16 B header + name + padding; 56-bit inode splits across inode and reserved[0..2] */
struct __attribute__((packed)) ipo_dir_entry {
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

struct ipo_fd {
    int used;
    uint64_t inode;
    uint64_t offset;
    int flags;
};

extern struct ipo_superblock sb;
extern uint64_t fs_start_lba;
extern bool fs_mounted;
extern struct ipo_fd *fds;
extern uint32_t fds_capacity;

extern int ipo_fs_last_error;
void ipo_fs_set_last_error(int err);
int ipo_fs_get_last_error(void);
const char *ipo_fs_get_last_error_str(void);
bool ipo_fs_get_inode_stats(uint64_t *out_total, uint64_t *out_used, uint64_t *out_free, uint64_t *out_dynamic_chunks);
bool ipo_fs_get_block_stats(uint64_t *out_total, uint64_t *out_used, uint64_t *out_free);

bool block_read(uint64_t fs_block_index, void *buffer);
bool block_read_multi(uint64_t fs_block_index, uint16_t count, void *buffer);
bool block_write(uint64_t fs_block_index, const void *buffer);
bool block_write_multi(uint64_t fs_block_index, uint16_t count, const void *buffer);
int64_t allocate_block(void);
int64_t allocate_contiguous_blocks(uint32_t count);
bool free_block(uint64_t phys_block);

bool bitmap_get(uint64_t bitmap_start, uint64_t bit_index);
bool bitmap_set(uint64_t bitmap_start, uint64_t bit_index, bool value);
uint32_t bitmap_count_set(uint64_t bitmap_start, uint64_t total_bits);

bool read_inode(uint64_t inode_no, struct ipo_inode *out);
bool write_inode(uint64_t inode_no, const struct ipo_inode *in);
int64_t allocate_inode(void);
bool free_inode(uint64_t inode_no);
int64_t get_data_block_for_inode(struct ipo_inode *inode, uint64_t logical_index, bool alloc);
int64_t get_contiguous_data_blocks_for_inode(struct ipo_inode *inode, uint64_t logical_index, uint64_t *out_count);
int inode_read_bytes(struct ipo_inode *inode, void *buffer, uint32_t size, uint64_t offset);
int inode_write_bytes(uint64_t inode_no, struct ipo_inode *inode, const void *buffer, uint32_t size, uint64_t offset);
bool inode_truncate_blocks(uint64_t inode_no, struct ipo_inode *inode, uint64_t new_size);

int dir_find_entry(uint64_t dir_inode_no, const char *name, struct ipo_dir_entry *out_entry, uint64_t *out_offset);
bool is_valid_filename(const char *name);
bool dir_add_entry(uint64_t dir_inode_no, const char *name, uint64_t inode_no, uint8_t type);
bool dir_remove_entry(uint64_t dir_inode_no, const char *name);
/* Caller must release returned path with kfree */
char *fs_make_absolute_path(const char *path, const char *base);
bool fs_canonicalize(const char *in, char *out, size_t out_size);
const char *fs_path_find_last_sep(const char *path);
int path_resolve_parent_alloc(const char *path, uint64_t *out_parent_inode, char **out_name);
int path_resolve_parent(const char *path, uint64_t *out_parent_inode, char *out_name);
int path_resolve(const char *path, uint64_t *out_inode);

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
