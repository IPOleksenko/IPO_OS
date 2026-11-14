#include <file_system/ipo_fs.h>
#include <memory/kmalloc.h>
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

static bool locate_inode_slot(uint64_t inode_no, uint64_t *out_block, uint32_t *out_offset) {
    if (inode_no == 0 || inode_no > sb.inode_count) {
        return false;
    }
    if (inode_no <= sb.primary_inodes) {
        uint64_t idx = inode_no - 1;
        *out_block = sb.inode_table_start + (idx / INODES_PER_BLOCK);
        *out_offset = (uint32_t)((idx % INODES_PER_BLOCK) * INODE_SIZE);
        return true;
    }
    return find_dynamic_inode_location(inode_no, out_block, out_offset);
}

bool read_inode(uint64_t inode_no, struct ipo_inode *out) {
    if (!out) return false;
    uint64_t block = 0;
    uint32_t offset = 0;
    if (!locate_inode_slot(inode_no, &block, &offset)) {
        if (inode_no > sb.primary_inodes && inode_no <= sb.inode_count) {
            serial_printf("[ipo_fs] read_inode(%llu) failed: dynamic inode not found in chunks\n", inode_no);
        }
        return false;
    }

    uint8_t *buf = (uint8_t *)kmalloc(IPO_FS_BLOCK_SIZE);
    if (!buf) return false;
    if (!block_read(block, buf)) {
        serial_printf("[ipo_fs] read_inode(%llu) failed: block_read(%llu) failed\n", inode_no, block);
        kfree(buf);
        return false;
    }
    memcpy(out, buf + offset, sizeof(*out));
    kfree(buf);
    return true;
}

bool write_inode(uint64_t inode_no, const struct ipo_inode *in) {
    if (!in) return false;
    uint64_t block = 0;
    uint32_t offset = 0;
    if (!locate_inode_slot(inode_no, &block, &offset)) {
        return false;
    }

    uint8_t *buf = (uint8_t *)kmalloc(IPO_FS_BLOCK_SIZE);
    if (!buf) return false;
    if (!block_read(block, buf)) {
        kfree(buf);
        return false;
    }
    memcpy(buf + offset, in, sizeof(*in));
    bool ok = block_write(block, buf);
    kfree(buf);
    return ok;
}

