#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# Host-side smoke test for fake_pci_sriov multiple-PF support.
set -euo pipefail

MODULE=${MODULE:-samples/pci/fake_pci_sriov.ko}
MODULE_ARGS=${MODULE_ARGS:-num_pfs=2}
VENDOR=${VENDOR:-0x1d55}
PF_DEVICE=${PF_DEVICE:-0x1000}
VF_DEVICE=${VF_DEVICE:-0x1001}
EXPECT_PFS=${EXPECT_PFS:-2}
EXPECT_PF_CLASS=${EXPECT_PF_CLASS:-0xff0000}
EXPECT_VF_CLASS=${EXPECT_VF_CLASS:-0xff0000}
LOG=${LOG:-/tmp/fake_pci_multi_pf_smoke.log}

: > "$LOG"
exec > >(tee -a "$LOG") 2>&1

msg() { echo "== $* =="; }

find_fake_devs() {
	local device=$1 d
	for d in /sys/bus/pci/devices/*; do
		[ "$(cat "$d/vendor" 2>/dev/null || true)" = "$VENDOR" ] || continue
		[ "$(cat "$d/device" 2>/dev/null || true)" = "$device" ] || continue
		basename "$d"
	done | sort
}

cleanup() {
	set +e
	msg cleanup
	LOG=/tmp/fake_pci_multi_pf_cleanup.log \
		samples/pci/cleanup_fake_pci_sriov.sh || true
	msg "log saved to $LOG"
}
trap cleanup EXIT

assert_class() {
	local dev=$1 expected=$2 class

	class=$(cat "/sys/bus/pci/devices/$dev/class")
	echo "DEV=$dev CLASS=$class EXPECT_CLASS=$expected"
	[ "$class" = "$expected" ] || {
		echo "FAIL: $dev class $class, expected $expected"
		exit 1
	}
}

assert_no_rom() {
	local dev=$1

	if lspci -D -s "$dev" -vv | grep -qi 'Expansion ROM'; then
		echo "FAIL: $dev unexpectedly has an expansion ROM resource"
		lspci -D -s "$dev" -vv
		exit 1
	fi
}

cd "$(dirname "$0")/../.."
command -v lspci >/dev/null
sudo -n true

if grep -q '^fake_pci_sriov ' /proc/modules ||
   [ -n "$(find_fake_devs "$PF_DEVICE")$(find_fake_devs "$VF_DEVICE")" ]; then
	LOG=/tmp/fake_pci_multi_pf_pre_cleanup.log \
		samples/pci/cleanup_fake_pci_sriov.sh || true
fi

msg "insmod $MODULE $MODULE_ARGS"
# shellcheck disable=SC2086
sudo -n insmod "$MODULE" $MODULE_ARGS
sleep 1

mapfile -t pfs < <(find_fake_devs "$PF_DEVICE")
printf 'PF_LIST=%s\n' "${pfs[*]}"
[ "${#pfs[@]}" -eq "$EXPECT_PFS" ] || {
	echo "FAIL: expected $EXPECT_PFS PFs, found ${#pfs[@]}"
	exit 1
}

for pf in "${pfs[@]}"; do
	assert_class "$pf" "$EXPECT_PF_CLASS"
	assert_no_rom "$pf"
done

for pf in "${pfs[@]}"; do
	msg "enable VF on PF=$pf"
	echo 1 | sudo -n tee "/sys/bus/pci/devices/$pf/sriov_numvfs" >/dev/null
done
sleep 1

mapfile -t vfs < <(find_fake_devs "$VF_DEVICE")
printf 'VF_LIST=%s\n' "${vfs[*]}"
[ "${#vfs[@]}" -eq "$EXPECT_PFS" ] || {
	echo "FAIL: expected $EXPECT_PFS VFs, found ${#vfs[@]}"
	exit 1
}

for vf in "${vfs[@]}"; do
	assert_class "$vf" "$EXPECT_VF_CLASS"
	assert_no_rom "$vf"
done

for dev in "${pfs[@]}" "${vfs[@]}"; do
	dev_path=/sys/bus/pci/devices/$dev
	driver=$(basename "$(readlink -f "$dev_path/driver" 2>/dev/null)" \
		2>/dev/null || true)
	group=$(basename "$(readlink -f "$dev_path/iommu_group" 2>/dev/null)" \
		2>/dev/null || true)
	echo "DEV=$dev DRIVER=$driver IOMMU_GROUP=$group"
	[ -n "$group" ] || {
		echo "FAIL: $dev has no IOMMU group"
		exit 1
	}
done

for pf in "${pfs[@]}"; do
	echo 0 | sudo -n tee "/sys/bus/pci/devices/$pf/sriov_numvfs" >/dev/null
done
sleep 1

mapfile -t vfs_after < <(find_fake_devs "$VF_DEVICE")
printf 'VF_LIST_AFTER_DISABLE=%s\n' "${vfs_after[*]:-}"
[ "${#vfs_after[@]}" -eq 0 ] || {
	echo "FAIL: VFs still visible after disable"
	exit 1
}

msg "cleanup helper"
LOG=/tmp/fake_pci_multi_pf_final_cleanup.log \
	samples/pci/cleanup_fake_pci_sriov.sh

[ -z "$(find_fake_devs "$PF_DEVICE")$(find_fake_devs "$VF_DEVICE")" ] || {
	echo "FAIL: fake devices remain after cleanup"
	exit 1
}
! grep -q '^fake_pci_sriov ' /proc/modules || {
	echo "FAIL: fake_pci_sriov remains loaded after cleanup"
	exit 1
}

trap - EXIT
msg "log saved to $LOG"
msg PASS
