#include <file_system/ipo_fs.h>
#include <string.h>
#include <stdio.h>

#define INODE_SIZE sizeof(struct ipo_inode)
#define INODES_PER_BLOCK (IPO_FS_BLOCK_SIZE / INODE_SIZE)

bool read_inode(uint32_t inode_no, struct ipo_inode *out) {
    if (inode_no == 0 || inode_no > sb.inode_count) return false;
    uint32_t idx = inode_no - 1; /* inodes are numbered from 1 */
    uint32_t block = sb.inode_table_start + (idx / INODES_PER_BLOCK);
    uint32_t offset = (idx % INODES_PER_BLOCK) * INODE_SIZE;
    uint8_t buf[IPO_FS_BLOCK_SIZE];
    if (!block_read(block, buf)) return false;
    memcpy(out, buf + offset, sizeof(*out));
    return true;
}

bool write_inode(uint32_t inode_no, const struct ipo_inode *in) {
    if (inode_no == 0 || inode_no > sb.inode_count) return false;
    uint32_t idx = inode_no - 1;
    uint32_t block = sb.inode_table_start + (idx / INODES_PER_BLOCK);
    uint32_t offset = (idx % INODES_PER_BLOCK) * INODE_SIZE;
    uint8_t buf[IPO_FS_BLOCK_SIZE];
    if (!block_read(block, buf)) return false;
    memcpy(buf + offset, in, sizeof(*in));
    return block_write(block, buf);
}

int allocate_inode(void) {
    /* Find a free bit in the inode bitmap */
    for (uint32_t i = 0; i < sb.inode_count; i++) {
        if (!bitmap_get(sb.inode_bitmap_start, i)) {
            if (!bitmap_set(sb.inode_bitmap_start, i, true)) return -1;
            /* zero the inode */
            struct ipo_inode zero;
            memset(&zero, 0, sizeof(zero));
            write_inode(i + 1, &zero);
            return i + 1;
        }
    }
    return -1; /* no free inodes */
}

bool free_inode(uint32_t inode_no) {
    if (inode_no == 0 || inode_no > sb.inode_count) return false;
    struct ipo_inode inode;
    if (!read_inode(inode_no, &inode)) return false;
    /* free all associated blocks */
    uint32_t nblocks = (inode.size + IPO_FS_BLOCK_SIZE - 1) / IPO_FS_BLOCK_SIZE;
    for (uint32_t i = 0; i < IPO_FS_DIRECT_BLOCKS && i < nblocks; i++) {
        if (inode.direct[i]) {
            uint32_t data_idx = inode.direct[i] - sb.data_blocks_start;
            bitmap_set(sb.block_bitmap_start, data_idx, false);
            inode.direct[i] = 0;
        }
    }
    /* single indirect */
    if (inode.indirect) {
        uint8_t ibuf[IPO_FS_BLOCK_SIZE];
        block_read(inode.indirect, ibuf);
        uint32_t *ptrs = (uint32_t *)ibuf;
        for (uint32_t i = 0; i < IPO_FS_BLOCK_SIZE / 4; i++) {
            if (ptrs[i]) {
                uint32_t data_idx = ptrs[i] - sb.data_blocks_start;
                bitmap_set(sb.block_bitmap_start, data_idx, false);
            }
        }
        /* free indirect block */
        bitmap_set(sb.block_bitmap_start, inode.indirect - sb.data_blocks_start, false);
        inode.indirect = 0;
    }
    /* clear inode bitmap */
    bitmap_set(sb.inode_bitmap_start, inode_no - 1, false);
    write_inode(inode_no, &inode);
    return true;
}

int allocate_block(void) {
    uint32_t data_blocks_total = sb.fs_size_blocks - sb.data_blocks_start;
    for (uint32_t i = 0; i < data_blocks_total; i++) {
        if (!bitmap_get(sb.block_bitmap_start, i)) {
            if (!bitmap_set(sb.block_bitmap_start, i, true)) { printf("allocate_block: bitmap_set failed at %u\n", i); return -1; }
            /* clear block */
            uint8_t zero[IPO_FS_BLOCK_SIZE];
            memset(zero, 0, sizeof(zero));
            if (!block_write(sb.data_blocks_start + i, zero)) { printf("allocate_block: block_write clear failed at %u\n", sb.data_blocks_start + i); return -1; }
            return sb.data_blocks_start + i; /* physical block index */
        }
    }
    printf("allocate_block: no free blocks\n");
    return -1; /* no space */
}

bool free_block(uint32_t phys_block) {
    if (phys_block < sb.data_blocks_start) return false;
    uint32_t i = phys_block - sb.data_blocks_start;
    return bitmap_set(sb.block_bitmap_start, i, false);
}

int get_data_block_for_inode(struct ipo_inode *inode, uint32_t logical_index, bool alloc) {
    if (logical_index < IPO_FS_DIRECT_BLOCKS) {
        if (!inode->direct[logical_index]) {
            if (!alloc) return -1;
            int phys = allocate_block();
            if (phys < 0) return -1;
            inode->direct[logical_index] = phys;
        }
        return inode->direct[logical_index];
    }
    /* single indirect */
    uint32_t idx = logical_index - IPO_FS_DIRECT_BLOCKS;
    uint32_t per_block = IPO_FS_BLOCK_SIZE / 4;
    if (idx >= per_block) return -1;
    uint8_t ibuf[IPO_FS_BLOCK_SIZE];
    if (!inode->indirect) {
        if (!alloc) return -1;
        int indirect_block = allocate_block();
        if (indirect_block < 0) return -1;
        inode->indirect = indirect_block;
        uint8_t zero[IPO_FS_BLOCK_SIZE]; memset(zero,0,sizeof(zero));
        block_write(indirect_block, zero);
    }
    if (!block_read(inode->indirect, ibuf)) return -1;
    uint32_t *ptrs = (uint32_t *)ibuf;
    if (!ptrs[idx]) {
        if (!alloc) return -1;
        int phys = allocate_block();
        if (phys < 0) return -1;
        ptrs[idx] = phys;
        block_write(inode->indirect, ibuf);
    }
    return ptrs[idx];
}
