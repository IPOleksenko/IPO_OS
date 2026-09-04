#!/usr/bin/env python3

import argparse
import os
import struct
import sys

BLOCK_SIZE = 512
IPO_INODE_EXTENTS = 4
IPO_EXTENT_NODE_EXTENTS = 20
IPO_INODE_TYPE_DIR = 0x1
IPO_INODE_TYPE_FILE = 0x2

SB_FMT = '<8sQIIQQQQQ'
SB_SIZE = struct.calcsize(SB_FMT)

EXTENT_FMT = '<QQII'
EXTENT_SIZE = struct.calcsize(EXTENT_FMT)

INODE_FMT = '<IIQ' + ('QQII' * IPO_INODE_EXTENTS) + 'Q8s'
INODE_SIZE = struct.calcsize(INODE_FMT)
INODES_PER_BLOCK = BLOCK_SIZE // INODE_SIZE
DIRENTRY_HDR_FMT = '<III B 3s'
DIRENTRY_HDR_SIZE = struct.calcsize(DIRENTRY_HDR_FMT)


class DiskError(Exception):
    pass


class DiskImage:
    def __init__(self, path, start_lba=2048, require_format=False):
        if not os.path.exists(path):
            raise DiskError(f"image not found: {path}")
        self.path = path
        self.start_lba = start_lba
        self.f = open(path, 'r+b')
        self.sb = None
        try:
            self._load_superblock()
        except DiskError as e:
            if require_format:
                self.sb = None
            else:
                raise

    def close(self):
        self.f.flush()
        os.fsync(self.f.fileno())
        self.f.close()

    # ================= BLOCK IO =================

    def _seek_block(self, idx):
        self.f.seek((self.start_lba + idx) * BLOCK_SIZE)

    def read_block(self, idx):
        self._seek_block(idx)
        data = self.f.read(BLOCK_SIZE)
        if len(data) != BLOCK_SIZE:
            raise DiskError("short read")
        return data

    def write_block(self, idx, data):
        if len(data) != BLOCK_SIZE:
            raise DiskError("bad block size")
        self._seek_block(idx)
        self.f.write(data)

    # ================= SUPERBLOCK =================

    def _load_superblock(self):
        buf = self.read_block(0)
        sb = struct.unpack(SB_FMT, buf[:SB_SIZE])
        if sb[0].rstrip(b'\x00') != b'IPO_FS':
            raise DiskError("not IPO_FS")
        self.sb = {
            'fs_size_blocks': sb[1],
            'block_size': sb[2],
            'flags': sb[3],
            'inode_count': sb[4],
            'inode_bitmap_start': sb[5],
            'block_bitmap_start': sb[6],
            'inode_table_start': sb[7],
            'data_blocks_start': sb[8],
        }

    def format_disk(self, total_inodes=None):
        """Format disk with a new IPO_FS filesystem"""
        # Get disk size  
        self.f.seek(0, 2)  # seek to end
        disk_size = self.f.tell() // BLOCK_SIZE
        total_blocks = disk_size - self.start_lba
        
        if total_blocks < 100:
            raise DiskError("disk too small")

        if total_inodes is None:
            total_inodes = max(1, total_blocks // 16)
        # Calculate layout
        inode_bitmap_blocks = (total_inodes + 4095) // 4096
        block_bitmap_blocks = (total_blocks + 4095) // 4096
        inode_table_blocks = (total_inodes * INODE_SIZE + BLOCK_SIZE - 1) // BLOCK_SIZE
        
        inode_bitmap_start = 1
        block_bitmap_start = inode_bitmap_start + inode_bitmap_blocks
        inode_table_start = block_bitmap_start + block_bitmap_blocks
        data_blocks_start = inode_table_start + inode_table_blocks
        
        # Create superblock
        self.sb = {
            'fs_size_blocks': total_blocks,
            'block_size': BLOCK_SIZE,
            'flags': 1,
            'inode_count': total_inodes,
            'inode_bitmap_start': inode_bitmap_start,
            'block_bitmap_start': block_bitmap_start,
            'inode_table_start': inode_table_start,
            'data_blocks_start': data_blocks_start,
        }
        
        # Write superblock
        magic = b'IPO_FS\x00\x00'
        sb_data = struct.pack(SB_FMT,
            magic,
            self.sb['fs_size_blocks'],
            self.sb['block_size'],
            1,
            self.sb['inode_count'],
            self.sb['inode_bitmap_start'],
            self.sb['block_bitmap_start'],
            self.sb['inode_table_start'],
            self.sb['data_blocks_start'],
        )
        # Pad to BLOCK_SIZE
        sb_block = sb_data + b'\x00' * (BLOCK_SIZE - len(sb_data))
        self.write_block(0, sb_block)
        
        # Zero out bitmaps and inode table
        zero_block = b'\x00' * BLOCK_SIZE
        for i in range(inode_bitmap_start, inode_table_start + inode_table_blocks):
            self.write_block(i, zero_block)
        
        # Allocate root inode and create root directory
        self.bitmap_set(inode_bitmap_start, 0, 1)  # inode 1 is root
        root_block = self.allocate_block()
        root_inode = self.empty_inode()
        root_inode['mode'] = IPO_INODE_TYPE_DIR
        root_inode['links_count'] = 2
        root_inode['extents'][0] = {
            'logical_block': 0,
            'physical_block': root_block,
            'block_count': 1,
            'flags': 0,
        }
        dots = bytearray(BLOCK_SIZE)
        dots[:40] = self.pack_dir_dots(1, 1)
        self.write_block(root_block, bytes(dots))
        root_inode['size'] = 40
        self.write_inode(1, root_inode)
        
        # Create /app directory
        self.bitmap_set(inode_bitmap_start, 1, 1)  # inode 2 is /app
        app_block = self.allocate_block()
        app_inode = self.empty_inode()
        app_inode['mode'] = IPO_INODE_TYPE_DIR | 0x80000000
        app_inode['links_count'] = 2
        app_inode['extents'][0] = {
            'logical_block': 0,
            'physical_block': app_block,
            'block_count': 1,
            'flags': 0,
        }
        dots = bytearray(BLOCK_SIZE)
        dots[:40] = self.pack_dir_dots(2, 1)
        self.write_block(app_block, bytes(dots))
        app_inode['size'] = 40
        self.write_inode(2, app_inode)
        self.dir_add_entry(1, 'app', 2, IPO_INODE_TYPE_DIR)

        # Create /autorun file (empty, protected)
        self.bitmap_set(inode_bitmap_start, 2, 1)  # inode 3 is /autorun
        ar_inode = self.empty_inode()
        ar_inode['mode'] = IPO_INODE_TYPE_FILE | 0x80000000
        ar_inode['links_count'] = 1
        self.write_inode(3, ar_inode)
        self.dir_add_entry(1, 'autorun', 3, IPO_INODE_TYPE_FILE)
        
        print("Disk formatted successfully")
    # ================= INODES =================

    def read_inode(self, ino):
        if ino <= 0 or ino > self.sb['inode_count']:
            raise DiskError("invalid inode")
        idx = ino - 1
        block = self.sb['inode_table_start'] + idx // INODES_PER_BLOCK
        offset = (idx % INODES_PER_BLOCK) * INODE_SIZE
        raw = self.read_block(block)[offset:offset + INODE_SIZE]
        mode, links_count, size = struct.unpack_from('<IIQ', raw, 0)
        extents = []
        for i in range(IPO_INODE_EXTENTS):
            e_off = 16 + i * 24
            l_blk, p_blk, b_cnt, flg = struct.unpack_from('<QQII', raw, e_off)
            extents.append({
                'logical_block': l_blk,
                'physical_block': p_blk,
                'block_count': b_cnt,
                'flags': flg,
            })
        next_extent_node = struct.unpack_from('<Q', raw, 16 + IPO_INODE_EXTENTS * 24)[0]
        return {
            'mode': mode,
            'links_count': links_count,
            'size': size,
            'extents': extents,
            'next_extent_node': next_extent_node,
        }

    def write_inode(self, ino, inode):
        idx = ino - 1
        block = self.sb['inode_table_start'] + idx // INODES_PER_BLOCK
        offset = (idx % INODES_PER_BLOCK) * INODE_SIZE
        buf = bytearray(self.read_block(block))
        packed = bytearray(INODE_SIZE)
        struct.pack_into('<IIQ', packed, 0, inode['mode'], inode['links_count'], inode['size'])
        for i in range(IPO_INODE_EXTENTS):
            ext = inode['extents'][i] if i < len(inode['extents']) else {'logical_block': 0, 'physical_block': 0, 'block_count': 0, 'flags': 0}
            struct.pack_into('<QQII', packed, 16 + i * 24, ext['logical_block'], ext['physical_block'], ext['block_count'], ext['flags'])
        struct.pack_into('<Q8s', packed, 16 + IPO_INODE_EXTENTS * 24, inode.get('next_extent_node', 0), b'\x00' * 8)
        buf[offset:offset + INODE_SIZE] = packed
        self.write_block(block, bytes(buf))

    def empty_inode(self):
        return {
            'mode': 0,
            'links_count': 0,
            'size': 0,
            'extents': [{'logical_block': 0, 'physical_block': 0, 'block_count': 0, 'flags': 0} for _ in range(IPO_INODE_EXTENTS)],
            'next_extent_node': 0,
        }

    def free_inode_blocks(self, inode):
        # Free primary extents
        for ext in inode.get('extents', []):
            if ext['block_count'] > 0 and ext['physical_block'] > 0:
                for b in range(ext['block_count']):
                    self.bitmap_set(self.sb['block_bitmap_start'], (ext['physical_block'] + b) - self.sb['data_blocks_start'], 0)
            ext['block_count'] = 0
            ext['physical_block'] = 0
            ext['logical_block'] = 0

        # Free chained extent nodes
        curr = inode.get('next_extent_node', 0)
        while curr != 0:
            raw = self.read_block(curr)
            count, flags, next_node = struct.unpack_from('<IIQ', raw, 0)
            for i in range(min(count, IPO_EXTENT_NODE_EXTENTS)):
                e_off = 16 + i * 24
                l_blk, p_blk, b_cnt, flg = struct.unpack_from('<QQII', raw, e_off)
                for b in range(b_cnt):
                    self.bitmap_set(self.sb['block_bitmap_start'], (p_blk + b) - self.sb['data_blocks_start'], 0)
            self.bitmap_set(self.sb['block_bitmap_start'], curr - self.sb['data_blocks_start'], 0)
            curr = next_node
        inode['next_extent_node'] = 0

    # ================= BITMAPS =================

    def bitmap_get(self, start, bit):
        byte = bit // 8
        block = byte // BLOCK_SIZE
        off = byte % BLOCK_SIZE
        return (self.read_block(start + block)[off] >> (bit & 7)) & 1

    def bitmap_set(self, start, bit, val):
        byte = bit // 8
        block = byte // BLOCK_SIZE
        off = byte % BLOCK_SIZE
        buf = bytearray(self.read_block(start + block))
        if val:
            buf[off] |= 1 << (bit & 7)
        else:
            buf[off] &= ~(1 << (bit & 7)) & 0xFF
        self.write_block(start + block, bytes(buf))

    # ================= ALLOCATION =================

    def allocate_inode(self):
        for i in range(self.sb['inode_count']):
            if not self.bitmap_get(self.sb['inode_bitmap_start'], i):
                self.bitmap_set(self.sb['inode_bitmap_start'], i, 1)
                self.write_inode(i + 1, self.empty_inode())
                return i + 1
        return -1

    def allocate_block(self):
        total = self.sb['fs_size_blocks'] - self.sb['data_blocks_start']
        num_bitmap_blocks = (total + BLOCK_SIZE * 8 - 1) // (BLOCK_SIZE * 8)
        for b in range(num_bitmap_blocks):
            blk_lba = self.sb['block_bitmap_start'] + b
            raw = bytearray(self.read_block(blk_lba))
            words = struct.unpack('<' + ('I' * (BLOCK_SIZE // 4)), raw)
            for w_idx, w in enumerate(words):
                if w != 0xFFFFFFFF:
                    for bit in range(32):
                        if not (w & (1 << bit)):
                            bit_idx = (b * BLOCK_SIZE * 8) + (w_idx * 32) + bit
                            if bit_idx >= total:
                                return -1
                            raw[w_idx * 4 + (bit // 8)] |= (1 << (bit % 8))
                            self.write_block(blk_lba, bytes(raw))
                            phys = self.sb['data_blocks_start'] + bit_idx
                            self.write_block(phys, b'\x00' * BLOCK_SIZE)
                            return phys
        return -1

    # ================= BLOCKS FOR INODE (LINKED EXTENTS) =================

    def get_block_for_inode(self, inode, logical, alloc=False):
        # 1. Search primary extents
        for ext in inode['extents']:
            if ext['block_count'] > 0:
                if ext['logical_block'] <= logical < ext['logical_block'] + ext['block_count']:
                    return ext['physical_block'] + (logical - ext['logical_block'])

        # 2. Search chained extent nodes
        curr = inode.get('next_extent_node', 0)
        last_node_blk = 0
        last_raw = None
        while curr != 0:
            raw = bytearray(self.read_block(curr))
            count, flags, next_node = struct.unpack_from('<IIQ', raw, 0)
            for i in range(min(count, IPO_EXTENT_NODE_EXTENTS)):
                e_off = 16 + i * 24
                l_blk, p_blk, b_cnt, flg = struct.unpack_from('<QQII', raw, e_off)
                if b_cnt > 0 and l_blk <= logical < l_blk + b_cnt:
                    return p_blk + (logical - l_blk)
            last_node_blk = curr
            last_raw = raw
            curr = next_node

        if not alloc:
            return -1

        # 3. Allocate new block
        blk = self.allocate_block()
        if blk < 0:
            raise DiskError("disk full: no free blocks")

        # Check if we can extend the last primary extent
        for ext in inode['extents']:
            if ext['block_count'] > 0 and ext['logical_block'] + ext['block_count'] == logical and ext['physical_block'] + ext['block_count'] == blk:
                ext['block_count'] += 1
                return blk

        # Check for free slot in primary extents
        for ext in inode['extents']:
            if ext['block_count'] == 0:
                ext['logical_block'] = logical
                ext['physical_block'] = blk
                ext['block_count'] = 1
                ext['flags'] = 0
                return blk

        # Check if last chained node can extend its last extent
        if last_node_blk != 0 and last_raw is not None:
            count, flags, next_node = struct.unpack_from('<IIQ', last_raw, 0)
            if count > 0:
                last_off = 16 + (count - 1) * 24
                l_blk, p_blk, b_cnt, flg = struct.unpack_from('<QQII', last_raw, last_off)
                if l_blk + b_cnt == logical and p_blk + b_cnt == blk:
                    struct.pack_into('<QQII', last_raw, last_off, l_blk, p_blk, b_cnt + 1, flg)
                    self.write_block(last_node_blk, bytes(last_raw))
                    return blk

            if count < IPO_EXTENT_NODE_EXTENTS:
                new_off = 16 + count * 24
                struct.pack_into('<QQII', last_raw, new_off, logical, blk, 1, 0)
                struct.pack_into('<I', last_raw, 0, count + 1)
                self.write_block(last_node_blk, bytes(last_raw))
                return blk

        # Need a new chained node block
        new_node_blk = self.allocate_block()
        if new_node_blk < 0:
            raise DiskError("disk full: no free blocks")

        node_buf = bytearray(BLOCK_SIZE)
        struct.pack_into('<IIQ', node_buf, 0, 1, 0, 0)
        struct.pack_into('<QQII', node_buf, 16, logical, blk, 1, 0)
        self.write_block(new_node_blk, bytes(node_buf))

        if last_node_blk != 0 and last_raw is not None:
            struct.pack_into('<Q', last_raw, 8, new_node_blk)
            self.write_block(last_node_blk, bytes(last_raw))
        else:
            inode['next_extent_node'] = new_node_blk

        return blk

    # ================= PATH =================

    def path_resolve(self, path):
        path = os.path.normpath(path)
        if path == '/':
            return 1
        if not path.startswith('/'):
            return -1
        cur = 1
        for name in [p for p in path.split('/') if p]:
            inode = self.read_inode(cur)
            if not (inode['mode'] & 1):
                return -1
            ent = self.find_entry(cur, name)
            if not ent:
                return -1
            cur = ent['inode']
        return cur

    def path_parent(self, path):
        path = os.path.normpath(path)
        if path == '/':
            return -1, ''
        parts = [p for p in path.split('/') if p]
        name = parts[-1]
        parent = '/' + '/'.join(parts[:-1]) if len(parts) > 1 else '/'
        return self.path_resolve(parent), name

    def read_inode_bytes(self, inode, size, offset):
        if offset >= inode['size']:
            return b''
        if offset + size > inode['size']:
            size = inode['size'] - offset
        first_block = offset // BLOCK_SIZE
        last_block = (offset + size - 1) // BLOCK_SIZE
        result = bytearray()
        copied = 0
        for b in range(first_block, last_block + 1):
            phys = self.get_block_for_inode(inode, b, alloc=False)
            if phys <= 0:
                break
            block_data = self.read_block(phys)
            block_offset = offset % BLOCK_SIZE if b == first_block else 0
            tocopy = min(BLOCK_SIZE - block_offset, size - copied)
            result.extend(block_data[block_offset:block_offset + tocopy])
            copied += tocopy
        return bytes(result)

    def write_inode_bytes(self, ino, inode, data, offset):
        size = len(data)
        if size == 0:
            return 0
        first_block = offset // BLOCK_SIZE
        last_block = (offset + size - 1) // BLOCK_SIZE
        written = 0
        for b in range(first_block, last_block + 1):
            phys = self.get_block_for_inode(inode, b, alloc=True)
            if phys <= 0:
                break
            block_data = bytearray(self.read_block(phys))
            block_offset = offset % BLOCK_SIZE if b == first_block else 0
            towrite = min(BLOCK_SIZE - block_offset, size - written)
            block_data[block_offset:block_offset + towrite] = data[written:written + towrite]
            self.write_block(phys, bytes(block_data))
            written += towrite
        if offset + written > inode['size']:
            inode['size'] = offset + written
            if ino > 0:
                self.write_inode(ino, inode)
        return written

    def pack_dir_dots(self, self_ino, parent_ino):
        # . -> rec_len = 20, name_len = 1
        d1 = struct.pack(DIRENTRY_HDR_FMT, self_ino, 20, 1, IPO_INODE_TYPE_DIR, b'\x00\x00\x00') + b'.\x00\x00\x00'
        # .. -> rec_len = 20, name_len = 2
        d2 = struct.pack(DIRENTRY_HDR_FMT, parent_ino, 20, 2, IPO_INODE_TYPE_DIR, b'\x00\x00\x00') + b'..\x00\x00'
        return d1 + d2

    # ================= DIRECTORY =================

    def dir_entries(self, inode):
        size = inode['size']
        offset = 0
        while offset + DIRENTRY_HDR_SIZE <= size:
            raw_hdr = self.read_inode_bytes(inode, DIRENTRY_HDR_SIZE, offset)
            if len(raw_hdr) < DIRENTRY_HDR_SIZE:
                break
            ino, rec_len, name_len, typ, _ = struct.unpack(DIRENTRY_HDR_FMT, raw_hdr)
            if rec_len < DIRENTRY_HDR_SIZE:
                break
            if ino != 0 and name_len > 0:
                raw_name = self.read_inode_bytes(inode, name_len, offset + DIRENTRY_HDR_SIZE)
                name = raw_name.decode('utf-8', errors='replace')
                yield {'inode': ino, 'type': typ, 'name': name, 'offset': offset, 'rec_len': rec_len}
            offset += rec_len

    def find_entry(self, dirino, name):
        din = self.read_inode(dirino)
        for e in self.dir_entries(din):
            if e['name'] == name:
                return e
        return None

    def dir_add_entry(self, dirino, name, ino, typ):
        din = self.read_inode(dirino)
        if self.find_entry(dirino, name):
            return False
        name_bytes = name.encode('utf-8')
        name_len = len(name_bytes)
        needed_rec_len = (DIRENTRY_HDR_SIZE + name_len + 1 + 3) & ~3

        # Search for a deleted slot (inode == 0) with rec_len >= needed_rec_len
        offset = 0
        target_offset = din['size']
        actual_rec_len = needed_rec_len
        while offset + DIRENTRY_HDR_SIZE <= din['size']:
            raw_hdr = self.read_inode_bytes(din, DIRENTRY_HDR_SIZE, offset)
            if len(raw_hdr) < DIRENTRY_HDR_SIZE:
                break
            slot_ino, slot_rec_len, _, _, _ = struct.unpack(DIRENTRY_HDR_FMT, raw_hdr)
            if slot_rec_len < DIRENTRY_HDR_SIZE:
                break
            if slot_ino == 0 and slot_rec_len >= needed_rec_len:
                target_offset = offset
                actual_rec_len = slot_rec_len
                break
            offset += slot_rec_len

        entry_data = struct.pack(DIRENTRY_HDR_FMT, ino, actual_rec_len, name_len, typ, b'\x00\x00\x00') + name_bytes + b'\x00'
        if len(entry_data) < actual_rec_len:
            entry_data += b'\x00' * (actual_rec_len - len(entry_data))

        self.write_inode_bytes(dirino, din, entry_data, target_offset)
        return True

    def dir_remove_entry(self, dirino, name):
        din = self.read_inode(dirino)
        ent = self.find_entry(dirino, name)
        if not ent:
            return False
        hdr = struct.pack('<I', 0)
        self.write_inode_bytes(dirino, din, hdr, ent['offset'])
        return True

    # ================= USER COMMANDS =================

    def ls(self, path='/'):
        ino = self.path_resolve(path)
        if ino < 0:
            raise DiskError("path not found")
        inode = self.read_inode(ino)
        if not (inode['mode'] & 1):
            raise DiskError("not dir")
        return [(e['name'], e['type']) for e in self.dir_entries(inode)]

    def cat(self, path):
        ino = self.path_resolve(path)
        if ino < 0:
            raise DiskError("file not found")
        inode = self.read_inode(ino)
        if inode['mode'] & 1:
            raise DiskError("is dir")
        data = bytearray()
        for b in range((inode['size'] + BLOCK_SIZE - 1) // BLOCK_SIZE):
            phys = self.get_block_for_inode(inode, b)
            data += self.read_block(phys)
        return bytes(data[:inode['size']])

    def mkdir(self, path):
        parent, name = self.path_parent(path)

        if parent < 0 or not name:
            raise DiskError("invalid path")

        if self.find_entry(parent, name):
            raise DiskError("already exists")

        ino = self.allocate_inode()
        if ino < 0:
            raise DiskError("no free inode")

        inode = self.empty_inode()
        inode['mode'] = 1  # directory
        inode['links_count'] = 2

        block = self.allocate_block()
        if block < 0:
            raise DiskError("no free block")

        inode['extents'][0] = {
            'logical_block': 0,
            'physical_block': block,
            'block_count': 1,
            'flags': 0,
        }

        # . and ..
        dots = bytearray(BLOCK_SIZE)
        dots[:40] = self.pack_dir_dots(ino, parent)
        self.write_block(block, bytes(dots))

        inode['size'] = 40
        self.write_inode(ino, inode)

        if not self.dir_add_entry(parent, name, ino, 1):
            raise DiskError("dir_add failed")

    def write_text(self, path, text):
        parent, name = self.path_parent(path)
        ino = self.path_resolve(path)
        if ino < 0:
            ino = self.allocate_inode()
            inode = self.empty_inode()
            inode['mode'] = 2
            inode['links_count'] = 1
            self.write_inode(ino, inode)
            self.dir_add_entry(parent, name, ino, 2)
        inode = self.read_inode(ino)
        data = text.encode()
        inode['size'] = len(data)
        for i in range((len(data) + BLOCK_SIZE - 1) // BLOCK_SIZE):
            phys = self.get_block_for_inode(inode, i, alloc=True)
            chunk = data[i * BLOCK_SIZE:(i + 1) * BLOCK_SIZE].ljust(BLOCK_SIZE, b'\x00')
            self.write_block(phys, chunk)
        self.write_inode(ino, inode)

    def put(self, localpath, destpath):
        """Copy a local host file into `destpath` on the IPO_FS image."""
        if not os.path.isfile(localpath):
            raise DiskError(f"local file not found: {localpath}")
        with open(localpath, 'rb') as lf:
            data = lf.read()
        # Normalize omitted or dot-like destinations to root
        if destpath is None or destpath == '' or destpath in ('.', './'):
            destpath = '/'

        # If destpath is root, place file inside root with basename
        if destpath == '/':
            parent = 1
            name = os.path.basename(localpath)
            target = -1
        else:
            parent, name = self.path_parent(destpath)
            if parent < 0:
                raise DiskError('invalid dest path')

            # If the provided destpath is a special name like '.' or '..',
            # treat it as a directory target and place the file inside it
            if name in ('.', '..'):
                name = os.path.basename(localpath)

            # If destpath resolves to an existing inode and it's a directory,
            # copy the file into that directory using the local filename.
            target = self.path_resolve(destpath)
            if target >= 0:
                tinode = self.read_inode(target)
                if tinode['mode'] & 1:
                    parent = target
                    name = os.path.basename(localpath)
                    target = -1

        if target < 0:
            ino = self.allocate_inode()
            if ino < 0:
                raise DiskError('no inodes')
            inode = self.empty_inode()
            inode['mode'] = 2
            inode['links_count'] = 1
            self.write_inode(ino, inode)
            if not self.dir_add_entry(parent, name, ino, 2):
                raise DiskError('dir add failed')
            target = ino

        inode = self.read_inode(target)
        self.free_inode_blocks(inode)
        inode['size'] = 0
        self.write_inode(target, inode)

        # write new data
        size = len(data)
        written = 0
        for i in range((size + BLOCK_SIZE - 1) // BLOCK_SIZE if size > 0 else 0):
            phys = self.get_block_for_inode(inode, i, alloc=True)
            chunk = data[i * BLOCK_SIZE:(i + 1) * BLOCK_SIZE]
            if len(chunk) < BLOCK_SIZE:
                chunk = chunk.ljust(BLOCK_SIZE, b'\x00')
            self.write_block(phys, chunk)
            written += len(chunk)

        inode['size'] = size
        self.write_inode(target, inode)
        return True

    def delete(self, path):
        """Delete a file or empty directory at `path`. Returns True on success."""
        parent, name = self.path_parent(path)
        if parent < 0:
            return False
        ent = self.find_entry(parent, name)
        if not ent:
            return False
        ino = ent['inode']
        inode = self.read_inode(ino)
        # protected flag (if set, don't delete)
        if inode['mode'] & 0x80000000:
            return False
        # directory: allow only empty (only '.' and '..')
        if inode['mode'] & 1:
            for e in self.dir_entries(inode):
                if e['name'] not in ('.', '..'):
                    return False
        # remove dir entry from parent
        if not self.dir_remove_entry(parent, name):
            return False

        # free blocks
        self.free_inode_blocks(inode)
        # clear inode bitmap
        self.bitmap_set(self.sb['inode_bitmap_start'], ino - 1, 0)
        # zero the inode on disk
        self.write_inode(ino, self.empty_inode())
        return True


# ================= CLI =================

def main():
    p = argparse.ArgumentParser()
    p.add_argument('-i', '--image', default='build/disk.img')
    p.add_argument('-s', '--start-lba', type=int, default=2048)
    sub = p.add_subparsers(dest='cmd')
    sub.add_parser('format')
    sub.add_parser('ls').add_argument('path', nargs='?', default='/')
    sub.add_parser('cat').add_argument('path')
    sub.add_parser('mkdir').add_argument('path')
    p_touch = sub.add_parser('touch')
    p_touch.add_argument('path')
    p_touch.add_argument('text', nargs='?')
    p_put = sub.add_parser('put')
    p_put.add_argument('src')
    p_put.add_argument('dest', nargs='?', default='/')
    p_rm = sub.add_parser('rm')
    p_rm.add_argument('path')
    args = p.parse_args()

    require_format = args.cmd == 'format'
    d = DiskImage(args.image, args.start_lba, require_format=require_format)

    if args.cmd == 'format':
        d.format_disk()
    elif args.cmd == 'ls':
        for n, t in d.ls(args.path):
            print(n + ('/' if t & 1 else ''))
    elif args.cmd == 'cat':
        sys.stdout.buffer.write(d.cat(args.path))
    elif args.cmd == 'mkdir':
        try:
            d.mkdir(args.path)
        except DiskError as e:
            if "already exists" not in str(e):
                raise
    elif args.cmd == 'touch':
        if getattr(args, 'text', None) is None:
            d.write_text(args.path, '')
        else:
            if os.path.isfile(args.text):
                d.put(args.text, args.path)
            else:
                d.write_text(args.path, args.text)
    elif args.cmd == 'put':
        d.put(args.src, args.dest)
    elif args.cmd == 'rm':
        if not d.delete(args.path):
            print('rm failed', file=sys.stderr); sys.exit(1)

    d.close()


if __name__ == '__main__':
    main()
