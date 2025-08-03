#!/bin/sh
set -e
. ./iso.sh

if [ ! -f "disk.img" ]; then
    echo "Creating virtual disk..."
    qemu-img create -f raw disk.img 10M
fi

qemu-system-$(./target-triplet-to-arch.sh $HOST) -cdrom IPO_OS.iso -hda disk.img

export DELETE_DISK=false
. ./clean.sh