# fake_pci_sriov multiple-PF support plan

## Goal

Allow one module instance to create multiple independent fake SR-IOV PFs.  This
will let Nova/libvirt tests exercise scheduling and resource-provider behavior
with more than one SR-IOV parent device, while preserving the same fake VFIO UART
payload for guest validation.

Example target usage:

```sh
sudo insmod samples/pci/fake_pci_sriov.ko num_pfs=2 vf_serial_class=1
```

Expected topology:

```text
0001:00:00.0  PF 1d55:1000
0002:00:00.0  PF 1d55:1000
```

Each PF should independently support:

```sh
echo 1 > /sys/bus/pci/devices/<pf>/sriov_numvfs
```

and create VFs in the same PCI domain as the parent PF.

## Current single-PF assumptions

The current implementation has several single-host assumptions:

- global `fake_host`
- static PCI window resources
- config-space access paths that rely on global host state
- module init creates one platform device / host bridge
- tests pick the first matching PF/VF

## Design

Add a module parameter:

```c
static unsigned int num_pfs = 1;
module_param(num_pfs, uint, 0444);
```

with a conservative cap, for example:

```c
#define FAKE_PCI_MAX_HOSTS 16
```

Represent hosts with a list:

```c
static LIST_HEAD(fake_hosts);
static DEFINE_MUTEX(fake_hosts_lock);
```

Extend `struct fake_pci_host` with:

```c
struct list_head list;
struct resource bus_resource;
struct resource mem_resource;
```

Move PCI window resources from file-static globals into each host instance.

## Implementation steps

### Step 1: single-PF refactor

Refactor internals to avoid global host assumptions while still creating one PF:

- derive `struct fake_pci_host *` from `bus->sysdata`
- make config-space read/write paths use the derived host
- make PF probe/remove and `sriov_configure` find their owning host
- move PCI bus/MMIO resources into `struct fake_pci_host`
- keep behavior unchanged with one host

### Step 2: create multiple hosts

- Add `num_pfs` parameter and cap it.
- Allocate one `fake_pci_host` and one platform device per PF.
- Assign conventional domains starting at 1.
- Give each host a non-overlapping MMIO window, e.g. a fixed base plus stride.

### Step 3: update tests

- Add a host-only multi-PF smoke path or script option.
- Teach tests to select a PF by domain/index when multiple PFs exist.
- Verify each PF can create/remove VFs independently.
- Verify at least one selected VF can still pass the cirros alphabet loopback.

## Risks / considerations

- Ensure resource windows do not overlap.
- Ensure IOMMU groups are still distinct per PF/VF.
- Avoid global state in VF enable/disable paths.
- Keep domains conventional 16-bit values for Nova compatibility.
- Tests need deterministic PF/VF selection once multiple identical IDs exist.

## Success criteria

- `num_pfs=1` behaves exactly like today.
- `num_pfs=2` creates two independent fake PFs.
- Enabling VFs on one PF does not affect the other PF.
- Each VF can bind to `pci_sim_vfio_pci`.
- Canonical cirros alphabet loopback passes for a selected VF.
