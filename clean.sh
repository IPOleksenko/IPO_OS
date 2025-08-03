#!/bin/sh
set -e
. ./config.sh

DELETE_DISK=${DELETE_DISK:-false}

for arg in "$@"; do
  if [ "$arg" = "--delete-disk" ]; then
    DELETE_DISK=true
  fi
done

for PROJECT in $PROJECTS; do
  (cd $PROJECT && $MAKE clean)
done

rm -rf sysroot
rm -rf isodir
rm -rf IPO_OS.iso

if [ "$DELETE_DISK" = true ]; then
  echo "Deleting disk.img..."
  rm -rf disk.img
fi
