#include <file_system/ipo_fs.h>
#include <string.h>
#include <stdio.h>

#define INODE_SIZE sizeof(struct ipo_inode)
#define INODES_PER_BLOCK (IPO_FS_BLOCK_SIZE / INODE_SIZE)

bool read_inode(uint32_t inode_no, struct ipo_inode *out) {
    if (inode_no == 0 || (uint64_t)inode_no > sb.inode_count) return false;
    uint32_t idx = inode_no - 1; /* inodes are numbered from 1 */
    uint64_t block = sb.inode_table_start + (idx / INODES_PER_BLOCK);
    uint32_t offset = (idx % INODES_PER_BLOCK) * INODE_SIZE;
    uint8_t buf[IPO_FS_BLOCK_SIZE];
    if (!block_read(block, buf)) return false;
    memcpy(out, buf + offset, sizeof(*out));
    return true;
}

bool write_inode(uint32_t inode_no, const struct ipo_inode *in) {
    if (inode_no == 0 || (uint64_t)inode_no > sb.inode_count) return false;
    uint32_t idx = inode_no - 1;
    uint64_t block = sb.inode_table_start + (idx / INODES_PER_BLOCK);
    uint32_t offset = (idx % INODES_PER_BLOCK) * INODE_SIZE;
    uint8_t buf[IPO_FS_BLOCK_SIZE];
    if (!block_read(block, buf)) return false;
    memcpy(buf + offset, in, sizeof(*in));
    return block_write(block, buf);
}

int allocate_inode(void) {
    /* Find a free bit in the inode bitmap */
    for (uint64_t i = 0; i < sb.inode_count; i++) {
        if (!bitmap_get(sb.inode_bitmap_start, i)) {
            if (!bitmap_set(sb.inode_bitmap_start, i, true)) return -1;
            /* zero the inode */
            struct ipo_inode zero;
            memset(&zero, 0, sizeof(zero));
            write_inode((uint32_t)(i + 1), &zero);
            return (int)(i + 1);
        }
    }
    return -1; /* no free inodes */
}

static void free_extent_blocks(const struct ipo_extent *ext) {
    if (ext->block_count == 0 || ext->physical_block == 0) return;
    for (uint32_t b = 0; b < ext->block_count; b++) {
        free_block(ext->physical_block + b);
    }
}

bool free_inode(uint32_t inode_no) {
    if (inode_no == 0 || (uint64_t)inode_no > sb.inode_count) return false;
    struct ipo_inode inode;
    if (!read_inode(inode_no, &inode)) return false;

    /* Free primary embedded extents */
    for (int i = 0; i < IPO_INODE_EXTENTS; i++) {
        free_extent_blocks(&inode.extents[i]);
    }

    /* Free chained extent nodes */
    uint64_t curr_node_blk = inode.next_extent_node;
    while (curr_node_blk != 0) {
        struct ipo_extent_node enode;
        if (!block_read(curr_node_blk, &enode)) break;
        for (uint32_t i = 0; i < enode.count && i < IPO_EXTENT_NODE_EXTENTS; i++) {
            free_extent_blocks(&enode.extents[i]);
        }
        uint64_t next_blk = enode.next_node;
        free_block(curr_node_blk);
        curr_node_blk = next_blk;
    }

    /* Clear inode bitmap */
    bitmap_set(sb.inode_bitmap_start, inode_no - 1, false);
    memset(&inode, 0, sizeof(inode));
    write_inode(inode_no, &inode);
    return true;
}

/* High-speed block allocator with word-skipping */
int64_t allocate_block(void) {
    if (sb.fs_size_blocks <= sb.data_blocks_start) return -1;
    uint64_t data_blocks_total = sb.fs_size_blocks - sb.data_blocks_start;
    uint64_t bitmap_blocks = (data_blocks_total + (IPO_FS_BLOCK_SIZE * 8) - 1) / (IPO_FS_BLOCK_SIZE * 8);

    uint8_t buf[IPO_FS_BLOCK_SIZE];

    for (uint64_t b = 0; b < bitmap_blocks; b++) {
        uint64_t lba = sb.block_bitmap_start + b;
        if (!block_read(lba, buf)) continue;

        uint32_t *words = (uint32_t *)buf;
        int num_words = IPO_FS_BLOCK_SIZE / sizeof(uint32_t);

        for (int w = 0; w < num_words; w++) {
            if (words[w] != 0xFFFFFFFFu) {
                /* Found word with at least one free bit */
                for (int bit = 0; bit < 32; bit++) {
                    if ((words[w] & (1u << bit)) == 0) {
                        uint64_t bit_idx = (b * IPO_FS_BLOCK_SIZE * 8) + (w * 32) + bit;
                        if (bit_idx >= data_blocks_total) return -1;

                        /* Mark bit as allocated */
                        words[w] |= (1u << bit);
                        if (!block_write(lba, buf)) return -1;

                        uint64_t phys = sb.data_blocks_start + bit_idx;
                        /* Clear allocated block */
                        uint8_t zero[IPO_FS_BLOCK_SIZE];
                        memset(zero, 0, sizeof(zero));
                        block_write(phys, zero);
                        return (int64_t)phys;
                    }
                }
            }
        }
    }

    return -1; /* No free blocks */
}

