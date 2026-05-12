#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# Boot cirros with fake_pci_sriov VF via VFIO and run guest-side probe commands.
set -euo pipefail

MODULE=${MODULE:-samples/pci/fake_pci_sriov.ko}
MODULE_ARGS=${MODULE_ARGS:-}
RELOAD_MODULE=${RELOAD_MODULE:-1}
IMAGE=${IMAGE:-/tmp/cirros-0.6.3-x86_64-disk.img}
IMAGE_URL=${IMAGE_URL:-https://github.com/cirros-dev/cirros/releases/download/0.6.3/cirros-0.6.3-x86_64-disk.img}
VENDOR=${VENDOR:-0x1d55}
PF_DEVICE=${PF_DEVICE:-0x1000}
VF_DEVICE=${VF_DEVICE:-0x1001}
LOG=${LOG:-/tmp/cirros_vfio_guest_probe.log}
QEMU_DEADLINE=${QEMU_DEADLINE:-120}

: > "$LOG"
exec > >(tee -a "$LOG") 2>&1
msg(){ echo "== $* =="; }
find_dev(){ local dev=$1 d; for d in /sys/bus/pci/devices/*; do [ "$(cat "$d/vendor" 2>/dev/null || true)" = "$VENDOR" ] && [ "$(cat "$d/device" 2>/dev/null || true)" = "$dev" ] && basename "$d" && return 0; done; return 1; }
PF=""; VF=""
cleanup(){ set +e; msg cleanup; if [ -n "${VF:-}" ] && [ -e "/sys/bus/pci/devices/$VF" ]; then [ -e "/sys/bus/pci/devices/$VF/driver/unbind" ] && echo "$VF" | sudo -n tee "/sys/bus/pci/devices/$VF/driver/unbind" >/dev/null; echo '' | sudo -n tee "/sys/bus/pci/devices/$VF/driver_override" >/dev/null; fi; if [ -n "${PF:-}" ] && [ -e "/sys/bus/pci/devices/$PF/sriov_numvfs" ]; then echo 0 | sudo -n tee "/sys/bus/pci/devices/$PF/sriov_numvfs" >/dev/null; fi; msg "log saved to $LOG"; }
trap cleanup EXIT

cd "$(dirname "$0")/../.."
command -v qemu-system-x86_64; command -v python3; sudo -n true
if [ ! -s "$IMAGE" ]; then timeout 180s curl -L --fail --connect-timeout 20 -o "$IMAGE.tmp" "$IMAGE_URL"; mv "$IMAGE.tmp" "$IMAGE"; fi
sudo -n modprobe vfio-pci
if grep -q '^fake_pci_sriov ' /proc/modules && [ "$RELOAD_MODULE" = 1 ]; then old_pf=$(find_dev "$PF_DEVICE" || true); [ -n "$old_pf" ] && echo 1 | sudo -n tee "/sys/bus/pci/devices/$old_pf/remove" >/dev/null || true; sudo -n rmmod fake_pci_sriov || true; fi
if ! grep -q '^fake_pci_sriov ' /proc/modules; then msg "insmod $MODULE $MODULE_ARGS"; sudo -n insmod "$MODULE" $MODULE_ARGS; fi
PF=$(find_dev "$PF_DEVICE"); msg "PF=$PF"; echo 0 | sudo -n tee "/sys/bus/pci/devices/$PF/sriov_numvfs" >/dev/null || true; echo 1 | sudo -n tee "/sys/bus/pci/devices/$PF/sriov_numvfs" >/dev/null; sleep 1
VF=$(find_dev "$VF_DEVICE"); msg "VF=$VF"; [ -e /sys/module/vfio_iommu_type1/parameters/allow_unsafe_interrupts ] && echo Y | sudo -n tee /sys/module/vfio_iommu_type1/parameters/allow_unsafe_interrupts >/dev/null
[ -e "/sys/bus/pci/devices/$VF/driver/unbind" ] && echo "$VF" | sudo -n tee "/sys/bus/pci/devices/$VF/driver/unbind" >/dev/null; echo pci_sim_vfio_pci | sudo -n tee "/sys/bus/pci/devices/$VF/driver_override" >/dev/null; echo "$VF" | sudo -n tee /sys/bus/pci/drivers_probe >/dev/null; readlink -f "/sys/bus/pci/devices/$VF/driver"

export IMAGE VF QEMU_DEADLINE
python3 - <<'PY'
import os, pty, select, subprocess, sys, time
image=os.environ['IMAGE']; vf=os.environ['VF']; deadline_s=int(os.environ.get('QEMU_DEADLINE','120'))
cmd=['sudo','-n','qemu-system-x86_64','-nodefaults','-display','none','-serial','stdio','-monitor','none','-machine','q35,accel=kvm','-cpu','host','-smp','1','-m','512M','-snapshot','-drive',f'file={image},if=virtio,format=qcow2','-device',f'vfio-pci,host={vf}','-no-reboot']
print('+ '+' '.join(cmd), flush=True)
master, slave=pty.openpty(); proc=subprocess.Popen(cmd, stdin=slave, stdout=slave, stderr=slave, close_fds=True); os.close(slave)
def send(s): os.write(master, s.encode())
def rd(t=0.5):
    out=b''; end=time.monotonic()+t
    while time.monotonic()<end:
        r,_,_=select.select([master],[],[],max(0,end-time.monotonic()))
        if not r: break
        try: c=os.read(master,4096)
        except OSError: break
        if not c: break
        out+=c
        if len(c)<4096: break
    return out.decode(errors='replace')
cmds = r"""
echo GUEST_PROBE_BEGIN
for d in /sys/bus/pci/devices/*; do echo DEV=$d vendor=$(cat $d/vendor) device=$(cat $d/device) class=$(cat $d/class); cat $d/resource 2>/dev/null | head -1; done
ls -l /dev/ttyS* 2>/dev/null || true
dmesg | grep -Ei '1d55|1001|1d0f|8250|serial|ttyS' || true
for t in /dev/ttyS1 /dev/ttyS2 /dev/ttyS3; do [ -e $t ] && echo TRY_TTY=$t && stty -F $t raw -echo 9600 2>&1 && (echo -n hello > $t) 2>&1 && timeout 2 cat $t 2>&1 | hexdump -C; done
echo GUEST_PROBE_END
""" + "\n"
buf=''; logged=False; sent=False; success=False; end=time.monotonic()+deadline_s
try:
    while time.monotonic()<end:
        p=rd(0.5)
        if p: sys.stdout.write(p); sys.stdout.flush(); buf+=p
        low=buf.lower()
        if not logged and 'login:' in low:
            send('cirros\n'); buf=''; continue
        if not logged and 'password:' in low:
            send('gocubsgo\n'); logged=True; buf=''; time.sleep(1); continue
        if logged and not sent and ('$ ' in buf or '# ' in buf):
            send(cmds); sent=True; buf=''; continue
        if 'GUEST_PROBE_END' in buf:
            success=True; break
        if proc.poll() is not None: break
finally:
    if proc.poll() is None:
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill(); proc.wait(timeout=5)
    os.close(master)
print(f"\nQEMU_RC={proc.returncode}")
if not success: sys.exit('FAIL: guest probe did not complete')
PY
msg PASS
