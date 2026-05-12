# Fake PCI Host Controller with Software IOMMU for SR-IOV Testing

This sample kernel module creates a virtual PCI host controller with SR-IOV
capable devices.  It includes a software IOMMU driver that provides separate
IOMMU groups for the fake devices and a host-side VF loopback TTY driver for
basic functional testing.

## Overview

The module creates:

- A fake PCI host bridge on a unique conventional PCI domain
- A Physical Function (PF) device: vendor `0x1d55`, device `0x1000`
- Up to 7 Virtual Functions (VFs): vendor `0x1d55`, device `0x1001`
- A software IOMMU that provides IOMMU groups for the fake devices
- A host-side VF driver that exposes loopback TTYs such as `/dev/ttyPCI_SIM0`

By default, VFs use a vendor-specific class code so the sample loopback driver
binds instead of `8250_pci`.  Use the module parameter `vf_serial_class=1` to
advertise VFs as PCI serial/16550 devices for explicit 8250/VFIO experiments.

## Use Cases

- Testing PCI/SR-IOV enumeration without real hardware
- Testing IOMMU group behavior for fake PFs/VFs
- Testing OpenStack Nova placement/resource-provider handling
- Basic host-side per-VF loopback tests through `/dev/ttyPCI_SIM<N>`
- VFIO binding experiments

Functional guest-visible 8250 loopback through VFIO passthrough is not provided
yet because this fake PCI host only emulates config space, not BAR MMIO side
effects.

## Building

### Kernel Configuration

Enable the module in your kernel config:

```bash
make menuconfig
# Navigate to: Sample kernel code -> Fake PCI Host Controller with SR-IOV
```

Or enable directly:

```bash
echo "CONFIG_SAMPLE_FAKE_PCI_SRIOV=m" >> .config
make olddefconfig
```

Required kernel options:

```text
CONFIG_PCI=y
CONFIG_IOMMU_API=y
CONFIG_TTY=y
CONFIG_VFIO=m           (for passthrough testing)
CONFIG_VFIO_PCI=m       (for passthrough testing)
CONFIG_VFIO_IOMMU_TYPE1=m
```

### Compile the Module

```bash
# Build just this module against the current tree
make M=samples/pci modules

# Or, when using an external/prepared build directory
make -C /lib/modules/$(uname -r)/build M=$PWD/samples/pci modules
```

## Loading the Module

```bash
sudo insmod samples/pci/fake_pci_sriov.ko
```

Optional serial-class mode for experiments:

```bash
sudo insmod samples/pci/fake_pci_sriov.ko vf_serial_class=1
```

Check kernel log for success:

```bash
dmesg | grep fake_pci
```

Expected output includes:

```text
fake_pci: Initializing fake PCI SR-IOV driver
fake_pci: Software IOMMU registered
fake_pci: Host bridge created on domain XXXX
fake_pci: Module loaded successfully
```

## Verification

### 1. Verify PCI Device Appears

```bash
lspci -D -d 1d55:1000 -vvv
```

### 2. Verify IOMMU Group Exists

```bash
PF_DEV=$(lspci -D -d 1d55:1000 | awk '{print $1}')
readlink /sys/bus/pci/devices/$PF_DEV/iommu_group
```

### 3. Enable Virtual Functions (SR-IOV)

```bash
PF_DEV=$(lspci -D -d 1d55:1000 | awk '{print $1}')
cat /sys/bus/pci/devices/$PF_DEV/sriov_numvfs

echo 4 | sudo tee /sys/bus/pci/devices/$PF_DEV/sriov_numvfs
lspci -D -d 1d55:1001
```

### 4. Host-side TTY loopback

When VFs bind to `pci_sim_loopback_vf`, the module creates one TTY per VF:

```bash
ls -l /dev/ttyPCI_SIM*
```

Use raw mode when manually testing to avoid line-discipline transformations:

```bash
sudo python3 - <<'PY'
import os, select, termios, tty
path = '/dev/ttyPCI_SIM0'
msg = b'hello pci sim\n'
fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
old = termios.tcgetattr(fd)
try:
    tty.setraw(fd)
    os.write(fd, msg)
    select.select([fd], [], [], 2)
    got = os.read(fd, len(msg))
    print(got)
finally:
    termios.tcsetattr(fd, termios.TCSANOW, old)
    os.close(fd)
PY
```

## Automated loopback test script

`samples/pci/test_pci_sim_loopback.py` performs a deterministic host-side smoke
test:

- finds the fake PF (`1d55:1000`)
- optionally loads, reloads, or unloads `fake_pci_sriov.ko`
- resets `sriov_numvfs` to `0`, then enables the requested VF count
- waits for `/dev/ttyPCI_SIM<N>` devices
- puts each TTY in raw mode
- writes a per-VF payload and verifies the same bytes are read back
- disables VFs unless `--keep-vfs` is specified

Common invocations:

```bash
# Fresh deterministic test: remove any existing fake PF/module, load, test,
# remove the fake PF, and unload the module.
sudo samples/pci/test_pci_sim_loopback.py --reload --unload --vfs 2

# Load if needed, reuse if already loaded, test, then leave VFs enabled.
sudo samples/pci/test_pci_sim_loopback.py --load --keep-vfs --vfs 2

# Reuse an already-loaded module, reset VFs, test, disable VFs afterward.
sudo samples/pci/test_pci_sim_loopback.py --vfs 2
```

Option behavior:

