#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# Best-effort cleanup helper for samples/pci/fake_pci_sriov.ko.
set -euo pipefail

VENDOR=${VENDOR:-0x1d55}
PF_DEVICE=${PF_DEVICE:-0x1000}
VF_DEVICE=${VF_DEVICE:-0x1001}
MODULE_NAME=${MODULE_NAME:-fake_pci_sriov}
RMMOD=${RMMOD:-1}
SUDO=${SUDO:-sudo -n}
LOG=${LOG:-/tmp/fake_pci_sriov_cleanup.log}

: > "$LOG"
exec > >(tee -a "$LOG") 2>&1

msg() { echo "== $* =="; }

sysfs_write() {
	local value=$1 path=$2

	[ -e "$path" ] || return 0
	printf '%s\n' "$value" | $SUDO tee "$path" >/dev/null
}

find_fake_devs() {
	local device=$1 d

	for d in /sys/bus/pci/devices/*; do
		[ "$(cat "$d/vendor" 2>/dev/null || true)" = "$VENDOR" ] || continue
		[ "$(cat "$d/device" 2>/dev/null || true)" = "$device" ] || continue
		basename "$d"
	done | sort
}

unbind_dev() {
	local dev=$1 base=/sys/bus/pci/devices/$1

	if [ -e "$base/driver/unbind" ]; then
		msg "unbind $dev from $(basename "$(readlink -f "$base/driver")")"
		sysfs_write "$dev" "$base/driver/unbind"
	fi

	sysfs_write '' "$base/driver_override"
}

msg "preflight"
$SUDO true

msg "unbind fake VFs"
for vf in $(find_fake_devs "$VF_DEVICE"); do
	unbind_dev "$vf" || true
done

msg "disable SR-IOV on fake PFs"
for pf in $(find_fake_devs "$PF_DEVICE"); do
	sysfs_write 0 "/sys/bus/pci/devices/$pf/sriov_numvfs" || true
done

msg "remove fake PFs"
for pf in $(find_fake_devs "$PF_DEVICE"); do
	unbind_dev "$pf" || true
	sysfs_write 1 "/sys/bus/pci/devices/$pf/remove" || true
done

if [ "$RMMOD" = 1 ] && grep -q "^$MODULE_NAME " /proc/modules; then
	msg "rmmod $MODULE_NAME"
	$SUDO rmmod "$MODULE_NAME"
fi

msg "remaining fake devices"
find_fake_devs "$PF_DEVICE" || true
find_fake_devs "$VF_DEVICE" || true
msg "log saved to $LOG"
msg PASS
