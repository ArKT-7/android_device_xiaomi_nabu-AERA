#!/bin/bash
set -e

out_img="../cust.img"
pack_dir=".tmp_cust_pack"
rm -rf "$pack_dir"
mkdir -p "$pack_dir"

# coyp only directories
for dir in */; do
    cp -a "$dir" "$pack_dir/"
done

# get size of the folders
dir_size=$(du -sm "$pack_dir" | cut -f1)

# a 5MB buffer
img_size=$((dir_size + 5))

# allocate raw space
dd if=/dev/zero of="$out_img" bs=1M count=$img_size status=none

# format exactly matching the stock partition layout
mke2fs -q -F -b 4096 -I 256 \
    -O none,ext_attr,resize_inode,filetype,extent,sparse_super,large_file,uninit_bg \
    -L cust -d "$pack_dir" "$out_img"

# disable max-mount-count and interval cheks to prevent kernel ext4 warnings
tune2fs -c 0 -i 0 "$out_img" >/dev/null 2>&1 || true

# clean up
rm -rf "$pack_dir"

echo "Done, saved to $out_img (${img_size}MB)"
echo "Flash with: fastboot flash cust $out_img"
