#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# Boot CirrOS with config-drive user_data to test the passed-through fake VF
# from inside the guest.  Bounded by outer timeout and internal QEMU deadline.
set -euo pipefail

MODULE=${MODULE:-samples/pci/fake_pci_sriov.ko}
MODULE_ARGS=${MODULE_ARGS:-vf_serial_class=1}
RELOAD_MODULE=${RELOAD_MODULE:-1}
IMAGE=${IMAGE:-/tmp/cirros-0.6.3-x86_64-disk.img}
IMAGE_URL=${IMAGE_URL:-https://github.com/cirros-dev/cirros/releases/download/0.6.3/cirros-0.6.3-x86_64-disk.img}
VENDOR=${VENDOR:-0x1d55}
PF_DEVICE=${PF_DEVICE:-0x1000}
VF_DEVICE=${VF_DEVICE:-0x1001}
QEMU_DEADLINE=${QEMU_DEADLINE:-150}
WORK=${WORK:-/tmp/cirros-vfio-e2e}
LOG=${LOG:-/tmp/cirros_vfio_userdata_echo.log}

: > "$LOG"
exec > >(tee -a "$LOG") 2>&1
msg(){ echo "== $* =="; }
find_dev(){ local dev=$1 d; for d in /sys/bus/pci/devices/*; do [ "$(cat "$d/vendor" 2>/dev/null || true)" = "$VENDOR" ] && [ "$(cat "$d/device" 2>/dev/null || true)" = "$dev" ] && basename "$d" && return 0; done; return 1; }
PF=""; VF=""
cleanup(){ set +e; msg cleanup; if [ -n "${VF:-}" ] && [ -e "/sys/bus/pci/devices/$VF" ]; then [ -e "/sys/bus/pci/devices/$VF/driver/unbind" ] && echo "$VF" | sudo -n tee "/sys/bus/pci/devices/$VF/driver/unbind" >/dev/null; echo '' | sudo -n tee "/sys/bus/pci/devices/$VF/driver_override" >/dev/null; fi; if [ -n "${PF:-}" ] && [ -e "/sys/bus/pci/devices/$PF/sriov_numvfs" ]; then echo 0 | sudo -n tee "/sys/bus/pci/devices/$PF/sriov_numvfs" >/dev/null; fi; msg "log saved to $LOG"; }
trap cleanup EXIT

cd "$(dirname "$0")/../.."
command -v qemu-system-x86_64; command -v python3; command -v xorriso; sudo -n true
if [ ! -s "$IMAGE" ]; then timeout 180s curl -L --fail --connect-timeout 20 -o "$IMAGE.tmp" "$IMAGE_URL"; mv "$IMAGE.tmp" "$IMAGE"; fi

msg "create config-drive user_data"
rm -rf "$WORK" && mkdir -p "$WORK/cfg/openstack/latest"
cat > "$WORK/cfg/openstack/latest/meta_data.json" <<'JSON'
{"uuid":"fake-pci-e2e-001","hostname":"cirros-fake-pci"}
JSON
cat > "$WORK/cfg/openstack/latest/user_data" <<'UD'
#!/bin/sh
exec </dev/console >/dev/ttyS0 2>&1
echo E2E_BEGIN
PATH=/sbin:/bin:/usr/sbin:/usr/bin
DEV=""
for d in /sys/bus/pci/devices/*; do
  v=$(cat $d/vendor 2>/dev/null || true)
  p=$(cat $d/device 2>/dev/null || true)
  if { [ "$v" = "0x1d55" ] && [ "$p" = "0x1001" ]; } || { [ "$v" = "0x1d0f" ] && [ "$p" = "0x8250" ]; } || { [ "$v" = "0x10a9" ] && [ "$p" = "0x0003" ]; }; then DEV=$d; break; fi
done
echo DEV=$DEV
if [ -n "$DEV" ]; then
  echo VENDOR=$(cat $DEV/vendor) DEVICE=$(cat $DEV/device) CLASS=$(cat $DEV/class)
  echo RESOURCE0=$(head -n1 $DEV/resource)
  BASE=$(awk 'NR==1 {print $1}' $DEV/resource)
  echo BASE=$BASE
  UART_OFFSET=0
  [ "$(cat $DEV/vendor)" = "0x10a9" ] && [ "$(cat $DEV/device)" = "0x0003" ] && UART_OFFSET=0x20178
  UART_BASE=$(printf "0x%x" $((BASE + UART_OFFSET)))
  echo UART_OFFSET=$UART_OFFSET UART_BASE=$UART_BASE
else
  echo NO_FAKE_VF
fi
echo TTY_LIST_BEGIN
ls -l /dev/ttyS* 2>&1 || true
echo TTY_LIST_END
echo DMESG_MATCH_BEGIN
dmesg | grep -Ei '1d55|1001|1d0f|8250|10a9|0003|serial|ttyS' || true
echo DMESG_MATCH_END
if [ -n "$DEV" ] && command -v devmem >/dev/null 2>&1; then
  echo DEVMEM_PRESENT
  # 16550 offsets: THR/RBR=0, LSR=5.  This is a smoke test for trapped MMIO.
  LSR_ADDR=$(printf "0x%x" $((UART_BASE + 5)))
  THR_ADDR=$(printf "0x%x" $((UART_BASE + 0)))
  echo LSR_BEFORE=$(devmem $LSR_ADDR 8 2>&1 || true)
  echo WRITE_THR_A=$(devmem $THR_ADDR 8 0x41 2>&1 || true)
  echo LSR_AFTER=$(devmem $LSR_ADDR 8 2>&1 || true)
  echo RBR_READ=$(devmem $THR_ADDR 8 2>&1 || true)
  echo TTY_ALPHABET_BEGIN
  TTY_PASS=0
  TEST_STR=ABCDEFGHIJKLMNOPQRSTUVWXYZ
  for t in /dev/ttyS32 /dev/ttyS33 /dev/ttyS34 /dev/ttyS35 /dev/ttyS1 /dev/ttyS2 /dev/ttyS3 /dev/ttyS4; do
    [ -e $t ] || continue
    echo TRY_TTY=$t
    if (
      exec 7<>$t || exit 1
      stty -F $t raw -echo -icanon clocal -hupcl min 0 time 20 9600 2>&1 || exit 1
      printf "%s" "$TEST_STR" >&7
      TTY_READ=$(dd bs=1 count=26 <&7 2>/dev/null || true)
      echo TTY_SELECTED=$t
      echo TTY_EXPECT=$TEST_STR
      echo TTY_READ=$TTY_READ
      echo TTY_READ_HEXDUMP_BEGIN
      printf "%s" "$TTY_READ" | hexdump -C || true
      echo TTY_READ_HEXDUMP_END
      [ "$TTY_READ" = "$TEST_STR" ] || exit 1
      exec 7>&-
    ) 2>&1; then
      TTY_PASS=1
      echo TTY_ALPHABET_PASS=$t
      break
    fi
  done
  if [ "$TTY_PASS" != 1 ]; then
    echo TTY_ALPHABET_FAIL
    echo E2E_FAIL
  fi
  echo TTY_ALPHABET_END
else
  echo DEVMEM_MISSING_OR_NO_DEV
fi
echo E2E_END
sync
poweroff -f
UD
chmod 755 "$WORK/cfg/openstack/latest/user_data"
xorriso -as mkisofs -quiet -V config-2 -J -r -o "$WORK/configdrive.iso" "$WORK/cfg"
ls -lh "$WORK/configdrive.iso"

sudo -n modprobe vfio-pci
if grep -q '^fake_pci_sriov ' /proc/modules && [ "$RELOAD_MODULE" = 1 ]; then msg "rmmod fake_pci_sriov"; old_pf=$(find_dev "$PF_DEVICE" || true); [ -n "$old_pf" ] && echo 1 | sudo -n tee "/sys/bus/pci/devices/$old_pf/remove" >/dev/null || true; sudo -n rmmod fake_pci_sriov || true; fi
if ! grep -q '^fake_pci_sriov ' /proc/modules; then msg "insmod $MODULE $MODULE_ARGS"; sudo -n insmod "$MODULE" $MODULE_ARGS; fi
PF=$(find_dev "$PF_DEVICE"); msg "PF=$PF"; echo 0 | sudo -n tee "/sys/bus/pci/devices/$PF/sriov_numvfs" >/dev/null || true; echo 1 | sudo -n tee "/sys/bus/pci/devices/$PF/sriov_numvfs" >/dev/null; sleep 1
VF=$(find_dev "$VF_DEVICE"); msg "VF=$VF"; [ -e /sys/module/vfio_iommu_type1/parameters/allow_unsafe_interrupts ] && echo Y | sudo -n tee /sys/module/vfio_iommu_type1/parameters/allow_unsafe_interrupts >/dev/null
[ -e "/sys/bus/pci/devices/$VF/driver/unbind" ] && echo "$VF" | sudo -n tee "/sys/bus/pci/devices/$VF/driver/unbind" >/dev/null; echo pci_sim_vfio_pci | sudo -n tee "/sys/bus/pci/devices/$VF/driver_override" >/dev/null; echo "$VF" | sudo -n tee /sys/bus/pci/drivers_probe >/dev/null; readlink -f "/sys/bus/pci/devices/$VF/driver"

msg "boot CirrOS with config-drive"
export IMAGE VF QEMU_DEADLINE CONFIG_ISO="$WORK/configdrive.iso"
python3 - <<'PY'
import os, pty, select, subprocess, sys, time
image=os.environ['IMAGE']; vf=os.environ['VF']; iso=os.environ['CONFIG_ISO']; deadline=int(os.environ.get('QEMU_DEADLINE','150'))
cmd=['sudo','-n','qemu-system-x86_64','-nodefaults','-display','none','-serial','stdio','-monitor','none','-machine','q35,accel=kvm','-cpu','host','-smp','1','-m','512M','-snapshot','-drive',f'file={image},if=virtio,format=qcow2','-drive',f'file={iso},if=virtio,media=cdrom,readonly=on,format=raw','-netdev','user,id=n0','-device','virtio-net-pci,netdev=n0','-device',f'vfio-pci,host={vf}','-no-reboot']
print('+ '+' '.join(cmd), flush=True)
master, slave=pty.openpty(); proc=subprocess.Popen(cmd, stdin=slave, stdout=slave, stderr=slave, close_fds=True); os.close(slave)
buf=''; success=False; end=time.monotonic()+deadline
try:
    while time.monotonic()<end:
        r,_,_=select.select([master],[],[],0.5)
        if r:
            try: data=os.read(master,4096)
            except OSError: break
            if not data: break
            s=data.decode(errors='replace'); sys.stdout.write(s); sys.stdout.flush(); buf+=s
            if 'E2E_END' in buf:
                success=True
                break
        if proc.poll() is not None and not r:
            break
finally:
    if proc.poll() is None:
        proc.terminate()
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill(); proc.wait(timeout=5)
    os.close(master)
print(f"\nQEMU_RC={proc.returncode}")
if not success:
    sys.exit('FAIL: E2E_END not seen')
if 'E2E_FAIL' in buf:
    sys.exit('FAIL: guest reported E2E_FAIL')
PY
msg "recent host dmesg"
dmesg | tail -80
msg PASS
