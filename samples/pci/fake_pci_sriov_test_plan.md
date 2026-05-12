# fake_pci_sriov test-script rationalization plan

## Goal

Keep a small, clear set of scripts for validating the fake SR-IOV VFIO fixture
before adding multiple PF and migration support.

The canonical test should match the primary Nova/libvirt/QEMU use case:

1. load `fake_pci_sriov.ko`
2. enable a VF through `sriov_numvfs`
3. bind the VF to `pci_sim_vfio_pci`
4. assign it to QEMU with `-device vfio-pci,host=$VF`
5. boot cirros
6. verify guest PCI visibility, raw BAR access, and tty-level UART loopback

## Current scripts

- `run_cirros_vfio_userdata_echo.sh`
  - Full config-drive/user-data E2E test.
  - This is the canonical test.
- `run_cirros_vfio_guest_probe.sh`
  - Login-driven guest debugging path.
  - Useful when config-drive/user-data is unavailable or broken.
- `run_cirros_vfio_visibility.sh`
  - Older visibility-only QEMU/cirros flow.
  - Mostly superseded by the user-data echo test.
- `run_fake_pci_qemu_vfio_smoke.sh`
  - Minimal QEMU/VFIO attach smoke test.
  - Useful for checking host VFIO attach without waiting for a full guest boot.

## Desired script set

Keep:

- `run_cirros_vfio_userdata_echo.sh`
  - canonical full E2E test
- `run_cirros_vfio_guest_probe.sh`
  - debug-only interactive guest probe
- `run_fake_pci_qemu_vfio_smoke.sh`
  - quick host/QEMU attach smoke test

Remove:

- `run_cirros_vfio_visibility.sh`
  - redundant with the canonical E2E test

## Cleanup tasks

1. Remove `run_cirros_vfio_visibility.sh`.
2. Make `run_fake_pci_qemu_vfio_smoke.sh` use `pci_sim_vfio_pci`, not generic
   `vfio-pci`, because fake BAR0 access requires the sample VFIO backend.
3. Ensure scripts consistently default to the Nova/QEMU test mode:
   - `MODULE_ARGS='vf_serial_class=1'` for guest-facing scripts
   - `driver_override=pci_sim_vfio_pci` for VFIO assignment
4. Update `README.fake_pci_sriov.md` to describe the final script roles.
5. Re-run at least:
   - module build
   - `run_fake_pci_qemu_vfio_smoke.sh`
   - `run_cirros_vfio_userdata_echo.sh`

## Success criteria

- Working tree contains only the intended script set.
- README clearly identifies `run_cirros_vfio_userdata_echo.sh` as canonical.
- Full cirros alphabet loopback still passes.
- Smoke script reaches expected QEMU timeout/attach success.