bool free_block(uint64_t phys_block) {
    if (phys_block < sb.data_blocks_start) return false;
    uint64_t i = phys_block - sb.data_blocks_start;
    return bitmap_set(sb.block_bitmap_start, i, false);
}

/* Find physical block for logical block index in Linked Extents (infinite file growth) */
int64_t get_data_block_for_inode(struct ipo_inode *inode, uint64_t logical_index, bool alloc) {
    /* 1. Search embedded primary extents */
    for (int i = 0; i < IPO_INODE_EXTENTS; i++) {
        struct ipo_extent *e = &inode->extents[i];
        if (e->block_count > 0) {
            if (logical_index >= e->logical_block && logical_index < e->logical_block + e->block_count) {
                return (int64_t)(e->physical_block + (logical_index - e->logical_block));
            }
        }
    }

    /* 2. Search chained extent nodes */
    uint64_t curr_node_blk = inode->next_extent_node;
    uint64_t last_node_blk = 0;
    struct ipo_extent_node last_enode;
    memset(&last_enode, 0, sizeof(last_enode));

    while (curr_node_blk != 0) {
        struct ipo_extent_node enode;
        if (!block_read(curr_node_blk, &enode)) break;

        for (uint32_t i = 0; i < enode.count && i < IPO_EXTENT_NODE_EXTENTS; i++) {
            struct ipo_extent *e = &enode.extents[i];
            if (e->block_count > 0) {
                if (logical_index >= e->logical_block && logical_index < e->logical_block + e->block_count) {
                    return (int64_t)(e->physical_block + (logical_index - e->logical_block));
                }
            }
        }

        last_node_blk = curr_node_blk;
        memcpy(&last_enode, &enode, sizeof(enode));
        curr_node_blk = enode.next_node;
    }

    if (!alloc) return -1;

    /* 3. Allocate a new physical block */
    int64_t new_phys = allocate_block();
    if (new_phys < 0) return -1;

    /* Check if we can extend the last primary extent */
    for (int i = 0; i < IPO_INODE_EXTENTS; i++) {
        struct ipo_extent *e = &inode->extents[i];
        if (e->block_count > 0 &&
            e->logical_block + e->block_count == logical_index &&
            e->physical_block + e->block_count == (uint64_t)new_phys) {
            e->block_count++;
            return new_phys;
        }
    }

    /* Check if there is an empty slot in primary extents */
    for (int i = 0; i < IPO_INODE_EXTENTS; i++) {
        struct ipo_extent *e = &inode->extents[i];
        if (e->block_count == 0) {
            e->logical_block = logical_index;
            e->physical_block = (uint64_t)new_phys;
            e->block_count = 1;
            e->flags = 0;
            return new_phys;
        }
    }

    /* If last chained node can extend its last extent */
    if (last_node_blk != 0 && last_enode.count > 0) {
        struct ipo_extent *last_e = &last_enode.extents[last_enode.count - 1];
        if (last_e->logical_block + last_e->block_count == logical_index &&
            last_e->physical_block + last_e->block_count == (uint64_t)new_phys) {
            last_e->block_count++;
            block_write(last_node_blk, &last_enode);
            return new_phys;
        }

        /* If last node has space for a new extent */
        if (last_enode.count < IPO_EXTENT_NODE_EXTENTS) {
            struct ipo_extent *new_e = &last_enode.extents[last_enode.count++];
            new_e->logical_block = logical_index;
            new_e->physical_block = (uint64_t)new_phys;
            new_e->block_count = 1;
            new_e->flags = 0;
            block_write(last_node_blk, &last_enode);
            return new_phys;
        }
    }

    /* Need a new chained extent node block */
    int64_t new_node_blk = allocate_block();
    if (new_node_blk < 0) {
        free_block((uint64_t)new_phys);
        return -1;
    }

    struct ipo_extent_node new_enode;
    memset(&new_enode, 0, sizeof(new_enode));
    new_enode.count = 1;
    new_enode.next_node = 0;
    new_enode.extents[0].logical_block = logical_index;
    new_enode.extents[0].physical_block = (uint64_t)new_phys;
    new_enode.extents[0].block_count = 1;
    new_enode.extents[0].flags = 0;
    block_write((uint64_t)new_node_blk, &new_enode);

    if (last_node_blk != 0) {
        last_enode.next_node = (uint64_t)new_node_blk;
        block_write(last_node_blk, &last_enode);
    } else {
        inode->next_extent_node = (uint64_t)new_node_blk;
    }

    return new_phys;
}
