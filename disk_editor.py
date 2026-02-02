#!/usr/bin/env python3

import argparse
import os
import struct
import sys

BLOCK_SIZE = 512
IPO_FS_MAX_NAME = 64
IPO_FS_DIRECT_BLOCKS = 6

SB_FMT = '<8sIIIIIII'
SB_SIZE = struct.calcsize(SB_FMT)
INODE_FMT = '<III' + ('I' * IPO_FS_DIRECT_BLOCKS) + 'I36s'
INODE_SIZE = struct.calcsize(INODE_FMT)
INODES_PER_BLOCK = BLOCK_SIZE // INODE_SIZE
DIRENTRY_FMT = '<I B B 2s {}s'.format(IPO_FS_MAX_NAME)
DIRENTRY_SIZE = struct.calcsize(DIRENTRY_FMT)


class DiskError(Exception):
    pass


class DiskImage:
    def __init__(self, path, start_lba=2048):
        if not os.path.exists(path):
            raise DiskError(f"image not found: {path}")
        self.path = path
        self.start_lba = start_lba
        self.f = open(path, 'r+b')
        self._load_superblock()

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
        self.f.flush()
        os.fsync(self.f.fileno())

    # ================= SUPERBLOCK =================

    def _load_superblock(self):
        buf = self.read_block(0)
        sb = struct.unpack(SB_FMT, buf[:SB_SIZE])
        if sb[0].rstrip(b'\x00') != b'IPO_FS':
            raise DiskError("not IPO_FS")
        self.sb = {
            'fs_size_blocks': sb[1],
            'block_size': sb[2],
            'inode_count': sb[3],
            'inode_bitmap_start': sb[4],
            'block_bitmap_start': sb[5],
            'inode_table_start': sb[6],
            'data_blocks_start': sb[7],
        }

    # ================= INODES =================

    def read_inode(self, ino):
        if ino <= 0 or ino > self.sb['inode_count']:
            raise DiskError("invalid inode")
        idx = ino - 1
        block = self.sb['inode_table_start'] + idx // INODES_PER_BLOCK
        offset = (idx % INODES_PER_BLOCK) * INODE_SIZE
        raw = self.read_block(block)[offset:offset + INODE_SIZE]
        t = struct.unpack(INODE_FMT, raw)
        return {
            'mode': t[0],
            'size': t[1],
            'links_count': t[2],
            'direct': list(t[3:3 + IPO_FS_DIRECT_BLOCKS]),
            'indirect': t[3 + IPO_FS_DIRECT_BLOCKS],
        }

    def write_inode(self, ino, inode):
        idx = ino - 1
        block = self.sb['inode_table_start'] + idx // INODES_PER_BLOCK
        offset = (idx % INODES_PER_BLOCK) * INODE_SIZE
        buf = bytearray(self.read_block(block))
        packed = struct.pack(
            INODE_FMT,
            inode['mode'], inode['size'], inode['links_count'],
            *inode['direct'], inode['indirect'], b'\x00' * 36
        )
        buf[offset:offset + INODE_SIZE] = packed
        self.write_block(block, bytes(buf))

    def empty_inode(self):
        return {'mode': 0, 'size': 0, 'links_count': 0,
                'direct': [0] * IPO_FS_DIRECT_BLOCKS, 'indirect': 0}

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
        for i in range(total):
            if not self.bitmap_get(self.sb['block_bitmap_start'], i):
                self.bitmap_set(self.sb['block_bitmap_start'], i, 1)
                self.write_block(self.sb['data_blocks_start'] + i, b'\x00' * BLOCK_SIZE)
                return self.sb['data_blocks_start'] + i
        return -1

    # ================= BLOCKS FOR INODE =================

    def get_block_for_inode(self, inode, logical, alloc=False):
        if logical < IPO_FS_DIRECT_BLOCKS:
            if inode['direct'][logical] == 0:
                if not alloc:
                    return -1
                inode['direct'][logical] = self.allocate_block()
            return inode['direct'][logical]

        idx = logical - IPO_FS_DIRECT_BLOCKS
        per = BLOCK_SIZE // 4
        if idx >= per:
            return -1

        if inode['indirect'] == 0:
            if not alloc:
                return -1
            inode['indirect'] = self.allocate_block()
            self.write_block(inode['indirect'], b'\x00' * BLOCK_SIZE)

        ibuf = bytearray(self.read_block(inode['indirect']))
        ptr = struct.unpack('<I', ibuf[idx * 4:(idx + 1) * 4])[0]
        if ptr == 0:
            if not alloc:
                return -1
            ptr = self.allocate_block()
            ibuf[idx * 4:(idx + 1) * 4] = struct.pack('<I', ptr)
            self.write_block(inode['indirect'], bytes(ibuf))
        return ptr

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

    # ================= DIRECTORY =================

    def dir_entries(self, inode):
        size = inode['size']
        if size == 0:
            return
        buf = bytearray()
        for b in range((size + BLOCK_SIZE - 1) // BLOCK_SIZE):
            phys = self.get_block_for_inode(inode, b)
            if phys < 0:
                break
            buf += self.read_block(phys)
        for i in range(size // DIRENTRY_SIZE):
            off = i * DIRENTRY_SIZE
            inode_no, typ, namelen, _, name = struct.unpack(
                DIRENTRY_FMT, buf[off:off + DIRENTRY_SIZE]
            )
            if inode_no:
                yield {'inode': inode_no, 'type': typ,
                       'name': name[:namelen].decode()}

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
        entry = struct.pack(DIRENTRY_FMT, ino, typ, len(name),
                            b'\x00\x00', name.encode().ljust(IPO_FS_MAX_NAME, b'\x00'))
        off = din['size']
        bidx = off // BLOCK_SIZE
        rel = off % BLOCK_SIZE
        phys = self.get_block_for_inode(din, bidx, alloc=True)
        blk = bytearray(self.read_block(phys))
        blk[rel:rel + DIRENTRY_SIZE] = entry
        self.write_block(phys, bytes(blk))
        din['size'] += DIRENTRY_SIZE
        self.write_inode(dirino, din)
        return True

    def dir_remove_entry(self, dirino, name):
        din = self.read_inode(dirino)
        size = din['size']
        if size == 0:
            return False
        nentries = size // DIRENTRY_SIZE
        buf = bytearray()
        blocks = (size + BLOCK_SIZE - 1) // BLOCK_SIZE
        for bidx in range(blocks):
            phys = self.get_block_for_inode(din, bidx, alloc=False)
            if phys < 0:
                return False
            buf += self.read_block(phys)
        found = False
        newbuf = bytearray()
        for i in range(nentries):
            off = i * DIRENTRY_SIZE
            chunk = buf[off:off + DIRENTRY_SIZE]
            inode_no = struct.unpack('<I', chunk[:4])[0]
            name_len = chunk[5]
            name_bytes = chunk[8:8 + IPO_FS_MAX_NAME]
            entryname = name_bytes[:name_len].decode('utf-8', errors='ignore')
            if entryname == name:
                found = True
                continue
            newbuf += chunk
        if not found:
            return False
        # write newbuf across blocks
        new_size = len(newbuf)
        blocks_needed = (new_size + BLOCK_SIZE - 1) // BLOCK_SIZE if new_size > 0 else 0
        for bidx in range(blocks_needed):
            phys = self.get_block_for_inode(din, bidx, alloc=True)
            chunk = newbuf[bidx * BLOCK_SIZE:(bidx + 1) * BLOCK_SIZE]
            if len(chunk) < BLOCK_SIZE:
                chunk = chunk.ljust(BLOCK_SIZE, b'\x00')
            self.write_block(phys, chunk)
        # free extra blocks
        for bidx in range(blocks_needed, blocks):
            phys = self.get_block_for_inode(din, bidx, alloc=False)
            if phys and phys >= self.sb['data_blocks_start']:
                self.bitmap_set(self.sb['block_bitmap_start'], phys - self.sb['data_blocks_start'], 0)
        din['size'] = new_size
        self.write_inode(dirino, din)
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

        inode['direct'][0] = block

        # . and ..
        de_dot = struct.pack(DIRENTRY_FMT, ino, 1, 1, b'\x00\x00',
                             b'.'.ljust(IPO_FS_MAX_NAME, b'\x00'))
        de_ddot = struct.pack(DIRENTRY_FMT, parent, 1, 2, b'\x00\x00',
                              b'..'.ljust(IPO_FS_MAX_NAME, b'\x00'))

        buf = bytearray(BLOCK_SIZE)
        buf[0:len(de_dot)] = de_dot
        buf[len(de_dot):len(de_dot) + len(de_ddot)] = de_ddot
        self.write_block(block, bytes(buf))

        inode['size'] = len(de_dot) + len(de_ddot)
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

        # free existing data blocks
        nblocks = (inode['size'] + BLOCK_SIZE - 1) // BLOCK_SIZE if inode['size'] > 0 else 0
        for i in range(min(IPO_FS_DIRECT_BLOCKS, nblocks)):
            if inode['direct'][i]:
                self.bitmap_set(self.sb['block_bitmap_start'], inode['direct'][i] - self.sb['data_blocks_start'], 0)
                inode['direct'][i] = 0

        # free indirect blocks
        if inode['indirect']:
            ibuf = self.read_block(inode['indirect'])
            for j in range(0, BLOCK_SIZE, 4):
                ptr = struct.unpack('<I', ibuf[j:j+4])[0]
                if ptr:
                    self.bitmap_set(self.sb['block_bitmap_start'], ptr - self.sb['data_blocks_start'], 0)
            # free the indirect block itself
            self.bitmap_set(self.sb['block_bitmap_start'], inode['indirect'] - self.sb['data_blocks_start'], 0)
            inode['indirect'] = 0

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
            if inode['size'] > (DIRENTRY_SIZE * 2):
                return False
        # remove dir entry from parent
        if not self.dir_remove_entry(parent, name):
            return False
        # free direct blocks
        nblocks = (inode['size'] + BLOCK_SIZE - 1) // BLOCK_SIZE if inode['size'] > 0 else 0
        for i in range(min(IPO_FS_DIRECT_BLOCKS, nblocks)):
            if inode['direct'][i]:
                self.bitmap_set(self.sb['block_bitmap_start'], inode['direct'][i] - self.sb['data_blocks_start'], 0)
                inode['direct'][i] = 0
        # free indirect
        if inode['indirect']:
            ibuf = self.read_block(inode['indirect'])
            for j in range(0, BLOCK_SIZE, 4):
                ptr = struct.unpack('<I', ibuf[j:j+4])[0]
                if ptr:
                    self.bitmap_set(self.sb['block_bitmap_start'], ptr - self.sb['data_blocks_start'], 0)
            # free the indirect block itself
            self.bitmap_set(self.sb['block_bitmap_start'], inode['indirect'] - self.sb['data_blocks_start'], 0)
            inode['indirect'] = 0
        # clear inode bitmap
        self.bitmap_set(self.sb['inode_bitmap_start'], ino-1, 0)
        # zero the inode on disk
        self.write_inode(ino, self.empty_inode())
        return True


# ================= CLI =================

def main():
    p = argparse.ArgumentParser()
    p.add_argument('-i', '--image', default='build/disk.img')
    p.add_argument('-s', '--start-lba', type=int, default=2048)
    sub = p.add_subparsers(dest='cmd')
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

    d = DiskImage(args.image, args.start_lba)

    if args.cmd == 'ls':
        for n, t in d.ls(args.path):
            print(n + ('/' if t & 1 else ''))
    elif args.cmd == 'cat':
        sys.stdout.buffer.write(d.cat(args.path))
    elif args.cmd == 'mkdir':
        d.mkdir(args.path)
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
