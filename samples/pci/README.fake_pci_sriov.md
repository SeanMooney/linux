# Fake PCI SR-IOV VFIO test fixture

`fake_pci_sriov.ko` is a self-contained sample module for testing SR-IOV
control-plane flows without physical SR-IOV hardware.  It creates a fake PCI
host bridge, one physical function (PF), and software-created virtual functions
(VFs).  The VFs can be bound to VFIO and assigned to a QEMU guest, making the
sample useful for OpenStack Nova/libvirt/QEMU SR-IOV testing.

The emulated VF payload is intentionally small: a 16550-style UART loopback.
It exists to prove that a VF assigned through VFIO is visible and usable inside
a guest such as CirrOS.

## Architecture

The sample contains four pieces:

- **Fake PCI topology**: a fake host bridge with a PF (`1d55:1000`) and up to
  seven VFs (`1d55:1001`).  VFs are enabled through the PF `sriov_numvfs` sysfs
  attribute.
- **Software IOMMU model**: enough IOMMU/group behavior for VFIO assignment in
  the test environment.
- **VFIO PCI backend**: `pci_sim_vfio_pci`, an override-only VFIO PCI driver for
  fake VFs.  It services trapped BAR0 reads/writes in software instead of
  relying on real host MMIO.
- **UART loopback payload**: writes to the UART transmit register are queued
  into the receive FIFO, allowing tests to write and read back bytes through a
  fake VF.
- **Host tty frontend**: `pci_sim_loopback_vf` can bind to fake VFs on the host
  and expose `/dev/ttyPCI_SIM*` loopback devices for local bring-up without
  QEMU.

Host-visible IDs remain the local experimental IDs.  When
`vfio_guest_8250_compat=1` (the default), VFIO config-space reads presented to
the guest are overlaid with an SGI IOC3 serial identity (`10a9:0003`).  This lets
stock Linux guests bind `8250_pci` and create a `/dev/ttyS*` device without
carrying a guest driver patch for the local fake IDs.

## Design notes

The fake PCI host emulates PCI config space through `pci_ops`, but ordinary BAR
MMIO accesses do not flow through `pci_ops`.  Once the kernel assigns a BAR,
drivers and VFIO treat it as a physical MMIO resource.  That is why fake VFs
need `pci_sim_vfio_pci`: generic `vfio-pci` cannot service reads and writes to a
purely software BAR.

The UART behavior is intentionally minimal and is modeled after the loopback
approach used by `samples/vfio-mdev/mtty.c`: TX writes feed an RX FIFO, `LSR`
reports data-ready/transmitter-empty state, `FCR` can clear FIFOs, and `LCR`
handles the divisor-latch bit.  The model is sufficient for polling-based
`8250_pci` tests; it is not intended to be a complete serial-device emulator.

The SGI IOC3 compatibility identity was chosen because Linux `8250_pci` has an
explicit MMIO, no-IRQ board entry for it.  That avoids requiring an interrupt
path or a guest kernel patch for `1d55:1001` while still proving VFIO assignment
and BAR access from an unmodified guest.

## Build

From the kernel source tree:

```sh
make -C /path/to/build M=$PWD/drivers/vfio/pci modules
make -C /path/to/build \
  M=$PWD/samples/pci \
  KBUILD_EXTRA_SYMBOLS=$PWD/drivers/vfio/pci/Module.symvers \
  modules
```

The sample depends on `VFIO_PCI_CORE`.

## Module parameters

- `vf_serial_class` (bool, default `false`): expose VFs with PCI serial class
  code instead of a vendor-specific class.  Nova/libvirt/QEMU tests usually use
  `vf_serial_class=1`.
- `vfio_guest_8250_compat` (bool, default `true`): expose VFIO-assigned VFs to
  guests as an SGI IOC3/8250-compatible serial device.  Host-visible IDs do not
  change.
- `vfio_uart_trace` (bool, default `false`): log VFIO BAR0 UART register
  accesses for debugging.
- `fake_intx_irq` (int, default `0`): optional fake INTx routing value.  The
  current guest UART test uses the no-IRQ/polling IOC3 path and does not require
  this.

## Manual VFIO flow

```sh
sudo modprobe vfio-pci
sudo insmod samples/pci/fake_pci_sriov.ko vf_serial_class=1

PF=$(basename /sys/bus/pci/devices/* | while read d; do
  [ "$(cat /sys/bus/pci/devices/$d/vendor 2>/dev/null)" = 0x1d55 ] && \
  [ "$(cat /sys/bus/pci/devices/$d/device 2>/dev/null)" = 0x1000 ] && \
  echo $d && break
done)

echo 1 | sudo tee /sys/bus/pci/devices/$PF/sriov_numvfs

VF=$(basename /sys/bus/pci/devices/* | while read d; do
  [ "$(cat /sys/bus/pci/devices/$d/vendor 2>/dev/null)" = 0x1d55 ] && \
  [ "$(cat /sys/bus/pci/devices/$d/device 2>/dev/null)" = 0x1001 ] && \
  echo $d && break
done)

[ -e /sys/bus/pci/devices/$VF/driver/unbind ] && \
  echo $VF | sudo tee /sys/bus/pci/devices/$VF/driver/unbind

echo pci_sim_vfio_pci | sudo tee /sys/bus/pci/devices/$VF/driver_override
echo $VF | sudo tee /sys/bus/pci/drivers_probe

sudo qemu-system-x86_64 ... -device vfio-pci,host=$VF
```

## Host-side tty loopback

For local debugging without QEMU, bind a VF to `pci_sim_loopback_vf` instead of
`pci_sim_vfio_pci`.  The driver creates `/dev/ttyPCI_SIM*` devices with
independent FIFO state per VF.  Bytes written to one VF's tty can be read back
from the same tty.

This path is useful for validating VF creation/removal and UART FIFO lifetime
rules before involving VFIO or a guest.

## Test scripts

The canonical end-to-end test is:

```sh
env IMAGE=/tmp/cirros-0.6.3-x86_64-disk.img \
  MODULE_ARGS='vf_serial_class=1' \
  samples/pci/run_cirros_vfio_userdata_echo.sh
```

It boots CirrOS with the fake VF assigned through VFIO, verifies guest PCI
visibility, verifies raw BAR UART loopback with `devmem`, then writes and reads
back `ABCDEFGHIJKLMNOPQRSTUVWXYZ` through the guest-created tty.

Expected success markers include:

```text
0000:00:02.0: ttyS4 at MMIO ... is a 16450
TTY_READ=ABCDEFGHIJKLMNOPQRSTUVWXYZ
TTY_ALPHABET_PASS=/dev/ttyS4
E2E_END
== PASS ==
```

Additional helper scripts are kept for narrower debugging:

- `run_cirros_vfio_guest_probe.sh`: interactive-login guest probe path.
- `run_cirros_vfio_visibility.sh`: older visibility-only flow.
- `run_fake_pci_qemu_vfio_smoke.sh`: minimal QEMU/VFIO attach smoke test.

## Limitations

- This is a test fixture, not a production hardware model.
- The UART is a loopback payload used to validate assignment and MMIO access.
- No real device DMA workload is implemented.
- The default guest compatibility path uses polling/no IRQ injection.
- APIs used by the fake host bridge, IOMMU, and VFIO backend are kernel-internal;
  this sample currently targets the kernel tree it is built with rather than a
  broad DKMS compatibility matrix.
