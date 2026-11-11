#include <file_system/ipo_fs.h>
#include <string.h>
#include <stdio.h>

#define INODE_SIZE sizeof(struct ipo_inode)
#define INODES_PER_BLOCK (IPO_FS_BLOCK_SIZE / INODE_SIZE)

extern struct ipo_inode_chunk_desc cached_chunk;
extern bool cached_chunk_valid;

static bool find_dynamic_inode_location(uint64_t inode_no, uint64_t *out_block, uint32_t *out_offset) {
    if (sb.dynamic_chunk_tree == 0) return false;

    if (cached_chunk_valid &&
        inode_no >= cached_chunk.start_inode &&
        inode_no < cached_chunk.start_inode + cached_chunk.inode_count) {
        uint64_t idx = inode_no - cached_chunk.start_inode;
        *out_block = cached_chunk.table_block + (idx / INODES_PER_BLOCK);
        *out_offset = (uint32_t)((idx % INODES_PER_BLOCK) * INODE_SIZE);
        return true;
    }

    uint64_t curr_node_blk = sb.dynamic_chunk_tree;
    while (curr_node_blk != 0) {
        struct ipo_inode_chunk_node node;
        if (!block_read(curr_node_blk, &node)) return false;

        for (uint32_t i = 0; i < node.count && i < IPO_CHUNK_NODE_CHUNKS; i++) {
            struct ipo_inode_chunk_desc *c = &node.chunks[i];
            if (inode_no >= c->start_inode && inode_no < c->start_inode + c->inode_count) {
                cached_chunk = *c;
                cached_chunk_valid = true;
                uint64_t idx = inode_no - c->start_inode;
                *out_block = c->table_block + (idx / INODES_PER_BLOCK);
                *out_offset = (uint32_t)((idx % INODES_PER_BLOCK) * INODE_SIZE);
                return true;
            }
        }
        curr_node_blk = node.next_node;
    }
    return false;
}

bool read_inode(uint64_t inode_no, struct ipo_inode *out) {
    if (inode_no == 0 || inode_no > sb.inode_count || !out) {
        return false;
    }

    uint64_t block = 0;
    uint32_t offset = 0;

    if (inode_no <= sb.primary_inodes) {
        uint64_t idx = inode_no - 1; /* 1-based */
        block = sb.inode_table_start + (idx / INODES_PER_BLOCK);
        offset = (uint32_t)((idx % INODES_PER_BLOCK) * INODE_SIZE);
    } else {
        if (!find_dynamic_inode_location(inode_no, &block, &offset)) {
            serial_printf("[ipo_fs] read_inode(%llu) failed: dynamic inode not found in chunks\n", inode_no);
            return false;
        }
    }

    uint8_t buf[IPO_FS_BLOCK_SIZE];
    if (!block_read(block, buf)) {
        serial_printf("[ipo_fs] read_inode(%llu) failed: block_read(%llu) failed\n", inode_no, block);
        return false;
    }
    memcpy(out, buf + offset, sizeof(*out));
    return true;
}

bool write_inode(uint64_t inode_no, const struct ipo_inode *in) {
    if (inode_no == 0 || inode_no > sb.inode_count || !in) {
        return false;
    }

    uint64_t block = 0;
    uint32_t offset = 0;

    if (inode_no <= sb.primary_inodes) {
        uint64_t idx = inode_no - 1;
        block = sb.inode_table_start + (idx / INODES_PER_BLOCK);
        offset = (uint32_t)((idx % INODES_PER_BLOCK) * INODE_SIZE);
    } else {
        if (!find_dynamic_inode_location(inode_no, &block, &offset)) {
            return false;
        }
    }

    uint8_t buf[IPO_FS_BLOCK_SIZE];
    if (!block_read(block, buf)) return false;
    memcpy(buf + offset, in, sizeof(*in));
    return block_write(block, buf);
}

