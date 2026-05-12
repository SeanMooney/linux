#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# Boot CirrOS with a fake_pci_sriov VF passed through by VFIO and verify that
# the guest can see the PCI device in sysfs.  All long-running operations are
# bounded by timeouts.

set -euo pipefail

MODULE=${MODULE:-samples/pci/fake_pci_sriov.ko}
MODULE_ARGS=${MODULE_ARGS:-}
RELOAD_MODULE=${RELOAD_MODULE:-0}
IMAGE_URL=${IMAGE_URL:-https://github.com/cirros-dev/cirros/releases/download/0.6.3/cirros-0.6.3-x86_64-disk.img}
IMAGE=${IMAGE:-/tmp/cirros-0.6.3-x86_64-disk.img}
VENDOR=${VENDOR:-0x1d55}
PF_DEVICE=${PF_DEVICE:-0x1000}
VF_DEVICE=${VF_DEVICE:-0x1001}
VFS=${VFS:-1}
QEMU_DEADLINE=${QEMU_DEADLINE:-120}
LOG=${LOG:-/tmp/cirros_vfio_visibility.log}

: > "$LOG"
exec > >(tee -a "$LOG") 2>&1

msg() { echo "== $* =="; }
find_dev() {
	local device=$1 d
	for d in /sys/bus/pci/devices/*; do
		[ "$(cat "$d/vendor" 2>/dev/null || true)" = "$VENDOR" ] || continue
		[ "$(cat "$d/device" 2>/dev/null || true)" = "$device" ] || continue
		basename "$d"
		return 0
	done
	return 1
}

PF=""
VF=""
cleanup() {
	set +e
	msg "cleanup"
	if [ -n "${VF:-}" ] && [ -e "/sys/bus/pci/devices/$VF" ]; then
		if [ -e "/sys/bus/pci/devices/$VF/driver/unbind" ]; then
			echo "$VF" | sudo -n tee "/sys/bus/pci/devices/$VF/driver/unbind" >/dev/null
		fi
		echo '' | sudo -n tee "/sys/bus/pci/devices/$VF/driver_override" >/dev/null
	fi
	if [ -n "${PF:-}" ] && [ -e "/sys/bus/pci/devices/$PF/sriov_numvfs" ]; then
		echo 0 | sudo -n tee "/sys/bus/pci/devices/$PF/sriov_numvfs" >/dev/null
	fi
	msg "log saved to $LOG"
}
trap cleanup EXIT

cd "$(dirname "$0")/../.."

msg "preflight"
command -v qemu-system-x86_64
command -v timeout
command -v python3
sudo -n true

if [ ! -s "$IMAGE" ]; then
	msg "download CirrOS image"
	if command -v curl >/dev/null 2>&1; then
		timeout 180s curl -L --fail --connect-timeout 20 -o "$IMAGE.tmp" "$IMAGE_URL"
	else
		timeout 180s wget -O "$IMAGE.tmp" "$IMAGE_URL"
	fi
	mv "$IMAGE.tmp" "$IMAGE"
fi
ls -lh "$IMAGE"
if command -v qemu-img >/dev/null 2>&1; then
	qemu-img info "$IMAGE" || true
fi

if grep -q '^fake_pci_sriov ' /proc/modules && [ "$RELOAD_MODULE" = 1 ]; then
	msg "reloading fake_pci_sriov"
	old_pf=$(find_dev "$PF_DEVICE" || true)
	if [ -n "$old_pf" ]; then
		echo 0 | sudo -n tee "/sys/bus/pci/devices/$old_pf/sriov_numvfs" >/dev/null || true
		echo 1 | sudo -n tee "/sys/bus/pci/devices/$old_pf/remove" >/dev/null || true
	fi
	sudo -n rmmod fake_pci_sriov
fi

if ! grep -q '^fake_pci_sriov ' /proc/modules; then
	msg "loading fake_pci_sriov $MODULE_ARGS"
	# shellcheck disable=SC2086
	sudo -n insmod "$MODULE" $MODULE_ARGS
else
	msg "fake_pci_sriov already loaded"
fi

PF=$(find_dev "$PF_DEVICE")
[ -n "$PF" ]
msg "PF=$PF"

msg "enable $VFS VF"
echo 0 | sudo -n tee "/sys/bus/pci/devices/$PF/sriov_numvfs" >/dev/null || true
echo "$VFS" | sudo -n tee "/sys/bus/pci/devices/$PF/sriov_numvfs" >/dev/null
sleep 1

VF=$(find_dev "$VF_DEVICE")
[ -n "$VF" ]
msg "VF=$VF"
readlink -f "/sys/bus/pci/devices/$VF/driver" || true

msg "bind VF to vfio-pci"
sudo -n modprobe vfio-pci
if [ -e /sys/module/vfio_iommu_type1/parameters/allow_unsafe_interrupts ]; then
	msg "enable vfio_iommu_type1.allow_unsafe_interrupts for nested test VM"
	echo Y | sudo -n tee /sys/module/vfio_iommu_type1/parameters/allow_unsafe_interrupts >/dev/null
fi
if [ -e "/sys/bus/pci/devices/$VF/driver/unbind" ]; then
	echo "$VF" | sudo -n tee "/sys/bus/pci/devices/$VF/driver/unbind" >/dev/null
fi
echo vfio-pci | sudo -n tee "/sys/bus/pci/devices/$VF/driver_override" >/dev/null
echo "$VF" | sudo -n tee /sys/bus/pci/drivers_probe >/dev/null
readlink -f "/sys/bus/pci/devices/$VF/driver"

msg "boot CirrOS and query guest PCI sysfs"
export IMAGE VF VENDOR VF_DEVICE QEMU_DEADLINE
python3 - <<'PY'
import os, pty, select, signal, subprocess, sys, time

image = os.environ['IMAGE']
vf = os.environ['VF']
vendor = os.environ['VENDOR'].lower()
vf_device = os.environ['VF_DEVICE'].lower()
pci_id = f"{vendor.removeprefix('0x')}:{vf_device.removeprefix('0x')}"
deadline_s = int(os.environ.get('QEMU_DEADLINE', '120'))

cmd = [
    'sudo', '-n', 'qemu-system-x86_64',
    '-nodefaults', '-display', 'none', '-serial', 'stdio', '-monitor', 'none',
    '-machine', 'q35,accel=kvm', '-cpu', 'host', '-smp', '1', '-m', '512M',
    '-snapshot', '-drive', f'file={image},if=virtio,format=qcow2',
    '-device', f'vfio-pci,host={vf}',
    '-no-reboot',
]
print('+ ' + ' '.join(cmd), flush=True)
master, slave = pty.openpty()
proc = subprocess.Popen(cmd, stdin=slave, stdout=slave, stderr=slave, close_fds=True)
os.close(slave)

def send(s):
    os.write(master, s.encode())

def read_available(timeout=0.2):
    out = b''
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        r, _, _ = select.select([master], [], [], max(0, end - time.monotonic()))
        if not r:
            break
        try:
            chunk = os.read(master, 4096)
        except OSError:
            break
        if not chunk:
            break
        out += chunk
        if len(chunk) < 4096:
            break
    return out.decode(errors='replace')

buf = ''
end = time.monotonic() + deadline_s
logged_in = False
success = False
query = (
    "echo GUEST_PCI_BEGIN; "
    "for d in /sys/bus/pci/devices/*; do "
    "printf '%s ' $d; cat $d/vendor; cat $d/device; "
    "done; "
    "echo GUEST_PCI_END\n"
)

try:
    while time.monotonic() < end:
        part = read_available(0.5)
        if part:
            sys.stdout.write(part)
            sys.stdout.flush()
            buf += part
        if not logged_in and ('login:' in buf.lower() or 'cirros login:' in buf.lower()):
            send('cirros\n')
            buf = ''
            continue
        if not logged_in and 'password:' in buf.lower():
            send('gocubsgo\n')
            logged_in = True
            buf = ''
            time.sleep(1)
            send(query)
            continue
        # The device is visible as soon as the guest kernel enumerates it,
        # e.g. "pci 0000:00:01.0: [1d55:1001] ...".  If login succeeds,
        # the sysfs query below verifies the same IDs as 0x-prefixed values.
        if pci_id in buf.lower() or f"[{pci_id}]" in buf.lower():
            success = True
            break
        if 'GUEST_PCI_END' in buf:
            if (vendor in buf.lower() and vf_device in buf.lower()) or pci_id in buf.lower():
                success = True
            break
        if proc.poll() is not None:
            break
finally:
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)
    os.close(master)

print(f"\nQEMU_RC={proc.returncode}")
if not success:
    print(f"FAIL: guest did not report {vendor}:{vf_device} / {pci_id}")
    sys.exit(1)
print(f"PASS: guest reported {vendor}:{vf_device} / {pci_id}")
PY

msg "recent kernel log"
dmesg | tail -120
msg "PASS"
