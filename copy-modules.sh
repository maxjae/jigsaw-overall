#!/usr/bin/env bash

if [ -z "$1" ]; then
    echo "Missing image name."
    echo "Usage: ./copy-modules.sh <Path of image>"
    exit 1
fi

linux_dir="./spdm-linux"
image=$1
qemu_dir="$linux_dir/qemu_edu"

nix-shell -p gnumake --run "make -C $qemu_dir"
mkdir mount-point
sudo mount -o loop $image mount-point
sudo mkdir -p mount-point/modules
sudo cp $qemu_dir/*.ko mount-point/modules/
sudo umount mount-point
rm -rdf mount-point