int64_t allocate_contiguous_blocks(uint32_t count) {
    if (count == 0 || sb.fs_size_blocks <= sb.data_blocks_start) {
        ipo_fs_set_last_error(IPO_FS_ERR_NO_SPACE);
        return -1;
    }
    uint64_t data_blocks_total = sb.fs_size_blocks - sb.data_blocks_start;
    if (count > data_blocks_total) {
        ipo_fs_set_last_error(IPO_FS_ERR_NO_SPACE);
        return -1;
    }

    uint64_t bitmap_blocks = (data_blocks_total + (IPO_FS_BLOCK_SIZE * 8) - 1) / (IPO_FS_BLOCK_SIZE * 8);
    uint8_t buf[IPO_FS_BLOCK_SIZE];

    uint64_t current_streak = 0;
    uint64_t streak_start_bit = 0;

    for (uint64_t b = 0; b < bitmap_blocks; b++) {
        uint64_t lba = sb.block_bitmap_start + b;
        if (!block_read(lba, buf)) {
            current_streak = 0;
            continue;
        }

        uint32_t *words = (uint32_t *)buf;
        int num_words = IPO_FS_BLOCK_SIZE / sizeof(uint32_t);

        for (int w = 0; w < num_words; w++) {
            uint32_t word = words[w];
            if (word == 0xFFFFFFFFu) {
                current_streak = 0;
                continue;
            }
            for (int bit = 0; bit < 32; bit++) {
                uint64_t bit_idx = (b * IPO_FS_BLOCK_SIZE * 8) + (w * 32) + bit;
                if (bit_idx >= data_blocks_total) {
                    ipo_fs_set_last_error(IPO_FS_ERR_NO_SPACE);
                    return -1;
                }
                if ((word & (1u << bit)) == 0) {
                    if (current_streak == 0) {
                        streak_start_bit = bit_idx;
                    }
                    current_streak++;
                    if (current_streak == count) {
                        /* Mark bits as allocated */
                        for (uint64_t i = 0; i < count; i++) {
                            if (!bitmap_set(sb.block_bitmap_start, streak_start_bit + i, true)) {
                                for (uint64_t r = 0; r < i; r++) {
                                    bitmap_set(sb.block_bitmap_start, streak_start_bit + r, false);
                                }
                                ipo_fs_set_last_error(IPO_FS_ERR_IO);
                                return -1;
                            }
                        }
                        /* Zero allocated blocks */
                        uint64_t phys_start = sb.data_blocks_start + streak_start_bit;
                        static uint8_t zero_block[IPO_FS_BLOCK_SIZE];
                        memset(zero_block, 0, sizeof(zero_block));
                        for (uint64_t i = 0; i < count; i++) {
                            if (!block_write(phys_start + i, zero_block)) {
                                for (uint64_t r = 0; r < count; r++) {
                                    bitmap_set(sb.block_bitmap_start, streak_start_bit + r, false);
                                }
                                ipo_fs_set_last_error(IPO_FS_ERR_IO);
                                return -1;
                            }
                        }
                        return (int64_t)phys_start;
                    }
                } else {
                    current_streak = 0;
                }
            }
        }
    }
    ipo_fs_set_last_error(IPO_FS_ERR_NO_SPACE);
    return -1;
}

int64_t allocate_block(void) {
    return allocate_contiguous_blocks(1);
}

bool free_block(uint64_t phys_block) {
    if (phys_block < sb.data_blocks_start || phys_block >= sb.fs_size_blocks) {
        return false;
    }
    uint64_t i = phys_block - sb.data_blocks_start;
    return bitmap_set(sb.block_bitmap_start, i, false);
}