static int64_t expand_and_allocate_inode(void) {
    uint64_t target_node_blk = 0;
    uint64_t prev_node_blk = 0;
    struct ipo_inode_chunk_node target_node;
    struct ipo_inode_chunk_node prev_node;
    memset(&target_node, 0, sizeof(target_node));
    memset(&prev_node, 0, sizeof(prev_node));

    uint64_t last_chunk_end_ino = (sb.primary_inodes > 0) ? sb.primary_inodes : 0;
    bool created_new_node = false;
    bool need_chain_link = false;

    if (sb.dynamic_chunk_tree == 0) {
        int64_t blk = allocate_block();
        if (blk < 0) {
            ipo_fs_set_last_error(IPO_FS_ERR_NO_SPACE);
            serial_printf("[ipo_fs] Inode expansion FAILED: cannot allocate chunk node (volume full)\n");
            return -1;
        }
        target_node_blk = (uint64_t)blk;
        created_new_node = true;
    } else {
        uint64_t curr = sb.dynamic_chunk_tree;
        while (curr != 0) {
            if (!block_read(curr, &target_node)) {
                ipo_fs_set_last_error(IPO_FS_ERR_IO);
                return -1;
            }
            if (target_node.count > 0 && target_node.count <= IPO_CHUNK_NODE_CHUNKS) {
                struct ipo_inode_chunk_desc *last_c = &target_node.chunks[target_node.count - 1];
                last_chunk_end_ino = last_c->start_inode + last_c->inode_count - 1;
            }
            prev_node_blk = curr;
            curr = target_node.next_node;
        }
        if (target_node.count < IPO_CHUNK_NODE_CHUNKS) {
            target_node_blk = prev_node_blk;
        } else {
            int64_t blk = allocate_block();
            if (blk < 0) {
                ipo_fs_set_last_error(IPO_FS_ERR_NO_SPACE);
                serial_printf("[ipo_fs] Inode expansion FAILED: cannot allocate chained chunk node (volume full)\n");
                return -1;
            }
            prev_node = target_node;
            memset(&target_node, 0, sizeof(target_node));
            target_node_blk = (uint64_t)blk;
            created_new_node = true;
            need_chain_link = true;
        }
    }

    /* Allocate inode table blocks (16 down to 1 block) before linking metadata on disk */
    uint32_t blocks_to_alloc = 16;
    int64_t table_phys = -1;
    while (blocks_to_alloc >= 1) {
        table_phys = allocate_contiguous_blocks(blocks_to_alloc);
        if (table_phys >= 0) break;
        blocks_to_alloc /= 2;
    }

    if (table_phys < 0) {
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
    desc->free_inodes = chunk_inodes - 1;
    desc->bitmap = 1ULL;
    target_node.count++;

    if (!block_write(target_node_blk, &target_node)) {
        for (uint32_t b = 0; b < blocks_to_alloc; b++) free_block((uint64_t)table_phys + b);
        if (created_new_node) free_block(target_node_blk);
        ipo_fs_set_last_error(IPO_FS_ERR_IO);
        return -1;
    }

    if (need_chain_link) {
        prev_node.next_node = target_node_blk;
        if (!block_write(prev_node_blk, &prev_node)) {
            for (uint32_t b = 0; b < blocks_to_alloc; b++) free_block((uint64_t)table_phys + b);
            free_block(target_node_blk);
            ipo_fs_set_last_error(IPO_FS_ERR_IO);
            return -1;
        }
    }

    uint64_t prev_tree = sb.dynamic_chunk_tree;
    if (sb.dynamic_chunk_tree == 0) {
        sb.dynamic_chunk_tree = target_node_blk;
    }
    sb.dynamic_chunks_count++;
    sb.dynamic_inodes_count += chunk_inodes;
    sb.dynamic_inodes_used++;
    sb.inode_count = sb.primary_inodes + sb.dynamic_inodes_count;

    if (!block_write(0, &sb)) {
        sb.dynamic_chunk_tree = prev_tree;
        sb.dynamic_chunks_count--;
        sb.dynamic_inodes_count -= chunk_inodes;
        sb.dynamic_inodes_used--;
        sb.inode_count = sb.primary_inodes + sb.dynamic_inodes_count;
        if (need_chain_link) {
            prev_node.next_node = 0;
            block_write(prev_node_blk, &prev_node);
        } else if (!created_new_node) {
            target_node.count--;
            memset(&target_node.chunks[target_node.count], 0, sizeof(target_node.chunks[0]));
            block_write(target_node_blk, &target_node);
        }
        for (uint32_t b = 0; b < blocks_to_alloc; b++) free_block((uint64_t)table_phys + b);
        if (created_new_node) free_block(target_node_blk);
        ipo_fs_set_last_error(IPO_FS_ERR_IO);
        return -1;
    }

    cached_chunk = *desc;
    cached_chunk_valid = true;

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

    /* Try primary inode bitmap */
    if (sb.primary_inodes > 0) {
        uint64_t bitmap_blocks = (sb.primary_inodes + (IPO_FS_BLOCK_SIZE * 8) - 1) / (IPO_FS_BLOCK_SIZE * 8);
        uint8_t *buf = (uint8_t *)kmalloc(IPO_FS_BLOCK_SIZE);
        if (!buf) {
            ipo_fs_set_last_error(IPO_FS_ERR_IO);
            return -1;
        }

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
                                    kfree(buf);
                                    ipo_fs_set_last_error(IPO_FS_ERR_IO);
                                    return -1;
                                }
                                kfree(buf);

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
        kfree(buf);
    }

    /* Try existing dynamic chunks */
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

    /* Primary and existing chunks full: expand chunk tree */
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

    /* Search embedded primary extents */
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

    /* Search chained extent nodes */
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

    /* Traverse chained extent nodes to locate the tail node */
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

    int64_t new_phys = allocate_block();
    if (new_phys < 0) return -1;

    /* Extend last primary extent if physically contiguous */
    for (int i = 0; i < IPO_INODE_EXTENTS; i++) {
        struct ipo_extent *e = &inode->extents[i];
        if (e->block_count > 0 &&
            e->logical_block + e->block_count == logical_index &&
            e->physical_block + e->block_count == (uint64_t)new_phys) {
            e->block_count++;
            return new_phys;
        }
    }

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

    /* Extend or append in the last chained extent node */
    if (last_node_blk != 0 && last_enode.count > 0) {
        struct ipo_extent *last_e = &last_enode.extents[last_enode.count - 1];
        if (last_e->logical_block + last_e->block_count == logical_index &&
            last_e->physical_block + last_e->block_count == (uint64_t)new_phys) {
            last_e->block_count++;
            if (!block_write(last_node_blk, &last_enode)) {
                last_e->block_count--;
                free_block((uint64_t)new_phys);
                return -1;
            }
            return new_phys;
        }

        if (last_enode.count < IPO_EXTENT_NODE_EXTENTS) {
            struct ipo_extent *new_e = &last_enode.extents[last_enode.count++];
            new_e->logical_block = logical_index;
            new_e->physical_block = (uint64_t)new_phys;
            new_e->block_count = 1;
            new_e->flags = 0;
            if (!block_write(last_node_blk, &last_enode)) {
                last_enode.count--;
                free_block((uint64_t)new_phys);
                return -1;
            }
            return new_phys;
        }
    }

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
    if (!block_write((uint64_t)new_node_blk, &new_enode)) {
        free_block((uint64_t)new_node_blk);
        free_block((uint64_t)new_phys);
        return -1;
    }

    if (last_node_blk != 0) {
        last_enode.next_node = (uint64_t)new_node_blk;
        if (!block_write(last_node_blk, &last_enode)) {
            free_block((uint64_t)new_node_blk);
            free_block((uint64_t)new_phys);
            return -1;
        }
    } else {
        inode->next_extent_node = (uint64_t)new_node_blk;
    }

    return new_phys;
}

