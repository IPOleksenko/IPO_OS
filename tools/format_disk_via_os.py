#!/usr/bin/env python3
"""
Format disk image via IPO_OS kernel in headless QEMU.
"""

import argparse
import os
import struct
import subprocess
import sys
import time
from pathlib import Path

FS_START_LBA = 2048
SECTOR_SIZE = 512
IPO_FS_MAGIC = b"IPO_FS\x00\x00"


def inject_mbr_partition_table(boot_sector: bytes, total_sectors: int) -> bytes:
    """Injects MBR Partition 1 entry at bytes 446..461 of sector 0."""
    if len(boot_sector) < 512:
        boot_sector = boot_sector.ljust(512, b"\x00")
    mbr = bytearray(boot_sector[:512])

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
    mbr[462:510] = b"\x00" * (510 - 462)
    mbr[510] = 0x55
    mbr[511] = 0xAA
    return bytes(mbr)


def is_disk_formatted(disk_path: Path) -> bool:
    """Check if disk has valid IPO_FS superblock at FS_START_LBA."""
    if not disk_path.exists() or disk_path.stat().st_size < (FS_START_LBA + 1) * SECTOR_SIZE:
        return False
    try:
        with open(disk_path, "rb") as f:
            f.seek(FS_START_LBA * SECTOR_SIZE)
            magic = f.read(8)
            return magic[:6] == b"IPO_FS"
    except Exception:
        return False


def main():
    parser = argparse.ArgumentParser(description="Format virtual disk using IPO_OS native kernel in QEMU.")
    repo_root = Path(__file__).resolve().parent.parent
    parser.add_argument("--disk", type=Path, default=repo_root / "build" / "disk.img", help="Target disk image")
    parser.add_argument("--boot", type=Path, default=repo_root / "build" / "boot" / "boot.bin", help="Bootloader binary")
    parser.add_argument("--kernel", type=Path, default=repo_root / "build" / "kernel" / "kernel.bin", help="Kernel binary")
    parser.add_argument("--qemu", type=str, default="qemu-system-i386", help="QEMU executable")
    parser.add_argument("--size-mb", type=int, default=128, help="Disk size in megabytes")
    parser.add_argument("--force", action="store_true", help="Force reformat even if already formatted")
    parser.add_argument("--timeout", type=float, default=20.0, help="Maximum timeout in seconds to wait for OS mount")
    args = parser.parse_args()

    disk_path = args.disk.resolve()
    boot_bin = args.boot.resolve()
    kernel_bin = args.kernel.resolve()

    if not boot_bin.exists() or not kernel_bin.exists():
        print(f"Error: {boot_bin} or {kernel_bin} not found. Build kernel & bootloader first.", file=sys.stderr)
        return 1

    # Check if disk is already formatted
    if not args.force and is_disk_formatted(disk_path):
        print(f"Disk {disk_path} is already formatted with IPO_FS.")
        return 0

    print(f"Preparing {disk_path} ({args.size_mb} MB) for in-kernel formatting...")
    disk_path.parent.mkdir(parents=True, exist_ok=True)

    total_bytes = args.size_mb * 1024 * 1024
    total_sectors = total_bytes // SECTOR_SIZE

    # Create / zero the disk image if needed or forced
    if not disk_path.exists() or args.force or disk_path.stat().st_size != total_bytes:
        with open(disk_path, "wb") as f:
            f.seek(total_bytes - 1)
            f.write(b"\x00")

    # Ensure MBR partition table is in sector 0
    with open(disk_path, "r+b") as f:
        f.seek(0)
        sector0 = f.read(512)
        if sector0[510:512] != b"\x55\xaa":
            updated_mbr = inject_mbr_partition_table(sector0, total_sectors)
            f.seek(0)
            f.write(updated_mbr)

    # Build pure kernel boot image (drive 0) without any filesystem partition
    boot_kernel_img = repo_root / "build" / "boot_kernel.img"
    with open(boot_bin, "rb") as fb, open(kernel_bin, "rb") as fk:
        boot_kernel_data = fb.read() + fk.read()
    pad = (len(boot_kernel_data) + 511) // 512 * 512
    boot_kernel_data = boot_kernel_data.ljust(pad, b"\x00")
    with open(boot_kernel_img, "wb") as fbk:
        fbk.write(boot_kernel_data)

    # Launch QEMU with OS kernel (hda) and target disk (hdb)
    qemu_cmd = [
        args.qemu,
        "-m", "2048",
        "-drive", f"format=raw,file={boot_kernel_img},if=ide,index=0,file.locking=off",
        "-drive", f"format=raw,file={disk_path},if=ide,index=1,file.locking=off",
        "-display", "none",
        "-serial", "stdio"
    ]

    print("Booting IPO_OS kernel in headless mode to format & mount filesystem...")
    proc = subprocess.Popen(
        qemu_cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1
    )

    t0 = time.time()
    mounted = False
    log_lines = []

    try:
        while time.time() - t0 < args.timeout:
            line = proc.stdout.readline()
            if not line:
                if proc.poll() is not None:
                    break
                time.sleep(0.05)
                continue

            stripped = line.strip()
            log_lines.append(stripped)

            if "Formatting" in stripped:
                print(f"  [kernel] {stripped}")
            elif "Mounted IPO_FS" in stripped:
                print(f"  [kernel] {stripped}")
                mounted = True
                break

    finally:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=1)

    if not mounted:
        print("Error: IPO_OS did not mount the filesystem within the timeout period.", file=sys.stderr)
        print("Last serial output lines:", file=sys.stderr)
        for l in log_lines[-15:]:
            print(f"  {l}", file=sys.stderr)
        return 1

    # Verify that the disk actually contains the superblock
    if not is_disk_formatted(disk_path):
        print(f"Error: {disk_path} did not contain valid IPO_FS superblock after OS format.", file=sys.stderr)
        return 1

    elapsed = time.time() - t0
    print(f"✓ IPO_FS filesystem formatted and mounted by IPO_OS kernel in {elapsed:.2f}s.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

