# Fake PCI Host Controller with Software IOMMU for SR-IOV Testing

This sample kernel module creates a virtual PCI host controller with SR-IOV
capable devices that can be assigned to VMs via VFIO. It includes a software
IOMMU driver that provides proper IOMMU groups, enabling device passthrough
with libvirt/QEMU/OpenStack Nova.

## Overview

The module creates:
- A fake PCI host bridge on a unique PCI domain
- A Physical Function (PF) device (vendor 0x1b36, device 0x000c)
- Up to 7 Virtual Functions (VFs) via SR-IOV (vendor 0x1b36, device 0x000d)
- A software IOMMU that provides IOMMU groups for the fake devices

## Use Cases

- Testing PCI device passthrough with libvirt/QEMU
- Testing SR-IOV functionality in OpenStack Nova
- VFIO driver development and testing
- PCI subsystem testing without real hardware

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
```
CONFIG_PCI=y
CONFIG_IOMMU_API=y
CONFIG_VFIO=m           (for passthrough testing)
CONFIG_VFIO_PCI=m       (for passthrough testing)
CONFIG_VFIO_IOMMU_TYPE1=m
```

### Compile the Module

```bash
# Build just this module
make M=samples/pci

# Or build as part of the full kernel build
make -j$(nproc)
```

## Loading the Module

```bash
# Load the module
sudo insmod samples/pci/fake_pci_sriov.ko

# Check kernel log for success
dmesg | grep fake_pci
```

Expected output:
```
fake_pci: Initializing fake PCI SR-IOV driver
fake_pci: Software IOMMU registered
fake_pci: Host bridge created on domain XXXX
fake_pci: Module loaded successfully
```

## Verification

### 1. Verify PCI Device Appears

```bash
# List fake PCI devices
lspci -d 1b36:000c -vvv

# Should show a serial controller on a new PCI domain
```

### 2. Verify IOMMU Group Exists

```bash
# List all IOMMU groups
ls -la /sys/kernel/iommu_groups/

# Find the fake device's group
find /sys/kernel/iommu_groups -name "*1b36*" 2>/dev/null

# Or check directly on the device
readlink /sys/bus/pci/devices/0000:00:00.0/iommu_group
```

### 3. Enable Virtual Functions (SR-IOV)

```bash
# Find the PF device (replace XXXX with your domain)
PF_DEV=$(lspci -d 1b36:000c -D | awk '{print $1}')

# Check current VF count
cat /sys/bus/pci/devices/$PF_DEV/sriov_numvfs

# Enable 4 VFs (max 7)
echo 4 | sudo tee /sys/bus/pci/devices/$PF_DEV/sriov_numvfs

# Verify VFs appear
lspci -d 1b36:000d
```

### 4. Bind to vfio-pci Driver

```bash
# Load VFIO modules
sudo modprobe vfio-pci

# Add device ID to vfio-pci
echo "1b36 000d" | sudo tee /sys/bus/pci/drivers/vfio-pci/new_id

# Find a VF device
VF_DEV=$(lspci -d 1b36:000d -D | head -1 | awk '{print $1}')

# Unbind from current driver (if any)
echo $VF_DEV | sudo tee /sys/bus/pci/devices/$VF_DEV/driver/unbind 2>/dev/null || true

# Bind to vfio-pci
echo $VF_DEV | sudo tee /sys/bus/pci/drivers/vfio-pci/bind

# Verify VFIO group exists
ls -la /dev/vfio/
```

### 5. Test with QEMU

```bash
# Get the VFIO group number
IOMMU_GROUP=$(readlink /sys/bus/pci/devices/$VF_DEV/iommu_group | xargs basename)

# Ensure permissions (or run QEMU as root)
sudo chmod 666 /dev/vfio/$IOMMU_GROUP

# Run QEMU with the device
qemu-system-x86_64 \
    -machine q35 \
    -m 2G \
    -device vfio-pci,host=$VF_DEV \
    -nographic
```

## Unloading

```bash
# Disable VFs first
echo 0 | sudo tee /sys/bus/pci/devices/$PF_DEV/sriov_numvfs

# Remove the module
sudo rmmod fake_pci_sriov
```

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                  fake_pci_sriov.ko                      │
│                                                         │
│  ┌─────────────────┐     ┌─────────────────────────┐   │
│  │ Software IOMMU  │     │   Fake PCI Host Bridge  │   │
│  │                 │     │                         │   │
│  │ - probe_device  │     │  - Custom pci_ops       │   │
│  │ - device_group  │     │  - Config space array   │   │
│  │ - domain ops    │     │  - BAR emulation        │   │
│  └────────┬────────┘     │  - SR-IOV capability    │   │
│           │              └────────────┬────────────┘   │
│           │                           │                 │
│           v                           v                 │
│  ┌─────────────────────────────────────────────────┐   │
│  │              Linux Kernel                        │   │
│  │  IOMMU Core  <-->  PCI Core  <-->  VFIO Core    │   │
│  └─────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────┘
                            │
                            v
              ┌─────────────────────────┐
              │       User Space        │
              │  lspci / QEMU / libvirt │
              └─────────────────────────┘
```

## Device Details

| Property | PF Value | VF Value |
|----------|----------|----------|
| Vendor ID | 0x1b36 | 0x1b36 |
| Device ID | 0x000c | 0x000d |
| Class | Serial Controller (0x0700) | Serial Controller (0x0700) |
| BAR0 | 4KB MMIO | 4KB MMIO |
| Max VFs | 7 | N/A |

## Troubleshooting

### Module fails to load

Check kernel log:
```bash
dmesg | tail -50
```

Ensure required kernel options are enabled.

### No IOMMU group

The software IOMMU should create groups automatically. Check:
```bash
dmesg | grep -i iommu
cat /sys/kernel/iommu_groups/*/devices/*
```

### VFIO binding fails

Ensure VFIO modules are loaded:
```bash
lsmod | grep vfio
sudo modprobe vfio-pci
```

### QEMU fails to start

Check VFIO group permissions:
```bash
ls -la /dev/vfio/
```

Run QEMU as root or fix permissions:
```bash
sudo chmod 666 /dev/vfio/<group_number>
```

## License

GPL-2.0
