#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

if [ "$#" -ne 2 ]; then
	echo "Usage: $0 FIRMWARE_SOURCE_DIR ROOTFS_DIR" >&2
	exit 2
fi

src=$1
root=$2

for file in htbtfw20.tlv htnv20.bin; do
	if [ ! -r "$src/qca/$file" ]; then
		echo "Missing firmware: $src/qca/$file" >&2
		exit 1
	fi
done

target=$root/lib/firmware/qca
mkdir -p "$target"
for file in htbtfw20.tlv htnv20.bin; do
	install -m 0644 "$src/qca/$file" "$target/$file"
done
