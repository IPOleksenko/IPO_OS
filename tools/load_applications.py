#!/usr/bin/env python3

import argparse
import os
import sys
from pathlib import Path

from disk_editor import DiskImage, DiskError


def safe_mkdir(disk, path):
    try:
        disk.mkdir(path)
    except Exception:
        pass


def safe_put(disk, src_path, dest_path):
    try:
        disk.put(str(src_path), dest_path)
    except Exception as e:
        print(f"Warning putting {src_path} -> {dest_path}: {e}")


def safe_put_executable(disk, src_path, dest_path):
    safe_put(disk, src_path, dest_path)


def main():
    parser = argparse.ArgumentParser(
        description="Load all built applications, libraries, headers, toolchains, and tests into IPO_OS disk image."
    )
    repo_root = Path(__file__).resolve().parent.parent
    parser.add_argument("--project-root", type=Path, default=repo_root)
    parser.add_argument("--image", type=Path, default=repo_root / "build" / "disk.img")
    parser.add_argument("--start-lba", type=int, default=2048)
    parser.add_argument("--apps-dir", type=Path, default=repo_root / "build" / "applications")
    parser.add_argument("--dry-run", action="store_true", help="Show what would be copied without modifying the image")
    args = parser.parse_args()

    project_root = args.project_root.resolve()
    image = (project_root / args.image).resolve() if not args.image.is_absolute() else args.image.resolve()
    apps_dir = (project_root / args.apps_dir).resolve() if not args.apps_dir.is_absolute() else args.apps_dir.resolve()
    if not apps_dir.exists():
        fallback_apps = project_root / "build" / "apps"
        if fallback_apps.exists():
            apps_dir = fallback_apps

    if not image.exists():
        raise FileNotFoundError(f"Disk image {image} does not exist. Run 'make disks' or 'make format-disk' first.")

    # Open existing filesystem (formatted by OS)
    try:
        disk = DiskImage(str(image), start_lba=args.start_lba, require_format=False)
    except DiskError as e:
        raise RuntimeError(
            f"Filesystem on {image} is not mounted/formatted ({e}). "
            f"Please run 'make format-disk' to format it via the IPO_OS kernel."
        )

    # Mandatory OS registered components: only /applications and /autorun
    safe_mkdir(disk, "/applications")
    autorun_file = repo_root / "autorun"
    if autorun_file.exists():
        safe_put(disk, autorun_file, "/autorun")
    else:
        try:
            disk.cat("/autorun")
        except Exception:
            try:
                disk.write_text("/autorun", "")
            except Exception:
                pass

    # 1. Upload applications to /applications
    if apps_dir.exists():
        app_files = sorted(apps_dir.rglob("*.bin"))
        print(f"Uploading {len(app_files)} app(s) to /applications...")
        for app in app_files:
            app_name = app.name[:-4] if app.name.endswith(".bin") else app.name
            safe_put_executable(disk, app, f"/applications/{app_name}")
            if app_name == "ifconfig":
                safe_put_executable(disk, app, "/applications/ipconfig")

        doom_wad = project_root / "applications" / "doom" / "doom1.wad"
        if doom_wad.exists():
            safe_put(disk, doom_wad, "/applications/doom1.wad")

        # Ensure /lib directory exists on disk
        safe_mkdir(disk, "/lib")

    # 2. Upload toolchain binaries to /applications
    toolchain_dir = project_root / "build" / "toolchain"
    tool_map = {
        "gcc": toolchain_dir / "gcc.elf",
        "g++": toolchain_dir / "g++.elf",
        "ld": toolchain_dir / "ld.elf",
        "as": toolchain_dir / "as.elf",
        "ar": toolchain_dir / "ar.elf",
        "ranlib": toolchain_dir / "ranlib.elf",
        "objcopy": toolchain_dir / "objcopy.elf",
        "objdump": toolchain_dir / "objdump.elf",
        "readelf": toolchain_dir / "readelf.elf",
        "nm": toolchain_dir / "nm.elf",
        "strip": toolchain_dir / "strip.elf",
        "nasm": toolchain_dir / "nasm.elf",
        "micropython": toolchain_dir / "python.elf",
        "python": toolchain_dir / "python.elf",
        "lua": toolchain_dir / "lua" / "lua.elf",
        "luac": toolchain_dir / "lua" / "luac.elf",
    }

    print("Uploading toolchain binaries to /applications...")
    for tool_name, tool_path in tool_map.items():
        if tool_path.exists():
            safe_put_executable(disk, tool_path, f"/applications/{tool_name}")
        else:
            print(f"Notice: Toolchain binary not found: {tool_path}")

    # 3. Bundled fonts
    fonts_dir = project_root / "build" / "fonts"
    font_files = sorted(fonts_dir.glob("*.fnt")) if fonts_dir.exists() else []
    safe_mkdir(disk, "/fonts")
    for font in font_files:
        destination = f"/fonts/{font.name}"
        disk.put(str(font), destination)
        if disk.cat(destination) != font.read_bytes():
            raise RuntimeError(f"Font copy verification failed: {font} -> {destination}")

    # 4. Bundled compiler headers (only if present)
    tcc_include = project_root / "applications" / "tcc" / "include"
    if not tcc_include.exists():
        tcc_include = project_root / "apps" / "tcc" / "include"
    if tcc_include.exists():
        safe_mkdir(disk, "/include")
        for h in tcc_include.glob("*.h"):
            safe_put(disk, h, f"/include/{h.name}")

    include_dir = project_root / "applications" / "include"
    if not include_dir.exists():
        include_dir = project_root / "apps" / "include"
    if include_dir.exists():
        safe_mkdir(disk, "/include")
        for h in include_dir.glob("*.h"):
            safe_put(disk, h, f"/include/{h.name}")

        sys_include = include_dir / "sys"
        if sys_include.exists():
            safe_mkdir(disk, "/include/sys")
            for h in sys_include.glob("*.h"):
                safe_put(disk, h, f"/include/sys/{h.name}")

        cxx_include = include_dir / "c++"
        if cxx_include.exists():
            safe_mkdir(disk, "/include/c++")
            for h in cxx_include.glob("*"):
                if h.is_file():
                    safe_put(disk, h, f"/include/c++/{h.name}")

    # 5. Bundled C/C++ runtime and libraries (only if present)
    lib_dir = project_root / "build" / "lib"
    lib_files = [
        "libc.a",
        "libm.a",
        "libstdc++.a",
        "libgcc.a",
        "crt0.o",
        "crti.o",
        "crtn.o",
        "crt1.o",
        "libtcc1.a",
    ]
    has_lib = any((lib_dir / lf).exists() for lf in lib_files)
    if has_lib:
        safe_mkdir(disk, "/lib")
        for lf in lib_files:
            p = lib_dir / lf
            if p.exists():
                safe_put(disk, p, f"/lib/{lf}")

        linker_script = project_root / "applications" / "lib" / "linker.ld"
        if not linker_script.exists():
            linker_script = project_root / "src" / "userland" / "linker.ld"
        if not linker_script.exists():
            linker_script = project_root / "lib" / "linker.ld"
        if linker_script.exists():
            safe_put(disk, linker_script, "/lib/linker.ld")

        safe_mkdir(disk, "/lib/python")

    disk.close()

    print("✓ Successfully populated IPO_OS filesystem image.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
