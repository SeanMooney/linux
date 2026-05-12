# fake_pci_sriov migration support plan

## Goal

Support live-migration testing for Nova/libvirt/QEMU guests that have an
assigned fake SR-IOV VF.  The goal is to exercise the control-plane and VFIO
migration paths with a deterministic software device, not to model a production
SR-IOV NIC.

The fake VF's device state is small: UART registers plus FIFO contents.  A
successful migration should preserve enough state that the guest's assigned VF
continues to function after migration.

## Questions to answer before implementation

1. Which layer needs to be validated first?
   - Nova/libvirt control flow for migratable assigned PCI devices
   - QEMU VFIO migration protocol
   - guest-visible UART state continuity
2. Which libvirt/QEMU capability is required for Nova to consider the assigned
   fake VF migratable?
3. How does the current upstream VFIO PCI core expose migration support for
   variant drivers?
4. Does `vfio_pci_core` already preserve PCI config state we need, or must the
   sample serialize selected config-space state too?
5. What source/destination topology matching is required for Nova/libvirt tests?

## Reference material

- `samples/vfio-mdev/mtty.c`
  - migration file object
  - migration state machine
  - serialized UART data structure
- `drivers/vfio/pci/*`
  - current VFIO PCI core APIs
- `drivers/vfio/pci/mlx5/*`
  - real VFIO PCI migration implementation reference
- `include/linux/vfio.h`
- `include/linux/vfio_pci_core.h`

## State to migrate

At minimum, serialize the UART model:

```c
struct pci_sim_uart_migration_state {
	__le64 magic;
	__le32 version;
	u8 regs[8];
	u8 fifo[PCI_SIM_UART_FIFO_SIZE];
	__le32 head;
	__le32 tail;
	__le32 count;
	u8 dlab;
	u8 overrun;
	__le16 divisor;
	u8 fcr;
	u8 intr_trigger_level;
};
```

Possible additional state:

- selected guest-visible config-space writes not already handled by
  `vfio_pci_core`
- BAR sizing/configuration if needed
- future interrupt state if IRQ support is added

## Implementation phases

### M1: local UART state serialization helpers

Add helpers independent of VFIO migration plumbing:

```c
pci_sim_uart_save_state()
pci_sim_uart_load_state()
```

Validate magic/version/length and FIFO bounds during restore.

### M2: wire VFIO migration API

Implement migration support in `pci_sim_vfio_pci` using the current upstream
VFIO PCI variant-driver hooks.  The implementation should expose save/resume
files and transition the fake VF through the relevant migration states.

Use `mtty` conceptually, but follow current VFIO PCI core examples for exact API
usage.

### M3: QEMU/libvirt validation

- Start source and destination with matching fake PF/VF topology.
- Assign fake VF to guest.
- Verify pre-migration UART loopback.
- Trigger live migration.
- Verify the guest still sees the tty and loopback works after migration.
- Add Nova-level test once the libvirt/QEMU behavior is understood.

## Risks / considerations

- VFIO migration APIs are more complex than the current no-IRQ UART path.
- Nova/libvirt may reject assigned PCI migration unless QEMU exposes the right
  migration capability.
- The destination host must provide a matching fake VF in a compatible PCI/IOMMU
  configuration.
- If we later add interrupts, interrupt-mask/pending state may need migration
  too.

## Success criteria

- QEMU reports the fake VFIO PCI device as migratable.
- A guest with the assigned fake VF can live migrate between two test hosts or
  two controlled test environments.
- UART alphabet loopback passes after migration.
- Optional stronger test: bytes queued before migration can be read after
  migration.
