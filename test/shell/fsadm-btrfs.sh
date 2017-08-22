#!/usr/bin/env bash

# Copyright (C) 2017 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

test_description='Exercise fsadm filesystem resize for btrfs'
SKIP_WITH_LVMLOCKD=1
SKIP_WITH_LVMPOLLD=1

. lib/inittest

aux prepare_vg 1 512

# Skip testing if btrfs tools don't exist
btrfs_bin=btrfs
which btrfs || skip

vg_lv=$vg/$lv1
dev_vg_lv="$DM_DEV_DIR/$vg_lv"
mount_dir="mnt"
test ! -d "$mount_dir" && mkdir "$mount_dir"

# for recursive call
LVM_BINARY=$(which lvm)
export LVM_BINARY

cleanup_mounted_and_teardown()
{
	umount "$mount_dir" || true
	aux teardown
}

fscheck_btrfs()
{
	btrfs check "$dev_vg_lv"
}

# verify btrfs size
verify_btrfs_size()
{
	size="$1"".00MiB"
	real=""

	mount "$dev_vg_lv" "$mount_dir"
	real=$(btrfs filesystem usage --mbytes "$TESTDIR/$mount_dir" | grep "Device size" | cut -d":" -f2 | tr -d '[:blank:]')
	umount "$mount_dir"

	if [ "$size" != "$real" ] ; then
		echo "ERROR: device size($real) reported by btrfs mismatches $size"
		return 1
	fi
}

# enforce the PE size to 1M so that we can play with size number in
# MB unit and verify the result size without worrying auto size-rounding
# to PE boundary.
vgchange -s 1M $vg

lvcreate -n $lv1 -L300M $vg
trap 'cleanup_mounted_and_teardown' EXIT

#
# offline resize
#

# The default "sectorsize" value is the page size and is autodetected.
mkfs.btrfs "$dev_vg_lv"
fsadm --lvresize resize $vg_lv 400M
check lv_field $vg_lv size "400.00m"
verify_btrfs_size 400

# btrfs-progs v4.5.3 shows the minimum size for each btrfs device is 40M, but
# mkfs.btrfs will fail on device less than 100M. To be safe use 4M as it is
# in fsadm.sh, anyway.
not fsadm -y --lvresize resize "$dev_vg_lv" 4M
check lv_field $vg_lv size "400.00m"
verify_btrfs_size 400

fsadm -y --lvresize resize "$dev_vg_lv" 256M
check lv_field $vg_lv size "256.00m"
verify_btrfs_size 256

# Increase and reduce size
lvresize -L+10M -r $vg_lv
check lv_field $vg_lv size "266.00m"
verify_btrfs_size 266

lvreduce -L-10M -r $vg_lv
check lv_field $vg_lv size "256.00m"
verify_btrfs_size 256

fscheck_btrfs

vgremove -ff $vg