int inode_read_bytes(struct ipo_inode *inode, void *buffer, uint32_t size, uint64_t offset) {
    if (!inode || !buffer || size == 0) return 0;
    if (offset >= inode->size) return 0;
    if (offset + size > inode->size) size = (uint32_t)(inode->size - offset);

    uint64_t first_block = offset / IPO_FS_BLOCK_SIZE;
    uint64_t last_block = (offset + size - 1) / IPO_FS_BLOCK_SIZE;
    uint8_t *tmp = (uint8_t *)kmalloc(IPO_FS_BLOCK_SIZE);
    if (!tmp) return 0;
    uint32_t copied = 0;

    uint64_t b = first_block;
    while (b <= last_block && copied < size) {
        uint64_t max_contig = 1;
        int64_t phys = get_contiguous_data_blocks_for_inode(inode, b, &max_contig);
        if (phys < 0) break;

        uint64_t blocks_remaining = last_block - b + 1;
        if (max_contig > blocks_remaining) max_contig = blocks_remaining;

        uint32_t block_offset = (b == first_block) ? (uint32_t)(offset % IPO_FS_BLOCK_SIZE) : 0u;

        if (block_offset != 0) {
            if (!block_read((uint64_t)phys, tmp)) break;
            uint32_t tocopy = IPO_FS_BLOCK_SIZE - block_offset;
            if (tocopy > size - copied) tocopy = size - copied;
            memcpy((uint8_t *)buffer + copied, tmp + block_offset, tocopy);
            copied += tocopy;
            b++;
            continue;
        }

        uint64_t full_blocks = (size - copied) / IPO_FS_BLOCK_SIZE;
        uint64_t batch_blocks = (max_contig < full_blocks) ? max_contig : full_blocks;
        if (batch_blocks > 128) batch_blocks = 128;

        if (batch_blocks > 0) {
            if (!block_read_multi((uint64_t)phys, (uint16_t)batch_blocks, (uint8_t *)buffer + copied)) {
                break;
            }
            copied += (uint32_t)(batch_blocks * IPO_FS_BLOCK_SIZE);
            b += batch_blocks;
            continue;
        }

        if (!block_read((uint64_t)phys, tmp)) break;
        uint32_t tocopy = size - copied;
        if (tocopy > IPO_FS_BLOCK_SIZE) tocopy = IPO_FS_BLOCK_SIZE;
        memcpy((uint8_t *)buffer + copied, tmp, tocopy);
        copied += tocopy;
        b++;
    }
    kfree(tmp);
    return (int)copied;
}

