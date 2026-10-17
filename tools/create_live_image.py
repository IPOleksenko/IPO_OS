#!/usr/bin/env python3
"""
tools/create_live_image.py - Builds a unified, bootable Live USB image for IPO_OS.

Layout of the unified Live USB medium (128 MB raw disk):
  Sector 0 (LBA 0):       MBR Bootloader with valid MBR Partition 1 entry (Type 0x83, LBA 2048)
  Sectors 1..N:           Kernel binary (sectors 1..~820)
  Sectors 2030:           Storage Pool Primary Superblock ('IPO_STORAGE_POOL', member ata0)
  Sectors 2031..2038:     Storage Pool WAL Journal (clean initialized)
  Sectors 2039:           Storage Pool Backup Superblock
  Sectors 2048+:          IPO_FS Filesystem Partition (/applications, /autorun, fonts, tools)

This allows writing the image directly to any USB flash drive
and booting on any PC without installation, preserving all changes on the USB flash drive.
"""

import argparse
import binascii
from pathlib import Path
import struct
import sys

SECTOR_SIZE = 512
FS_START_LBA = 2048
SB_PRIMARY_LBA = 2030
SB_BACKUP_LBA = 2039
WAL_JOURNAL_LBA = 2031
WAL_JOURNAL_SECTORS = 8

STORAGE_POOL_MAGIC = 0x504F4F4C  # 'POOL'
STORAGE_POOL_SB_VERSION = 2


def build_superblock(total_sectors: int, member_name: str = "ata0") -> bytes:
    """Builds a packed 512-byte storage_pool_disk_sb_t with valid CRC32."""
    pool_uuid = b"IPO_STORAGE_POOL\x00\x00\x00\x00"[:16]
    local_uuid = b"MEMBER-00\x00\x00\x00\x00\x00\x00\x00"[:16]
    member_idx = 0
    member_count = 1
    total_capacity = total_sectors
    chunk_size = 2048  # 1MB
    generation = 1
    state = 0  # STORAGE_POOL_STATE_ONLINE

    # Pack member 0 descriptor (48 bytes: uuid 16s, idx B, flags B, bdev_name 14s, start Q, cap Q)
    mem_name_bytes = member_name.encode("ascii").ljust(14, b"\x00")[:14]
    member0 = struct.pack(
        "<16sBB14sQQ",
        local_uuid,
        0,
        0x01,  # Online
        mem_name_bytes,
        0,
        total_sectors,
    )

    empty_member = b"\x00" * 48
    members_block = member0 + (empty_member * 7)
    assert len(members_block) == 384

    recovery_count = 0
    reserved = b"\x00" * 52

    # Pack first 508 bytes
    pre_crc = struct.pack(
        "<II16s16sIIQIII",
        STORAGE_POOL_MAGIC,
        STORAGE_POOL_SB_VERSION,
        pool_uuid,
        local_uuid,
        member_idx,
        member_count,
        total_capacity,
        chunk_size,
        generation,
        state,
    ) + members_block + struct.pack("<I52s", recovery_count, reserved)

    assert len(pre_crc) == 508, f"Expected 508 bytes, got {len(pre_crc)}"
    crc = binascii.crc32(pre_crc) & 0xFFFFFFFF
    sb = pre_crc + struct.pack("<I", crc)
    assert len(sb) == 512
    return sb


def inject_mbr_partition_table(boot_sector: bytes, total_sectors: int) -> bytes:
    """Injects MBR Partition 1 entry at bytes 446..461 of sector 0."""
    if len(boot_sector) < 512:
        boot_sector = boot_sector.ljust(512, b"\x00")

    mbr = bytearray(boot_sector[:512])

    # Partition 1 entry (16 bytes):
    # status=0x80 (Active), chs_start=(0, 32, 33), type=0x83 (Linux/IPO_FS), chs_end=(fe, ff, ff)
    # lba_start=2048, sector_count=total_sectors - 2048
    part_count = max(0, total_sectors - FS_START_LBA)
    part1_entry = struct.pack(
        "<B3sB3sII",
        0x80,                 # Bootable active
        b"\x00\x20\x21",      # CHS start
        0x83,                 # Type 0x83 (Linux / IPO_FS)
        b"\xfe\xff\xff",      # CHS end
        FS_START_LBA,         # LBA Start: 2048
        part_count,           # Sectors count
    )

    mbr[446:462] = part1_entry
    mbr[462:510] = b"\x00" * (510 - 462)  # Zero out partition entries 2, 3, 4
    # Ensure boot signature
    mbr[510] = 0x55
    mbr[511] = 0xAA

    return bytes(mbr)


