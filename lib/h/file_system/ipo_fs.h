#ifndef IPO_FS_H
#define IPO_FS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define IPO_FS_BLOCK_SIZE 512
#define IPO_FS_MAX_NAME 64
#define IPO_FS_DIRECT_BLOCKS 6
#define IPO_FS_MAGIC_STR "IPO_FS"

/* inode types/flags */
#define IPO_INODE_TYPE_DIR 0x1
#define IPO_INODE_TYPE_FILE 0x2
#define IPO_INODE_FLAG_PROTECTED 0x80000000u

#define IPO_MAX_FDS 32

struct ipo_superblock {
    char magic[8];
    uint32_t fs_size_blocks;
    uint32_t block_size;
    uint32_t inode_count;
    uint32_t inode_bitmap_start;
    uint32_t block_bitmap_start;
    uint32_t inode_table_start;
    uint32_t data_blocks_start;
};

struct ipo_inode {
    uint32_t mode; /* type and flags */
    uint32_t size;
    uint32_t links_count;
    uint32_t direct[IPO_FS_DIRECT_BLOCKS];
    uint32_t indirect;
    uint8_t  _pad[36]; /* padding to make inode reasonably sized */
};

struct ipo_dir_entry {
    uint32_t inode;
    uint8_t type;
    uint8_t name_len;
    uint8_t reserved[2];
    char name[IPO_FS_MAX_NAME];
};

/* file descriptor */
struct ipo_fd {
    int used;
    uint32_t inode;
    uint32_t offset;
    int flags;
};

/* Public state (defined in implementation) */
extern struct ipo_superblock sb;
extern uint32_t fs_start_lba;
extern bool fs_mounted;
extern struct ipo_fd fds[IPO_MAX_FDS];

/* Block layer */
bool block_read(uint32_t fs_block_index, void *buffer);
bool block_write(uint32_t fs_block_index, const void *buffer);

/* Bitmap API */
bool bitmap_get(uint32_t bitmap_start, uint32_t bit_index);
bool bitmap_set(uint32_t bitmap_start, uint32_t bit_index, bool value);

/* Inode API */
bool read_inode(uint32_t inode_no, struct ipo_inode *out);
bool write_inode(uint32_t inode_no, const struct ipo_inode *in);
int allocate_inode(void);
bool free_inode(uint32_t inode_no);
int allocate_block(void);
bool free_block(uint32_t phys_block);
int get_data_block_for_inode(struct ipo_inode *inode, uint32_t logical_index, bool alloc);

/* Directory / path */
int dir_find_entry(uint32_t dir_inode_no, const char *name, struct ipo_dir_entry *out_entry, uint32_t *out_block, uint32_t *out_block_off);
bool is_valid_filename(const char *name);
bool dir_add_entry(uint32_t dir_inode_no, const char *name, uint32_t inode_no, uint8_t type);
bool dir_remove_entry(uint32_t dir_inode_no, const char *name);
void fs_canonicalize(const char *in, char *out, size_t out_size);
int path_resolve_parent(const char *path, uint32_t *out_parent_inode, char *out_name);
int path_resolve(const char *path, uint32_t *out_inode);

/* Public FS API */
void ipo_fs_init(void);
bool ipo_fs_format(uint32_t disk_start_lba, uint32_t total_blocks, uint32_t total_inodes);
bool ipo_fs_mount(uint32_t disk_start_lba);
int ipo_fs_create(const char *path, uint8_t type);
int ipo_fs_open(const char *path);
int ipo_fs_read(int fd, void *buffer, uint32_t size, uint32_t offset);
int ipo_fs_write(int fd, const void *buffer, uint32_t size, uint32_t offset);
bool ipo_fs_delete(const char *path);
bool ipo_fs_stat(const char *path, struct ipo_inode *out);
bool ipo_fs_write_text(const char *path, const char *text, bool append);
bool ipo_fs_rename(const char *oldpath, const char *newpath);
int ipo_fs_list_dir(const char *path, char *out, int out_size);

#endif /* IPO_FS_H */