int inode_write_bytes(uint64_t inode_no, struct ipo_inode *inode, const void *buffer, uint32_t size, uint64_t offset) {
    if (!inode || !buffer || size == 0) return 0;

    uint64_t first_block = offset / IPO_FS_BLOCK_SIZE;
    uint64_t last_block = (offset + size - 1) / IPO_FS_BLOCK_SIZE;
    uint8_t *tmp = (uint8_t *)kmalloc(IPO_FS_BLOCK_SIZE);
    if (!tmp) return 0;
    uint32_t written = 0;
    bool inode_dirty = false;

    for (uint64_t b = first_block; b <= last_block; b++) {
        int64_t existing = get_contiguous_data_blocks_for_inode(inode, b, NULL);
        int64_t phys = existing;
        if (phys < 0) {
            phys = get_data_block_for_inode(inode, b, true);
            if (phys < 0) break;
            inode_dirty = true;
        }
        uint32_t block_offset = (b == first_block) ? (uint32_t)(offset % IPO_FS_BLOCK_SIZE) : 0u;
        uint32_t towrite = IPO_FS_BLOCK_SIZE - block_offset;
        if (towrite > size - written) towrite = size - written;

        if (block_offset != 0 || towrite < IPO_FS_BLOCK_SIZE) {
            if (existing < 0) {
                memset(tmp, 0, IPO_FS_BLOCK_SIZE);
            } else if (!block_read((uint64_t)phys, tmp)) {
                break;
            }
        }
        memcpy(tmp + block_offset, (const uint8_t *)buffer + written, towrite);
        if (!block_write((uint64_t)phys, tmp)) break;
        written += towrite;
    }

    kfree(tmp);
    if (offset + written > inode->size) {
        inode->size = offset + written;
        inode_dirty = true;
    }
    if ((written > 0 || inode_dirty) && inode_no > 0) {
        write_inode(inode_no, inode);
    }
    return (int)written;
}

static void trim_extent_to_blocks(struct ipo_extent *e, uint64_t keep_blocks) {
    if (!e || e->block_count == 0) return;
    uint64_t end = e->logical_block + e->block_count;
    if (end <= keep_blocks) return;

    if (e->logical_block >= keep_blocks) {
        free_extent_blocks(e);
        memset(e, 0, sizeof(*e));
        return;
    }

    uint32_t keep_cnt = (uint32_t)(keep_blocks - e->logical_block);
    for (uint32_t b = keep_cnt; b < e->block_count; b++) {
        free_block(e->physical_block + b);
    }
    e->block_count = keep_cnt;
}

bool inode_truncate_blocks(uint64_t inode_no, struct ipo_inode *inode, uint64_t new_size) {
    if (!inode) return false;
    uint64_t keep_blocks = (new_size == 0) ? 0 : ((new_size + IPO_FS_BLOCK_SIZE - 1) / IPO_FS_BLOCK_SIZE);

    for (int i = 0; i < IPO_INODE_EXTENTS; i++) {
        trim_extent_to_blocks(&inode->extents[i], keep_blocks);
    }

    uint64_t curr_node_blk = inode->next_extent_node;
    uint64_t prev_node_blk = 0;
    struct ipo_extent_node prev_enode;
    memset(&prev_enode, 0, sizeof(prev_enode));

    while (curr_node_blk != 0) {
        struct ipo_extent_node enode;
        if (!block_read(curr_node_blk, &enode)) break;
        uint64_t next_blk = enode.next_node;

        uint32_t live = 0;
        for (uint32_t i = 0; i < enode.count && i < IPO_EXTENT_NODE_EXTENTS; i++) {
            trim_extent_to_blocks(&enode.extents[i], keep_blocks);
            if (enode.extents[i].block_count > 0) {
                if (live != i) {
                    enode.extents[live] = enode.extents[i];
                    memset(&enode.extents[i], 0, sizeof(enode.extents[i]));
                }
                live++;
            }
        }
        enode.count = live;

        if (live == 0) {
            if (prev_node_blk == 0) {
                inode->next_extent_node = next_blk;
            } else {
                prev_enode.next_node = next_blk;
                block_write(prev_node_blk, &prev_enode);
            }
            free_block(curr_node_blk);
        } else {
            block_write(curr_node_blk, &enode);
            prev_node_blk = curr_node_blk;
            prev_enode = enode;
        }
        curr_node_blk = next_blk;
    }

    inode->size = new_size;
    if (inode_no > 0) {
        return write_inode(inode_no, inode);
    }
    return true;
}
