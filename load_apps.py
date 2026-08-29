#!/usr/bin/env python3

import argparse
import shutil
import subprocess
import sys
from pathlib import Path


def run_disk_editor(args, command, *extra):
    cmd = [sys.executable, str(args.disk_editor), *command, *[str(x) for x in extra]]
    result = subprocess.run(cmd, cwd=args.project_root)
    if result.returncode != 0:
        raise SystemExit(f"disk_editor command failed: {' '.join(cmd)}")


def ensure_filesystem_ready(args):
    image = (args.project_root / args.image).resolve() if not args.image.is_absolute() else args.image.resolve()
    if not image.exists():
        run_disk_editor(args, ["-i", str(image), "-s", str(args.start_lba), "format"])
        return

    probe = subprocess.run(
        [sys.executable, str(args.disk_editor), "-i", str(image), "-s", str(args.start_lba), "ls", "/"],
        cwd=args.project_root,
        capture_output=True,
        text=True,
    )
    if probe.returncode != 0:
        print(f"Image {image} is not a valid IPO_FS filesystem; formatting it now.")
        run_disk_editor(args, ["-i", str(image), "-s", str(args.start_lba), "format"])


def main():
    parser = argparse.ArgumentParser(
        description="Load all built app binaries into the /app directory of the IPO_OS image using disk_editor.py."
    )
    parser.add_argument("--project-root", type=Path, default=Path(__file__).resolve().parent)
    parser.add_argument("--disk-editor", type=Path, default=Path(__file__).resolve().parent / "disk_editor.py")
    parser.add_argument("--image", type=Path, default=Path(__file__).resolve().parent / "build" / "disk.img")
    parser.add_argument("--start-lba", type=int, default=2048)
    parser.add_argument("--apps-dir", type=Path, default=Path(__file__).resolve().parent / "build" / "apps")
    parser.add_argument("--dry-run", action="store_true", help="Show what would be copied without modifying the image")
    args = parser.parse_args()

    project_root = args.project_root.resolve()
    disk_editor = args.disk_editor.resolve()
    apps_dir = (project_root / args.apps_dir).resolve() if not args.apps_dir.is_absolute() else args.apps_dir.resolve()
    image = (project_root / args.image).resolve() if not args.image.is_absolute() else args.image.resolve()

    if not disk_editor.exists():
        raise SystemExit(f"disk_editor not found: {disk_editor}")

    if not apps_dir.exists():
        raise SystemExit(f"apps directory not found: {apps_dir}. Run 'make apps' first.")

    matches = sorted(apps_dir.rglob("*.bin"))
    if not matches:
        raise SystemExit(f"No .bin app files found in {apps_dir}")

    print(f"Found {len(matches)} app(s) in {apps_dir}")

    if args.dry_run:
        for app in matches:
            app_name = app.name[:-4] if app.name.endswith(".bin") else app.name
            print(f"Would upload: {app.relative_to(project_root)} -> /app/{app_name}")
        return 0

    ensure_filesystem_ready(args)

    try:
        run_disk_editor(args, ["-i", str(image), "-s", str(args.start_lba), "ls", "/app"])
    except SystemExit:
        run_disk_editor(args, ["-i", str(image), "-s", str(args.start_lba), "mkdir", "/app"])

    for app in matches:
        app_name = app.name[:-4] if app.name.endswith(".bin") else app.name
        target = f"/app/{app_name}"
        print(f"Uploading {app.relative_to(project_root)} -> {target}")
        run_disk_editor(args, ["-i", str(image), "-s", str(args.start_lba), "put", str(app), target])

    print(f"Done. {len(matches)} app(s) loaded into /app on {image}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