static int64_t expand_and_allocate_inode(void) {
    uint64_t target_node_blk = 0;
    struct ipo_inode_chunk_node target_node;
    memset(&target_node, 0, sizeof(target_node));

    uint64_t last_chunk_end_ino = (sb.primary_inodes > 0) ? sb.primary_inodes : 0;
    bool created_new_node = false;

    if (sb.dynamic_chunk_tree == 0) {
        /* Allocate first chunk node */
        int64_t blk = allocate_block();
        if (blk < 0) {
            ipo_fs_set_last_error(IPO_FS_ERR_NO_SPACE);
            serial_printf("[ipo_fs] Inode expansion FAILED: cannot allocate chunk node (volume full)\n");
            return -1;
        }
        target_node_blk = (uint64_t)blk;
        target_node.count = 0;
        target_node.flags = 0;
        target_node.next_node = 0;
        created_new_node = true;
    } else {
        uint64_t curr = sb.dynamic_chunk_tree;
        uint64_t prev = 0;
        while (curr != 0) {
            if (!block_read(curr, &target_node)) {
                ipo_fs_set_last_error(IPO_FS_ERR_IO);
                return -1;
            }
            if (target_node.count > 0) {
                struct ipo_inode_chunk_desc *last_c = &target_node.chunks[target_node.count - 1];
                last_chunk_end_ino = last_c->start_inode + last_c->inode_count - 1;
            }
            prev = curr;
            curr = target_node.next_node;
        }
        if (target_node.count < IPO_CHUNK_NODE_CHUNKS) {
            target_node_blk = prev;
        } else {
            /* Last chunk node is full, allocate new chained node */
            int64_t blk = allocate_block();
            if (blk < 0) {
                ipo_fs_set_last_error(IPO_FS_ERR_NO_SPACE);
                serial_printf("[ipo_fs] Inode expansion FAILED: cannot allocate chained chunk node (volume full)\n");
                return -1;
            }
            uint64_t new_node_blk = (uint64_t)blk;
            struct ipo_inode_chunk_node new_node;
            memset(&new_node, 0, sizeof(new_node));
            new_node.count = 0;
            new_node.flags = 0;
            new_node.next_node = 0;
            if (!block_write(new_node_blk, &new_node)) {
                free_block(new_node_blk);
                ipo_fs_set_last_error(IPO_FS_ERR_IO);
                return -1;
            }
            target_node.next_node = new_node_blk;
            if (!block_write(prev, &target_node)) {
                free_block(new_node_blk);
                ipo_fs_set_last_error(IPO_FS_ERR_IO);
                return -1;
            }
            target_node_blk = new_node_blk;
            target_node = new_node;
            created_new_node = true;
        }
    }

    /* Allocate blocks for the chunk table: try 16, 8, 4, 2, 1 blocks */
    uint32_t blocks_to_alloc = 16; /* 64 inodes */
    int64_t table_phys = -1;
    while (blocks_to_alloc >= 1) {
        table_phys = allocate_contiguous_blocks(blocks_to_alloc);
        if (table_phys >= 0) break;
        blocks_to_alloc /= 2;
    }

    if (table_phys < 0) {
        /* No space for inode table blocks: rollback new node block if allocated */
        if (created_new_node) {
            free_block(target_node_blk);
        }
        ipo_fs_set_last_error(IPO_FS_ERR_NO_SPACE);
        serial_printf("[ipo_fs] Inode expansion FAILED: no data blocks available for inode table\n");
        return -1;
    }

    uint32_t chunk_inodes = blocks_to_alloc * (uint32_t)INODES_PER_BLOCK;
    uint64_t start_inode = last_chunk_end_ino + 1;

    struct ipo_inode_chunk_desc *desc = &target_node.chunks[target_node.count];
    desc->start_inode = start_inode;
    desc->table_block = (uint64_t)table_phys;
    desc->inode_count = chunk_inodes;
    desc->free_inodes = chunk_inodes - 1; /* first inode allocated immediately */
    desc->bitmap = 1ULL;

    target_node.count++;

    if (!block_write(target_node_blk, &target_node)) {
        for (uint32_t b = 0; b < blocks_to_alloc; b++) free_block((uint64_t)table_phys + b);
        if (created_new_node) free_block(target_node_blk);
        ipo_fs_set_last_error(IPO_FS_ERR_IO);
        return -1;
    }

    if (sb.dynamic_chunk_tree == 0) {
        sb.dynamic_chunk_tree = target_node_blk;
    }
    sb.dynamic_chunks_count++;
    sb.dynamic_inodes_count += chunk_inodes;
    sb.dynamic_inodes_used++;
    sb.inode_count = sb.primary_inodes + sb.dynamic_inodes_count;

    if (!block_write(0, &sb)) {
        ipo_fs_set_last_error(IPO_FS_ERR_IO);
        return -1;
    }

    cached_chunk = *desc;
    cached_chunk_valid = true;

    /* Zero the first inode */
    struct ipo_inode zero;
    memset(&zero, 0, sizeof(zero));
    write_inode(start_inode, &zero);

    serial_printf("[ipo_fs] Inode expansion SUCCESS: added chunk (%u inodes, %u blocks, start_ino=%llu, total_inodes=%llu)\n",
                  chunk_inodes, blocks_to_alloc, start_inode, sb.inode_count);

    ipo_fs_set_last_error(IPO_FS_OK);
    return (int64_t)start_inode;
}

