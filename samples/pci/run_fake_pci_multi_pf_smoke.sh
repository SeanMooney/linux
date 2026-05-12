#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# Host-side smoke test for fake_pci_sriov multiple-PF support.
set -euo pipefail

MODULE=${MODULE:-samples/pci/fake_pci_sriov.ko}
MODULE_ARGS=${MODULE_ARGS:-num_pfs=2 vf_serial_class=1}
VENDOR=${VENDOR:-0x1d55}
PF_DEVICE=${PF_DEVICE:-0x1000}
VF_DEVICE=${VF_DEVICE:-0x1001}
EXPECT_PFS=${EXPECT_PFS:-2}
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
	for pf in $(find_fake_devs "$PF_DEVICE"); do
		[ -e "/sys/bus/pci/devices/$pf/sriov_numvfs" ] && \
			echo 0 | sudo -n tee "/sys/bus/pci/devices/$pf/sriov_numvfs" >/dev/null
	done
	for pf in $(find_fake_devs "$PF_DEVICE"); do
		[ -e "/sys/bus/pci/devices/$pf/remove" ] && \
			echo 1 | sudo -n tee "/sys/bus/pci/devices/$pf/remove" >/dev/null
	done
	if grep -q '^fake_pci_sriov ' /proc/modules; then
		sudo -n rmmod fake_pci_sriov || true
	fi
	msg "log saved to $LOG"
}
trap cleanup EXIT

cd "$(dirname "$0")/../.."
sudo -n true

if grep -q '^fake_pci_sriov ' /proc/modules; then
	for pf in $(find_fake_devs "$PF_DEVICE"); do
		echo 0 | sudo -n tee "/sys/bus/pci/devices/$pf/sriov_numvfs" >/dev/null || true
		echo 1 | sudo -n tee "/sys/bus/pci/devices/$pf/remove" >/dev/null || true
	done
	sudo -n rmmod fake_pci_sriov || true
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

for dev in "${pfs[@]}" "${vfs[@]}"; do
	driver=$(basename "$(readlink -f "/sys/bus/pci/devices/$dev/driver" 2>/dev/null)" 2>/dev/null || true)
	group=$(basename "$(readlink -f "/sys/bus/pci/devices/$dev/iommu_group" 2>/dev/null)" 2>/dev/null || true)
	echo "DEV=$dev DRIVER=$driver IOMMU_GROUP=$group"
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

msg PASS