def main():
    parser = argparse.ArgumentParser(description="Create unified Live USB bootable image for IPO_OS")
    repo_root = Path(__file__).resolve().parent.parent

    parser.add_argument("--boot", type=Path, default=repo_root / "build" / "boot" / "boot.bin")
    parser.add_argument("--kernel", type=Path, default=repo_root / "build" / "kernel" / "kernel.bin")
    parser.add_argument("--disk", type=Path, default=repo_root / "build" / "disk.img")
    parser.add_argument("--output", type=Path, default=repo_root / "build" / "IPO_OS.img")
    parser.add_argument("--size-mb", type=int, default=128)

    args = parser.parse_args()

    if not args.boot.exists() or not args.kernel.exists():
        print(f"Error: boot.bin or kernel.bin not found. Run make first.")
        return 1

    boot_bytes = args.boot.read_bytes()
    kernel_bytes = args.kernel.read_bytes()

    # Determine size from disk.img if present
    if args.disk.exists():
        disk_size = args.disk.stat().st_size
    else:
        disk_size = args.size_mb * 1024 * 1024

    total_sectors = disk_size // SECTOR_SIZE

    print(f"Building Unified Live USB OS Image ({total_sectors} sectors, {disk_size // (1024*1024)} MB)...")

    # 1. Prepare MBR with Partition Table
    mbr_sector = inject_mbr_partition_table(boot_bytes, total_sectors)

    # 2. Prepare Superblocks for ata0
    sb_bytes = build_superblock(total_sectors, member_name="ata0")

    # 3. Read filesystem data starting from FS_START_LBA (2048) if disk.img exists
    fs_data = b""
    if args.disk.exists():
        with open(args.disk, "rb") as f:
            f.seek(FS_START_LBA * SECTOR_SIZE)
            fs_data = f.read()

    # 4. Construct the complete raw image
    # Preallocate image memory
    image_data = bytearray(disk_size)

    # Sector 0: MBR Bootloader
    image_data[0:SECTOR_SIZE] = mbr_sector

    # Sectors 1..N: Kernel
    kernel_len = len(kernel_bytes)
    image_data[SECTOR_SIZE:SECTOR_SIZE + kernel_len] = kernel_bytes

    # Sector 2030: Primary Superblock
    image_data[SB_PRIMARY_LBA * SECTOR_SIZE:(SB_PRIMARY_LBA + 1) * SECTOR_SIZE] = sb_bytes

    # Sectors 2031..2038: Clean WAL Journal (zeroes)
    journal_start = WAL_JOURNAL_LBA * SECTOR_SIZE
    journal_end = (WAL_JOURNAL_LBA + WAL_JOURNAL_SECTORS) * SECTOR_SIZE
    image_data[journal_start:journal_end] = b"\x00" * (WAL_JOURNAL_SECTORS * SECTOR_SIZE)

    # Sector 2039: Backup Superblock
    image_data[SB_BACKUP_LBA * SECTOR_SIZE:(SB_BACKUP_LBA + 1) * SECTOR_SIZE] = sb_bytes

    # Sector 2048+: IPO_FS partition data
    if fs_data:
        fs_start_byte = FS_START_LBA * SECTOR_SIZE
        fs_len = min(len(fs_data), disk_size - fs_start_byte)
        image_data[fs_start_byte:fs_start_byte + fs_len] = fs_data[:fs_len]

    # Write output images
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with open(args.output, "wb") as f:
        f.write(image_data)
    print(f"  -> Generated {args.output} ({len(image_data)} bytes)")

    # Also update disk.img with MBR partition table and superblocks at 2030/2039 for dual-drive setups
    if args.disk.exists():
        sb_disk = build_superblock(total_sectors, member_name="ata1")
        with open(args.disk, "r+b") as f:
            # Inject MBR partition table into sector 0 of disk.img
            f.seek(0)
            cur_mbr = f.read(512)
            updated_mbr = inject_mbr_partition_table(cur_mbr, total_sectors)
            f.seek(0)
            f.write(updated_mbr)

            # Write superblocks
            f.seek(SB_PRIMARY_LBA * SECTOR_SIZE)
            f.write(sb_disk)
            f.seek(SB_BACKUP_LBA * SECTOR_SIZE)
            f.write(sb_disk)
        print(f"  -> Updated {args.disk} with MBR partition table and superblocks.")

    print("✓ Live USB image creation complete.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