int64_t allocate_inode(void) {
    if (sb.inode_count == 0) {
        ipo_fs_set_last_error(IPO_FS_ERR_NOT_MOUNTED);
        return -1;
    }

    /* 1. Try primary inode bitmap */
    if (sb.primary_inodes > 0) {
        uint64_t bitmap_blocks = (sb.primary_inodes + (IPO_FS_BLOCK_SIZE * 8) - 1) / (IPO_FS_BLOCK_SIZE * 8);
        uint8_t buf[IPO_FS_BLOCK_SIZE];

        for (uint64_t b = 0; b < bitmap_blocks; b++) {
            uint64_t lba = sb.inode_bitmap_start + b;
            if (!block_read(lba, buf)) continue;

            uint32_t *words = (uint32_t *)buf;
            int num_words = IPO_FS_BLOCK_SIZE / sizeof(uint32_t);

            for (int w = 0; w < num_words; w++) {
                if (words[w] != 0xFFFFFFFFu) {
                    for (int bit = 0; bit < 32; bit++) {
                        if ((words[w] & (1u << bit)) == 0) {
                            uint64_t bit_idx = (b * IPO_FS_BLOCK_SIZE * 8) + (w * 32) + bit;
                            if (bit_idx < sb.primary_inodes) {
                                words[w] |= (1u << bit);
                                if (!block_write(lba, buf)) {
                                    ipo_fs_set_last_error(IPO_FS_ERR_IO);
                                    return -1;
                                }

                                uint64_t ino = bit_idx + 1;
                                struct ipo_inode zero;
                                memset(&zero, 0, sizeof(zero));
                                write_inode(ino, &zero);
                                ipo_fs_set_last_error(IPO_FS_OK);
                                return (int64_t)ino;
                            }
                        }
                    }
                }
            }
        }
    }

    /* 2. Try existing dynamic chunks */
    if (sb.dynamic_chunk_tree != 0) {
        uint64_t curr_node_blk = sb.dynamic_chunk_tree;
        while (curr_node_blk != 0) {
            struct ipo_inode_chunk_node node;
            if (!block_read(curr_node_blk, &node)) break;

            for (uint32_t i = 0; i < node.count && i < IPO_CHUNK_NODE_CHUNKS; i++) {
                struct ipo_inode_chunk_desc *c = &node.chunks[i];
                if (c->free_inodes > 0) {
                    for (uint32_t bit = 0; bit < c->inode_count; bit++) {
                        if ((c->bitmap & (1ULL << bit)) == 0) {
                            c->bitmap |= (1ULL << bit);
                            c->free_inodes--;
                            block_write(curr_node_blk, &node);
                            if (cached_chunk_valid && cached_chunk.start_inode == c->start_inode) {
                                cached_chunk = *c;
                            }
                            sb.dynamic_inodes_used++;
                            block_write(0, &sb);

                            uint64_t ino = c->start_inode + bit;
                            struct ipo_inode zero;
                            memset(&zero, 0, sizeof(zero));
                            write_inode(ino, &zero);
                            ipo_fs_set_last_error(IPO_FS_OK);
                            return (int64_t)ino;
                        }
                    }
                }
            }
            curr_node_blk = node.next_node;
        }
    }

    /* 3. Primary and existing chunks full: dynamically expand! */
    return expand_and_allocate_inode();
}

static void free_extent_blocks(const struct ipo_extent *ext) {
    if (ext->block_count == 0 || ext->physical_block == 0) return;
    for (uint32_t b = 0; b < ext->block_count; b++) {
        free_block(ext->physical_block + b);
    }
}

bool free_inode(uint64_t inode_no) {
    if (inode_no == 0 || inode_no > sb.inode_count) return false;
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

    /* Zero inode content */
    memset(&inode, 0, sizeof(inode));
    write_inode(inode_no, &inode);

    /* Clear bit in appropriate bitmap */
    if (inode_no <= sb.primary_inodes) {
        bitmap_set(sb.inode_bitmap_start, inode_no - 1, false);
    } else {
        uint64_t curr_node_blk = sb.dynamic_chunk_tree;
        bool found = false;
        while (curr_node_blk != 0) {
            struct ipo_inode_chunk_node node;
            if (!block_read(curr_node_blk, &node)) break;

            for (uint32_t i = 0; i < node.count && i < IPO_CHUNK_NODE_CHUNKS; i++) {
                struct ipo_inode_chunk_desc *c = &node.chunks[i];
                if (inode_no >= c->start_inode && inode_no < c->start_inode + c->inode_count) {
                    uint32_t bit = (uint32_t)(inode_no - c->start_inode);
                    c->bitmap &= ~(1ULL << bit);
                    c->free_inodes++;
                    block_write(curr_node_blk, &node);
                    if (cached_chunk_valid && cached_chunk.start_inode == c->start_inode) {
                        cached_chunk = *c;
                    }
                    found = true;
                    break;
                }
            }
            if (found) break;
            curr_node_blk = node.next_node;
        }

        if (sb.dynamic_inodes_used > 0) {
            sb.dynamic_inodes_used--;
            block_write(0, &sb);
        }
    }

    return true;
}