- `--load` / `--insmod`: load the module if needed; reuse it if already loaded.
- `--reload`: remove the fake PF if present, unload any existing module, then
  insert the requested module path.
- `--unload` / `--rmmod`: remove the fake PF and unload the module after the
  test.
- `--keep-vfs`: leave VFs enabled after the test.
- `--module PATH`: module path used by `--load` or `--reload`; defaults to
  `samples/pci/fake_pci_sriov.ko`.

Example successful output:

```text
+ insmod samples/pci/fake_pci_sriov.ko
PF: 0001:00:00.0
ok: /dev/ttyPCI_SIM0 echoed b'pci-sim-vf0-hello\n'
ok: /dev/ttyPCI_SIM1 echoed b'pci-sim-vf1-hello\n'
+ rmmod fake_pci_sriov
PASS
```

## VFIO binding experiment

```bash
sudo modprobe vfio-pci
echo "1d55 1001" | sudo tee /sys/bus/pci/drivers/vfio-pci/new_id

VF_DEV=$(lspci -D -d 1d55:1001 | head -1 | awk '{print $1}')
[ -e /sys/bus/pci/devices/$VF_DEV/driver/unbind ] && \
    echo $VF_DEV | sudo tee /sys/bus/pci/devices/$VF_DEV/driver/unbind

echo $VF_DEV | sudo tee /sys/bus/pci/drivers/vfio-pci/bind
readlink /sys/bus/pci/devices/$VF_DEV/iommu_group
ls -l /dev/vfio/
```

This verifies enumeration and group behavior.  Functional guest-visible UART
loopback is not expected until BAR MMIO behavior is implemented in an mdev/VFIO
or QEMU-facing model.

## Unloading

The fake PF may hold a module reference while it is present.  For deterministic
manual cleanup, remove the fake PF first, then unload the module:

```bash
PF_DEV=$(lspci -D -d 1d55:1000 | awk '{print $1}')
[ -n "$PF_DEV" ] && echo 0 | sudo tee /sys/bus/pci/devices/$PF_DEV/sriov_numvfs
[ -n "$PF_DEV" ] && echo 1 | sudo tee /sys/bus/pci/devices/$PF_DEV/remove
sudo rmmod fake_pci_sriov
```

Or use the test script:

```bash
sudo samples/pci/test_pci_sim_loopback.py --reload --unload --vfs 2
```

## Device Details

| Property | PF Value | VF Value |
|----------|----------|----------|
| Vendor ID | `0x1d55` | `0x1d55` |
| Device ID | `0x1000` | `0x1001` |
| Default class | Serial controller (`0x070002`) | Vendor-specific (`0xff0000`) |
| Optional VF class | N/A | Serial controller (`0x070002`) with `vf_serial_class=1` |
| BAR0 | 4 KiB MMIO resource | 4 KiB MMIO resource |
| Max VFs | 7 | N/A |
| Host loopback device | N/A | `/dev/ttyPCI_SIM<N>` |

## Architecture

```text
┌──────────────────────────────────────────────────────────────┐
│                    fake_pci_sriov.ko                         │
│                                                              │
│  ┌─────────────────┐     ┌──────────────────────────────┐   │
│  │ Software IOMMU  │     │    Fake PCI Host Bridge      │   │
│  │                 │     │                              │   │
│  │ - probe_device  │     │  - Custom pci_ops            │   │
│  │ - device_group  │     │  - Config space arrays       │   │
│  │ - domain ops    │     │  - SR-IOV capability         │   │
│  └────────┬────────┘     └──────────────┬───────────────┘   │
│           │                             │                   │
│           │                             v                   │
│           │              ┌──────────────────────────────┐   │
│           │              │ Host VF loopback TTY driver  │   │
│           │              │ - pci_sim_loopback_vf        │   │
│           │              │ - /dev/ttyPCI_SIM<N>         │   │
│           │              │ - mtty-like FIFO backend     │   │
│           │              └──────────────────────────────┘   │
└───────────┼─────────────────────────────┼───────────────────┘
            v                             v
  ┌─────────────────────┐       ┌─────────────────────────┐
  │ Linux PCI/IOMMU core│       │ User space tests/tools  │
  └─────────────────────┘       └─────────────────────────┘
```

## Troubleshooting

### Module fails to load

Check kernel log:

```bash
dmesg | tail -50
```

Ensure required kernel options are enabled.

### `rmmod` reports the module is in use

Remove the fake PF first:

```bash
PF_DEV=$(lspci -D -d 1d55:1000 | awk '{print $1}')
echo 1 | sudo tee /sys/bus/pci/devices/$PF_DEV/remove
sudo rmmod fake_pci_sriov
```

### TTY devices do not appear

Check that VFs are enabled and bound to the sample VF driver:

```bash
lspci -D -d 1d55:1001 -k
ls -l /dev/ttyPCI_SIM*
dmesg | grep pci_sim_loopback_vf
```

If the module was loaded with `vf_serial_class=1`, another serial driver may
bind first.  Use the default class mode for host loopback tests.

### No IOMMU group

The software IOMMU should create groups automatically. Check:

```bash
dmesg | grep -i iommu
find /sys/kernel/iommu_groups -maxdepth 2 -type l -name '0001:*' -print
```

### VFIO binding fails

Ensure VFIO modules are loaded:

```bash
lsmod | grep vfio
sudo modprobe vfio-pci
```

## License

GPL-2.0