int64_t get_contiguous_data_blocks_for_inode(struct ipo_inode *inode, uint64_t logical_index, uint64_t *out_count) {
    if (!inode) return -1;
    if (out_count) *out_count = 1;

    /* 1. Search embedded primary extents */
    for (int i = 0; i < IPO_INODE_EXTENTS; i++) {
        struct ipo_extent *e = &inode->extents[i];
        if (e->block_count > 0) {
            if (logical_index >= e->logical_block && logical_index < e->logical_block + e->block_count) {
                uint64_t offset_in_ext = logical_index - e->logical_block;
                if (out_count) *out_count = e->block_count - offset_in_ext;
                return (int64_t)(e->physical_block + offset_in_ext);
            }
        }
    }

    /* 2. Search chained extent nodes */
    uint64_t curr_node_blk = inode->next_extent_node;
    while (curr_node_blk != 0) {
        struct ipo_extent_node enode;
        if (!block_read(curr_node_blk, &enode)) break;

        for (uint32_t i = 0; i < enode.count && i < IPO_EXTENT_NODE_EXTENTS; i++) {
            struct ipo_extent *e = &enode.extents[i];
            if (e->block_count > 0) {
                if (logical_index >= e->logical_block && logical_index < e->logical_block + e->block_count) {
                    uint64_t offset_in_ext = logical_index - e->logical_block;
                    if (out_count) *out_count = e->block_count - offset_in_ext;
                    return (int64_t)(e->physical_block + offset_in_ext);
                }
            }
        }
        curr_node_blk = enode.next_node;
    }

    return -1;
}

int64_t get_data_block_for_inode(struct ipo_inode *inode, uint64_t logical_index, bool alloc) {
    int64_t existing = get_contiguous_data_blocks_for_inode(inode, logical_index, NULL);
    if (existing >= 0) return existing;
    if (!alloc) return -1;

    /* 2. Traverse chained extent nodes to find the last node for extension */
    uint64_t curr_node_blk = inode->next_extent_node;
    uint64_t last_node_blk = 0;
    struct ipo_extent_node last_enode;
    memset(&last_enode, 0, sizeof(last_enode));

    while (curr_node_blk != 0) {
        struct ipo_extent_node enode;
        if (!block_read(curr_node_blk, &enode)) break;

        last_node_blk = curr_node_blk;
        memcpy(&last_enode, &enode, sizeof(enode));
        curr_node_blk = enode.next_node;
    }

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

bool ipo_fs_get_inode_stats(uint64_t *out_total, uint64_t *out_used, uint64_t *out_free, uint64_t *out_dynamic_chunks) {
    if (!fs_mounted) {
        if (out_total) *out_total = 0;
        if (out_used) *out_used = 0;
        if (out_free) *out_free = 0;
        if (out_dynamic_chunks) *out_dynamic_chunks = 0;
        return false;
    }

    uint64_t total = sb.inode_count;
    uint64_t primary_used = (sb.primary_inodes > 0) ? bitmap_count_set(sb.inode_bitmap_start, sb.primary_inodes) : 0;
    uint64_t dynamic_used = sb.dynamic_inodes_used;
    uint64_t used = primary_used + dynamic_used;
    uint64_t free_cnt = (total >= used) ? (total - used) : 0;

    if (out_total) *out_total = total;
    if (out_used) *out_used = used;
    if (out_free) *out_free = free_cnt;
    if (out_dynamic_chunks) *out_dynamic_chunks = sb.dynamic_chunks_count;
    return true;
}

bool ipo_fs_get_block_stats(uint64_t *out_total, uint64_t *out_used, uint64_t *out_free) {
    if (!fs_mounted) {
        if (out_total) *out_total = 0;
        if (out_used) *out_used = 0;
        if (out_free) *out_free = 0;
        return false;
    }

    uint64_t data_start = sb.data_blocks_start;
    uint64_t total = (sb.fs_size_blocks > data_start) ? (sb.fs_size_blocks - data_start) : 0;
    uint64_t used = (total > 0) ? bitmap_count_set(sb.block_bitmap_start, total) : 0;
    uint64_t free_cnt = (total >= used) ? (total - used) : 0;

    if (out_total) *out_total = total;
    if (out_used) *out_used = used;
    if (out_free) *out_free = free_cnt;
    return true;
}
