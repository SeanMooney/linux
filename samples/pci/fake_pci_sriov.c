// SPDX-License-Identifier: GPL-2.0
/*
 * Fake PCI SR-IOV VFIO test fixture
 *
 * This sample creates a fake PCI host bridge with one physical function (PF)
 * and software-created virtual functions (VFs).  It is intended for SR-IOV
 * control-plane testing, especially libvirt/QEMU device assignment flows used
 * by OpenStack Nova, on systems without physical SR-IOV hardware.
 *
 * The VFs can be bound to the override-only pci_sim_vfio_pci driver below and
 * assigned to a guest with QEMU's vfio-pci device.  The VFIO backend emulates
 * BAR0 accesses in software and exposes a small 16550-style UART loopback
 * payload so a guest can prove that the assigned VF is usable.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/pci_regs.h>
#include <linux/platform_device.h>
#include <linux/iommu.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/idr.h>
#include <linux/xarray.h>
#include <linux/serial_reg.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/uaccess.h>
#include <linux/vfio_pci_core.h>
#include <linux/string.h>
#include <asm/pci.h>

/*
 * Host-visible device identification.  These local experimental IDs are what
 * host tools such as Nova/libvirt see when discovering the fake PF/VFs.
 */
#define FAKE_PCI_VENDOR_ID	0x1d55
#define FAKE_PCI_PF_DEVICE_ID	0x1000	/* PF device ID */
#define FAKE_PCI_VF_DEVICE_ID	0x1001	/* VF device ID */
#define FAKE_PCI_CLASS		0x070002 /* Serial controller, 16550 */
#define FAKE_PCI_SUBSYS_VENDOR	FAKE_PCI_VENDOR_ID
#define FAKE_PCI_SUBSYS_ID	FAKE_PCI_PF_DEVICE_ID

/* SR-IOV configuration */
#define MAX_VFS			7
#define SRIOV_CAP_OFFSET	0x100	/* Extended capability offset */
#define PCIE_CAP_OFFSET		0x40	/* PCIe capability offset */

/* BAR configuration */
#define BAR0_SIZE		0x1000	/* 4KB host-visible MMIO region */
#define PCI_SIM_VFIO_BAR0_SIZE	0x40000	/* Guest SGI IOC3 window size */
#define PCI_SIM_SGI_IOC3_UART_OFFSET 0x20178

/* Host-side VF TTY loopback configuration */
#define PCI_SIM_TTY_NAME	"ttyPCI_SIM"
#define PCI_SIM_MAX_TTYS	256
#define PCI_SIM_UART_FIFO_SIZE	4096
#define PCI_SIM_UART_CHUNK	256

static bool vf_serial_class;
module_param(vf_serial_class, bool, 0644);
MODULE_PARM_DESC(vf_serial_class,
		 "Expose host-visible VFs as PCI serial/16550 class devices instead of vendor-specific");

static bool vfio_guest_8250_compat = true;
module_param(vfio_guest_8250_compat, bool, 0644);
MODULE_PARM_DESC(vfio_guest_8250_compat,
		 "Overlay VFIO guest config space with an 8250_pci-compatible SGI IOC3 serial identity");

static bool vfio_uart_trace;
module_param(vfio_uart_trace, bool, 0644);
MODULE_PARM_DESC(vfio_uart_trace,
		 "Trace VFIO BAR0 UART register accesses");

static int fake_intx_irq;
module_param(fake_intx_irq, int, 0644);
MODULE_PARM_DESC(fake_intx_irq,
		 "Optional host IRQ number to report for fake PCI INTx routing (0 disables)");

/* Forward declarations */
static struct fake_pci_host *fake_host;

/*
 * ============================================================================
 * Data Structures
 * ============================================================================
 */

struct fake_pci_device {
	u8 config_space[4096];	/* Full PCIe extended config space */
	u32 bar0_saved;		/* Saved BAR0 value for sizing */
	bool present;
	bool is_vf;
	int vf_index;		/* VF index (0-6) for VFs */
};

struct fake_pci_host {
	struct pci_host_bridge *bridge;
	struct platform_device *pdev;
	struct pci_sysdata sysdata;
	struct fake_pci_device pf;
	struct fake_pci_device vfs[MAX_VFS];
	int num_vfs_enabled;
	int domain_nr;
	struct mutex lock;	/* Protects VF enable/disable */
};

struct pci_sim_uart {
	spinlock_t lock;
	u8 regs[8];
	u8 fifo[PCI_SIM_UART_FIFO_SIZE];
	unsigned int head;
	unsigned int tail;
	unsigned int count;
	bool dlab;
	bool overrun;
	u16 divisor;
	u8 fcr;
	u8 intr_trigger_level;
};

struct pci_sim_vf_tty {
	struct pci_dev *pdev;
	struct tty_port port;
	struct mutex state_lock;
	struct pci_sim_uart uart;
	int id;
	bool dead;
};

struct pci_sim_vfio_vf {
	struct vfio_pci_core_device core;
	struct mutex lock;
	struct pci_sim_uart uart;
};

/*
 * ============================================================================
 * Software IOMMU Driver
 * ============================================================================
 */

static struct iommu_device fake_iommu_dev;
static struct platform_device *fake_iommu_pdev;

static DEFINE_IDR(pci_sim_tty_idr);
static DEFINE_MUTEX(pci_sim_tty_idr_lock);
static struct tty_driver *pci_sim_tty_driver;

/* Static resources for PCI bus (must not be stack-allocated). */
static struct resource fake_pci_bus_resource = {
	.start	= 0,
	.end	= 0,
	.flags	= IORESOURCE_BUS,
	.name	= "fake_pci_bus",
};

static struct resource fake_pci_mem_resource = {
	/*
	 * Keep the fake PCI MMIO window out of System RAM.  VFIO reserves BARs
	 * with request_mem_region() when servicing trapped BAR accesses; placing
	 * this window inside RAM makes that reservation fail with -EBUSY.
	 */
	.start	= 0xd0000000,
	.end	= 0xd00fffff,
	.flags	= IORESOURCE_MEM,
	.name	= "fake_pci_mem",
};

struct fake_iommu_domain {
	struct iommu_domain domain;
	struct xarray mappings;
};

static struct fake_iommu_domain *to_fake_domain(struct iommu_domain *dom)
{
	return container_of(dom, struct fake_iommu_domain, domain);
}

static int fake_domain_attach(struct iommu_domain *domain, struct device *dev,
			      struct iommu_domain *old)
{
	/* Software IOMMU - no actual DMA remapping, just accept the attach */
	dev_dbg(dev, "fake_iommu: attached to domain type %u\n", domain->type);
	return 0;
}

static const struct iommu_domain_ops fake_blocking_domain_ops = {
	.attach_dev = fake_domain_attach,
};

static struct iommu_domain fake_blocking_domain = {
	.type = IOMMU_DOMAIN_BLOCKED,
	.ops = &fake_blocking_domain_ops,
};

static void fake_domain_free_paging(struct iommu_domain *domain)
{
	struct fake_iommu_domain *fake_dom = to_fake_domain(domain);

	xa_destroy(&fake_dom->mappings);
	kfree(fake_dom);
}

static int fake_domain_map_pages(struct iommu_domain *domain,
				 unsigned long iova, phys_addr_t paddr,
				 size_t pgsize, size_t pgcount, int prot,
				 gfp_t gfp, size_t *mapped)
{
	struct fake_iommu_domain *fake_dom = to_fake_domain(domain);
	unsigned long iova_pfn = iova >> PAGE_SHIFT;
	unsigned long pfn = paddr >> PAGE_SHIFT;
	size_t total = pgsize * pgcount;
	size_t npages = total >> PAGE_SHIFT;
	size_t i;
	int ret;

	*mapped = 0;

	if (!IS_ALIGNED(iova, PAGE_SIZE) || !IS_ALIGNED(paddr, PAGE_SIZE) ||
	    !IS_ALIGNED(total, PAGE_SIZE))
		return -EINVAL;

	for (i = 0; i < npages; i++) {
		if (xa_load(&fake_dom->mappings, iova_pfn + i)) {
			ret = -EBUSY;
			goto err_unmap;
		}

		ret = xa_err(xa_store(&fake_dom->mappings, iova_pfn + i,
					 xa_mk_value(pfn + i), gfp));
		if (ret)
			goto err_unmap;

		*mapped += PAGE_SIZE;
	}

	return 0;

err_unmap:
	while (i--)
		xa_erase(&fake_dom->mappings, iova_pfn + i);
	*mapped = 0;
	return ret;
}

static size_t fake_domain_unmap_pages(struct iommu_domain *domain,
				      unsigned long iova, size_t pgsize,
				      size_t pgcount,
				      struct iommu_iotlb_gather *gather)
{
	struct fake_iommu_domain *fake_dom = to_fake_domain(domain);
	unsigned long iova_pfn = iova >> PAGE_SHIFT;
	size_t total = pgsize * pgcount;
	size_t npages = total >> PAGE_SHIFT;
	size_t unmapped = 0;

	if (!IS_ALIGNED(iova, PAGE_SIZE) || !IS_ALIGNED(total, PAGE_SIZE))
		return 0;

	while (unmapped < npages) {
		if (!xa_erase(&fake_dom->mappings, iova_pfn + unmapped))
			break;
		unmapped++;
	}

	return unmapped << PAGE_SHIFT;
}

static phys_addr_t fake_domain_iova_to_phys(struct iommu_domain *domain,
					    dma_addr_t iova)
{
	struct fake_iommu_domain *fake_dom = to_fake_domain(domain);
	void *entry;

	entry = xa_load(&fake_dom->mappings, iova >> PAGE_SHIFT);
	if (!entry || !xa_is_value(entry))
		return 0;

	return ((phys_addr_t)xa_to_value(entry) << PAGE_SHIFT) |
	       (iova & ~PAGE_MASK);
}

static const struct iommu_domain_ops fake_paging_domain_ops = {
	.attach_dev	= fake_domain_attach,
	.free		= fake_domain_free_paging,
	.map_pages	= fake_domain_map_pages,
	.unmap_pages	= fake_domain_unmap_pages,
	.iova_to_phys	= fake_domain_iova_to_phys,
};

static struct iommu_domain *
fake_domain_alloc_paging_flags(struct device *dev, u32 flags,
			       const struct iommu_user_data *user_data)
{
	struct fake_iommu_domain *fake_dom;

	if (flags & ~IOMMU_HWPT_ALLOC_PASID)
		return ERR_PTR(-EOPNOTSUPP);
	if (user_data)
		return ERR_PTR(-EOPNOTSUPP);

	fake_dom = kzalloc(sizeof(*fake_dom), GFP_KERNEL);
	if (!fake_dom)
		return ERR_PTR(-ENOMEM);

	fake_dom->domain.type = IOMMU_DOMAIN_UNMANAGED;
	fake_dom->domain.ops = &fake_paging_domain_ops;
	fake_dom->domain.pgsize_bitmap = PAGE_SIZE;
	fake_dom->domain.geometry.aperture_start = 0;
	fake_dom->domain.geometry.aperture_end = ~0UL;
	fake_dom->domain.geometry.force_aperture = true;
	xa_init(&fake_dom->mappings);

	return &fake_dom->domain;
}

static bool fake_iommu_capable(struct device *dev, enum iommu_cap cap)
{
	switch (cap) {
	case IOMMU_CAP_CACHE_COHERENCY:
		return true;
	default:
		return false;
	}
}

static struct iommu_device *fake_iommu_probe_device(struct device *dev)
{
	struct pci_dev *pdev;

	/* Only claim PCI devices */
	if (!dev_is_pci(dev))
		return ERR_PTR(-ENODEV);

	pdev = to_pci_dev(dev);

	/* Only claim devices on our fake PCI domain */
	if (!fake_host || pci_domain_nr(pdev->bus) != fake_host->domain_nr)
		return ERR_PTR(-ENODEV);

	dev_info(dev, "fake_iommu: probed device %04x:%02x:%02x.%d\n",
		 pci_domain_nr(pdev->bus), pdev->bus->number,
		 PCI_SLOT(pdev->devfn), PCI_FUNC(pdev->devfn));

	return &fake_iommu_dev;
}

static void fake_iommu_release_device(struct device *dev)
{
	dev_dbg(dev, "fake_iommu: released device\n");
}

static struct iommu_group *fake_iommu_device_group(struct device *dev)
{
	/* Each device gets its own IOMMU group for maximum flexibility */
	return generic_device_group(dev);
}

static const struct iommu_ops fake_iommu_ops = {
	.owner				= THIS_MODULE,
	.default_domain			= &fake_blocking_domain,
	.blocked_domain			= &fake_blocking_domain,
	.capable			= fake_iommu_capable,
	.probe_device			= fake_iommu_probe_device,
	.release_device			= fake_iommu_release_device,
	.device_group			= fake_iommu_device_group,
	.domain_alloc_paging_flags	= fake_domain_alloc_paging_flags,
};

/*
 * ============================================================================
 * PCI Config Space Initialization
 * ============================================================================
 */

static void fake_pci_set_class(u8 *config, u32 class)
{
	config[PCI_CLASS_PROG] = class & 0xff;
	config[PCI_CLASS_DEVICE] = (class >> 8) & 0xff;
	config[PCI_CLASS_DEVICE + 1] = (class >> 16) & 0xff;
}

static void init_pcie_capability(u8 *config, bool is_pf)
{
	u8 *cap = &config[PCIE_CAP_OFFSET];

	/* Capability ID */
	cap[PCI_CAP_LIST_ID] = PCI_CAP_ID_EXP;
	/* Next capability - SR-IOV for PF, none for VF */
	cap[PCI_CAP_LIST_NEXT] = 0;

	/* PCIe Capabilities Register: version in bits 3:0, type in bits 7:4. */
	*(u16 *)&cap[PCI_EXP_FLAGS] = 2 |
				      (PCI_EXP_TYPE_ENDPOINT << 4);

	/* Device Capabilities */
	*(u32 *)&cap[PCI_EXP_DEVCAP] = PCI_EXP_DEVCAP_FLR;

	/* Device Control - nothing enabled */
	*(u16 *)&cap[PCI_EXP_DEVCTL] = 0;

	/* Device Status */
	*(u16 *)&cap[PCI_EXP_DEVSTA] = 0;

	/* Link Capabilities - Gen1 x1 */
	*(u32 *)&cap[PCI_EXP_LNKCAP] = PCI_EXP_LNKCAP_SLS_2_5GB |
				       (1 << 4); /* x1 width */

	/* Link Status - Gen1 x1 */
	*(u16 *)&cap[PCI_EXP_LNKSTA] = PCI_EXP_LNKSTA_CLS_2_5GB |
				       PCI_EXP_LNKSTA_NLW_X1;
}

static void init_sriov_capability(u8 *config)
{
	u8 *cap = &config[SRIOV_CAP_OFFSET];

	/* Extended Capability Header */
	*(u16 *)&cap[0] = PCI_EXT_CAP_ID_SRIOV;
	*(u16 *)&cap[2] = 0;	/* Next cap offset (none) */

	/* SR-IOV Capabilities */
	*(u32 *)&cap[PCI_SRIOV_CAP] = 0;

	/* SR-IOV Control - initially disabled */
	*(u16 *)&cap[PCI_SRIOV_CTRL] = 0;

	/* SR-IOV Status */
	*(u16 *)&cap[PCI_SRIOV_STATUS] = 0;

	/* Initial VFs */
	*(u16 *)&cap[PCI_SRIOV_INITIAL_VF] = MAX_VFS;

	/* Total VFs */
	*(u16 *)&cap[PCI_SRIOV_TOTAL_VF] = MAX_VFS;

	/* Num VFs - start with 0 */
	*(u16 *)&cap[PCI_SRIOV_NUM_VF] = 0;

	/* Function Dependency Link */
	*(u16 *)&cap[PCI_SRIOV_FUNC_LINK] = 0;

	/* First VF Offset - VF0 at devfn PF+1 */
	*(u16 *)&cap[PCI_SRIOV_VF_OFFSET] = 1;

	/* VF Stride - consecutive devfns */
	*(u16 *)&cap[PCI_SRIOV_VF_STRIDE] = 1;

	/* VF Device ID */
	*(u16 *)&cap[PCI_SRIOV_VF_DID] = FAKE_PCI_VF_DEVICE_ID;

	/* Supported Page Sizes - 4KB */
	*(u32 *)&cap[PCI_SRIOV_SUP_PGSIZE] = 1;

	/* System Page Size - 4KB */
	*(u32 *)&cap[PCI_SRIOV_SYS_PGSIZE] = 1;

	/* VF BAR0 - 4KB MMIO */
	*(u32 *)&cap[PCI_SRIOV_BAR] = PCI_BASE_ADDRESS_MEM_TYPE_32;
}

static void init_pf_config_space(struct fake_pci_device *dev)
{
	u8 *config = dev->config_space;

	memset(config, 0, sizeof(dev->config_space));
	dev->present = true;
	dev->is_vf = false;

	/* Vendor ID and Device ID */
	*(u16 *)&config[PCI_VENDOR_ID] = FAKE_PCI_VENDOR_ID;
	*(u16 *)&config[PCI_DEVICE_ID] = FAKE_PCI_PF_DEVICE_ID;

	/* Command - respond to memory and I/O, allow bus mastering */
	*(u16 *)&config[PCI_COMMAND] = PCI_COMMAND_IO | PCI_COMMAND_MEMORY |
				       PCI_COMMAND_MASTER;

	/* Status - capabilities list */
	*(u16 *)&config[PCI_STATUS] = PCI_STATUS_CAP_LIST;

	/* Revision ID */
	config[PCI_REVISION_ID] = 0x01;

	/* Class code - Serial controller */
	fake_pci_set_class(config, FAKE_PCI_CLASS);

	/* Cache line size */
	config[PCI_CACHE_LINE_SIZE] = 64 / 4;

	/* Header type - normal multi-function device (PF + VFs) */
	config[PCI_HEADER_TYPE] = PCI_HEADER_TYPE_NORMAL | PCI_HEADER_TYPE_MFD;

	/* BAR0 - 4KB 32-bit non-prefetchable MMIO */
	*(u32 *)&config[PCI_BASE_ADDRESS_0] = PCI_BASE_ADDRESS_MEM_TYPE_32;
	dev->bar0_saved = 0;

	/* Subsystem Vendor ID and Subsystem ID */
	*(u16 *)&config[PCI_SUBSYSTEM_VENDOR_ID] = FAKE_PCI_SUBSYS_VENDOR;
	*(u16 *)&config[PCI_SUBSYSTEM_ID] = FAKE_PCI_SUBSYS_ID;

	/* Capabilities pointer */
	config[PCI_CAPABILITY_LIST] = PCIE_CAP_OFFSET;

	/* Interrupt line and pin */
	config[PCI_INTERRUPT_LINE] = 0;
	config[PCI_INTERRUPT_PIN] = 1;	/* INTA# */

	/* Initialize PCIe capability */
	init_pcie_capability(config, true);

	/* Initialize SR-IOV capability */
	init_sriov_capability(config);
}

static void init_vf_config_space(struct fake_pci_device *dev, int vf_index)
{
	u8 *config = dev->config_space;

	memset(config, 0, sizeof(dev->config_space));
	dev->present = true;
	dev->is_vf = true;
	dev->vf_index = vf_index;

	/* Vendor ID and Device ID */
	*(u16 *)&config[PCI_VENDOR_ID] = FAKE_PCI_VENDOR_ID;
	*(u16 *)&config[PCI_DEVICE_ID] = FAKE_PCI_VF_DEVICE_ID;

	/* Command */
	*(u16 *)&config[PCI_COMMAND] = PCI_COMMAND_IO | PCI_COMMAND_MEMORY |
				       PCI_COMMAND_MASTER;

	/* Status - capabilities list */
	*(u16 *)&config[PCI_STATUS] = PCI_STATUS_CAP_LIST;

	/* Revision ID */
	config[PCI_REVISION_ID] = 0x01;

	/*
	 * Default VFs to vendor-specific class so 8250_pci does not bind before
	 * the host-side sample loopback driver.  Set vf_serial_class=1 when
	 * intentionally experimenting with 8250/VFIO guest-visible behavior.
	 */
	fake_pci_set_class(config, vf_serial_class ? FAKE_PCI_CLASS : 0xff0000);

	/* Cache line size */
	config[PCI_CACHE_LINE_SIZE] = 64 / 4;

	/* Header type - normal device */
	config[PCI_HEADER_TYPE] = PCI_HEADER_TYPE_NORMAL;

	/* BAR0 - 4KB 32-bit non-prefetchable MMIO */
	*(u32 *)&config[PCI_BASE_ADDRESS_0] = PCI_BASE_ADDRESS_MEM_TYPE_32;
	dev->bar0_saved = 0;

	/* Subsystem Vendor ID and Subsystem ID */
	*(u16 *)&config[PCI_SUBSYSTEM_VENDOR_ID] = FAKE_PCI_SUBSYS_VENDOR;
	*(u16 *)&config[PCI_SUBSYSTEM_ID] = FAKE_PCI_SUBSYS_ID;

	/* Capabilities pointer */
	config[PCI_CAPABILITY_LIST] = PCIE_CAP_OFFSET;

	/* Initialize PCIe capability (VF version) */
	init_pcie_capability(config, false);
}

/*
 * ============================================================================
 * PCI Config Space Access (pci_ops)
 * ============================================================================
 */

static struct fake_pci_device *get_fake_device(struct fake_pci_host *host,
					       unsigned int devfn)
{
	int slot = PCI_SLOT(devfn);
	int func = PCI_FUNC(devfn);

	/* Only slot 0 has devices */
	if (slot != 0)
		return NULL;

	/* Function 0 is the PF */
	if (func == 0)
		return &host->pf;

	/* Functions 1-7 are VFs (if enabled) */
	if (func >= 1 && func <= MAX_VFS) {
		int vf_idx = func - 1;

		if (vf_idx < host->num_vfs_enabled)
			return &host->vfs[vf_idx];
	}

	return NULL;
}

static int fake_pci_read_config(struct pci_bus *bus, unsigned int devfn,
				int where, int size, u32 *val)
{
	struct fake_pci_host *host = fake_host;
	struct fake_pci_device *dev;

	/* Only bus 0 has devices */
	if (bus->number != 0) {
		*val = ~0;
		return PCIBIOS_DEVICE_NOT_FOUND;
	}

	dev = get_fake_device(host, devfn);
	if (!dev || !dev->present) {
		*val = ~0;
		return PCIBIOS_DEVICE_NOT_FOUND;
	}

	if (where + size > sizeof(dev->config_space)) {
		*val = ~0;
		return PCIBIOS_BAD_REGISTER_NUMBER;
	}

	*val = 0;
	memcpy(val, &dev->config_space[where], size);

	return PCIBIOS_SUCCESSFUL;
}

static void handle_sriov_numvfs_write(struct fake_pci_host *host, u16 num_vfs);

static bool fake_pci_is_sriov_cfg(int where)
{
	return where >= SRIOV_CAP_OFFSET &&
	       where < SRIOV_CAP_OFFSET + PCI_SRIOV_BAR +
		       PCI_SRIOV_NUM_BARS * 4;
}

static int fake_pci_write_config(struct pci_bus *bus, unsigned int devfn,
				 int where, int size, u32 val)
{
	struct fake_pci_host *host = fake_host;
	struct fake_pci_device *dev;
	u8 *config;

	if (bus->number != 0)
		return PCIBIOS_DEVICE_NOT_FOUND;

	dev = get_fake_device(host, devfn);
	if (!dev || !dev->present)
		return PCIBIOS_DEVICE_NOT_FOUND;

	if (where + size > sizeof(dev->config_space))
		return PCIBIOS_BAD_REGISTER_NUMBER;

	config = dev->config_space;

	/* Handle BAR sizing */
	if (where >= PCI_BASE_ADDRESS_0 && where < PCI_BASE_ADDRESS_5 + 4) {
		int bar_offset = where - PCI_BASE_ADDRESS_0;
		int bar_num = bar_offset / 4;

		if (bar_num == 0) {
			if (val == 0xFFFFFFFF) {
				/* BAR sizing - save current value and write size mask */
				dev->bar0_saved = *(u32 *)&config[where];
				*(u32 *)&config[where] = ~(BAR0_SIZE - 1) |
							 PCI_BASE_ADDRESS_MEM_TYPE_32;
				return PCIBIOS_SUCCESSFUL;
			}
			/* Restore or set new BAR value */
			*(u32 *)&config[where] = (val & ~(BAR0_SIZE - 1)) |
						 PCI_BASE_ADDRESS_MEM_TYPE_32;
			return PCIBIOS_SUCCESSFUL;
		}
	}

	/* Handle PF SR-IOV VF BAR sizing/assignment. */
	if (!dev->is_vf && where >= SRIOV_CAP_OFFSET + PCI_SRIOV_BAR &&
	    where < SRIOV_CAP_OFFSET + PCI_SRIOV_BAR +
		    PCI_SRIOV_NUM_BARS * 4) {
		int vf_bar = (where - (SRIOV_CAP_OFFSET + PCI_SRIOV_BAR)) / 4;

		if (vf_bar == 0) {
			if (val == 0xffffffff) {
				*(u32 *)&config[where] = ~(BAR0_SIZE - 1) |
							 PCI_BASE_ADDRESS_MEM_TYPE_32;
				return PCIBIOS_SUCCESSFUL;
			}

			*(u32 *)&config[where] = (val & ~(BAR0_SIZE - 1)) |
						 PCI_BASE_ADDRESS_MEM_TYPE_32;
		} else {
			*(u32 *)&config[where] = 0;
		}

		return PCIBIOS_SUCCESSFUL;
	}

	/*
	 * Other SR-IOV config space is managed by the PCI core.  Do not create
	 * or rescan VFs from config write callbacks: the PF driver's
	 * .sriov_configure path below decides when VFs should become visible.
	 */
	if (!dev->is_vf && fake_pci_is_sriov_cfg(where)) {
		memcpy(&config[where], &val, size);
		return PCIBIOS_SUCCESSFUL;
	}

	/* Write to read-only registers is ignored */
	switch (where) {
	case PCI_VENDOR_ID:
	case PCI_DEVICE_ID:
	case PCI_REVISION_ID:
	case PCI_CLASS_PROG:
	case PCI_CLASS_DEVICE:
	case PCI_HEADER_TYPE:
	case PCI_SUBSYSTEM_VENDOR_ID:
	case PCI_SUBSYSTEM_ID:
	case PCI_CAPABILITY_LIST:
		return PCIBIOS_SUCCESSFUL;
	}

	/* Default: write to config space */
	memcpy(&config[where], &val, size);

	return PCIBIOS_SUCCESSFUL;
}

static struct pci_ops fake_pci_ops = {
	.read	= fake_pci_read_config,
	.write	= fake_pci_write_config,
};

/*
 * ============================================================================
 * SR-IOV VF Management
 * ============================================================================
 */

static void handle_sriov_numvfs_write(struct fake_pci_host *host, u16 num_vfs)
{
	int i;
	u8 *pf_config = host->pf.config_space;

	mutex_lock(&host->lock);

	if (num_vfs > MAX_VFS)
		num_vfs = MAX_VFS;

	pr_info("fake_pci: Setting NumVFs from %d to %d\n",
		host->num_vfs_enabled, num_vfs);

	/* If reducing VFs, mark extras as not present */
	for (i = num_vfs; i < host->num_vfs_enabled; i++) {
		host->vfs[i].present = false;
		pr_info("fake_pci: Disabled VF%d\n", i);
	}

	/* If increasing VFs, initialize and mark as present */
	for (i = host->num_vfs_enabled; i < num_vfs; i++) {
		init_vf_config_space(&host->vfs[i], i);
		pr_info("fake_pci: Enabled VF%d\n", i);
	}

	host->num_vfs_enabled = num_vfs;

	/* Update NumVF in PF config space */
	*(u16 *)&pf_config[SRIOV_CAP_OFFSET + PCI_SRIOV_NUM_VF] = num_vfs;

	mutex_unlock(&host->lock);
}

/*
 * ============================================================================
 * PF Driver / SR-IOV Control
 * ============================================================================
 */

static int fake_pci_pf_probe(struct pci_dev *pdev,
			     const struct pci_device_id *id)
{
	pci_info(pdev, "fake_pci: PF probed\n");
	return 0;
}

static void fake_pci_pf_remove(struct pci_dev *pdev)
{
	if (pci_num_vf(pdev)) {
		pci_info(pdev, "fake_pci: disabling VFs before PF removal\n");
		pci_disable_sriov(pdev);
		if (fake_host)
			handle_sriov_numvfs_write(fake_host, 0);
	}

	pci_info(pdev, "fake_pci: PF removed\n");
}

static int fake_pci_sriov_configure(struct pci_dev *pdev, int num_vfs)
{
	int err;

	if (!fake_host)
		return -ENODEV;

	if (num_vfs < 0 || num_vfs > MAX_VFS)
		return -EINVAL;

	if (!num_vfs) {
		pci_disable_sriov(pdev);
		handle_sriov_numvfs_write(fake_host, 0);
		return 0;
	}

	if (fake_host->num_vfs_enabled)
		return -EBUSY;

	/* Make VFs visible to our pci_ops before the PCI core scans them. */
	handle_sriov_numvfs_write(fake_host, num_vfs);

	err = pci_enable_sriov(pdev, num_vfs);
	if (err) {
		handle_sriov_numvfs_write(fake_host, 0);
		return err;
	}

	return num_vfs;
}

static const struct pci_device_id fake_pci_pf_ids[] = {
	{ PCI_DEVICE(FAKE_PCI_VENDOR_ID, FAKE_PCI_PF_DEVICE_ID) },
	{ }
};
MODULE_DEVICE_TABLE(pci, fake_pci_pf_ids);

static struct pci_driver fake_pci_pf_driver = {
	.name		= "fake_pci_sriov_pf",
	.id_table	= fake_pci_pf_ids,
	.probe		= fake_pci_pf_probe,
	.remove		= fake_pci_pf_remove,
	.sriov_configure = fake_pci_sriov_configure,
};

/*
 * ============================================================================
 * Host-side VF TTY Loopback Driver
 * ============================================================================
 */

static void pci_sim_uart_reset(struct pci_sim_uart *uart)
{
	unsigned long flags;

	spin_lock_irqsave(&uart->lock, flags);
	memset(uart->regs, 0, sizeof(uart->regs));
	uart->head = 0;
	uart->tail = 0;
	uart->count = 0;
	uart->dlab = false;
	uart->overrun = false;
	uart->divisor = 0;
	uart->fcr = 0;
	uart->intr_trigger_level = 1;
	uart->regs[UART_LSR] = UART_LSR_TEMT | UART_LSR_THRE;
	uart->regs[UART_MSR] = UART_MSR_DSR | UART_MSR_DCD | UART_MSR_CTS;
	spin_unlock_irqrestore(&uart->lock, flags);
}

static void pci_sim_uart_init(struct pci_sim_uart *uart)
{
	spin_lock_init(&uart->lock);
	pci_sim_uart_reset(uart);
}

static unsigned int pci_sim_uart_space_locked(struct pci_sim_uart *uart)
{
	return PCI_SIM_UART_FIFO_SIZE - uart->count;
}

static size_t pci_sim_uart_write_data(struct pci_sim_uart *uart,
				      const u8 *buf, size_t len)
{
	unsigned long flags;
	size_t copied, i;

	spin_lock_irqsave(&uart->lock, flags);
	copied = min_t(size_t, len, pci_sim_uart_space_locked(uart));

	for (i = 0; i < copied; i++) {
		uart->fifo[uart->head] = buf[i];
		uart->head = (uart->head + 1) % PCI_SIM_UART_FIFO_SIZE;
		uart->count++;
	}

	if (copied)
		uart->overrun = false;
	if (copied < len)
		uart->overrun = true;

	spin_unlock_irqrestore(&uart->lock, flags);

	return copied;
}

static size_t pci_sim_uart_peek_data(struct pci_sim_uart *uart, u8 *buf,
				     size_t len)
{
	unsigned long flags;
	size_t copied, i;
	unsigned int tail;

	spin_lock_irqsave(&uart->lock, flags);
	copied = min_t(size_t, len, uart->count);
	tail = uart->tail;

	for (i = 0; i < copied; i++) {
		buf[i] = uart->fifo[tail];
		tail = (tail + 1) % PCI_SIM_UART_FIFO_SIZE;
	}

	spin_unlock_irqrestore(&uart->lock, flags);

	return copied;
}

static void pci_sim_uart_consume_data(struct pci_sim_uart *uart, size_t len)
{
	unsigned long flags;

	spin_lock_irqsave(&uart->lock, flags);
	len = min_t(size_t, len, uart->count);
	uart->tail = (uart->tail + len) % PCI_SIM_UART_FIFO_SIZE;
	uart->count -= len;
	spin_unlock_irqrestore(&uart->lock, flags);
}

static unsigned int pci_sim_uart_write_room(struct pci_sim_uart *uart)
{
	unsigned long flags;
	unsigned int room;

	spin_lock_irqsave(&uart->lock, flags);
	room = pci_sim_uart_space_locked(uart);
	spin_unlock_irqrestore(&uart->lock, flags);

	return room;
}

static unsigned int pci_sim_uart_chars_in_buffer(struct pci_sim_uart *uart)
{
	unsigned long flags;
	unsigned int count;

	spin_lock_irqsave(&uart->lock, flags);
	count = uart->count;
	spin_unlock_irqrestore(&uart->lock, flags);

	return count;
}

static u8 pci_sim_uart_lsr(struct pci_sim_uart *uart)
{
	unsigned long flags;
	u8 lsr = UART_LSR_TEMT | UART_LSR_THRE;

	spin_lock_irqsave(&uart->lock, flags);
	if (uart->count)
		lsr |= UART_LSR_DR;
	if (uart->overrun)
		lsr |= UART_LSR_OE;
	spin_unlock_irqrestore(&uart->lock, flags);

	return lsr;
}

static u8 pci_sim_uart_read_data(struct pci_sim_uart *uart)
{
	unsigned long flags;
	u8 val = 0xff;

	spin_lock_irqsave(&uart->lock, flags);
	if (uart->count) {
		val = uart->fifo[uart->tail];
		uart->tail = (uart->tail + 1) % PCI_SIM_UART_FIFO_SIZE;
		uart->count--;
	}
	spin_unlock_irqrestore(&uart->lock, flags);

	return val;
}

static u8 pci_sim_uart_iir(struct pci_sim_uart *uart)
{
	unsigned long flags;
	u8 iir, ier;

	spin_lock_irqsave(&uart->lock, flags);
	ier = uart->regs[UART_IER];

	if ((ier & UART_IER_RLSI) && uart->overrun)
		iir = UART_IIR_RLSI;
	else if ((ier & UART_IER_RDI) &&
		 uart->count >= uart->intr_trigger_level)
		iir = UART_IIR_RDI;
	else if (ier & UART_IER_THRI)
		iir = UART_IIR_THRI;
	else if ((ier & UART_IER_MSI) &&
		 (uart->regs[UART_MCR] & (UART_MCR_RTS | UART_MCR_DTR)))
		iir = UART_IIR_MSI;
	else
		iir = UART_IIR_NO_INT;

	if (uart->fcr & UART_FCR_ENABLE_FIFO)
		iir |= UART_IIR_FIFO_ENABLED_16550A;

	spin_unlock_irqrestore(&uart->lock, flags);

	return iir;
}

static void pci_sim_uart_clear_fifo(struct pci_sim_uart *uart)
{
	unsigned long flags;

	spin_lock_irqsave(&uart->lock, flags);
	uart->head = 0;
	uart->tail = 0;
	uart->count = 0;
	uart->overrun = false;
	spin_unlock_irqrestore(&uart->lock, flags);
}

static u8 pci_sim_uart_read_reg(struct pci_sim_uart *uart, u8 reg)
{
	reg &= 7;

	if (uart->dlab) {
		switch (reg) {
		case UART_DLL:
			return uart->divisor & 0xff;
		case UART_DLM:
			return uart->divisor >> 8;
		}
	}

	switch (reg) {
	case UART_RX:
		return pci_sim_uart_read_data(uart);
	case UART_IER:
		return uart->regs[UART_IER] & 0x0f;
	case UART_IIR:
		return pci_sim_uart_iir(uart);
	case UART_LCR:
		return uart->regs[UART_LCR];
	case UART_MCR:
		return uart->regs[UART_MCR];
	case UART_LSR:
		return pci_sim_uart_lsr(uart);
	case UART_MSR:
		return uart->regs[UART_MSR];
	case UART_SCR:
		return uart->regs[UART_SCR];
	default:
		return 0xff;
	}
}

static void pci_sim_uart_write_reg(struct pci_sim_uart *uart, u8 reg, u8 val)
{
	reg &= 7;

	if (uart->dlab) {
		switch (reg) {
		case UART_DLL:
			uart->divisor = (uart->divisor & 0xff00) | val;
			return;
		case UART_DLM:
			uart->divisor = (uart->divisor & 0x00ff) | (val << 8);
			return;
		}
	}

	switch (reg) {
	case UART_TX:
		pci_sim_uart_write_data(uart, &val, 1);
		break;
	case UART_IER:
		uart->regs[UART_IER] = val & 0x0f;
		break;
	case UART_FCR:
		uart->fcr = val;
		switch (val & UART_FCR_TRIGGER_MASK) {
		case UART_FCR_TRIGGER_1:
			uart->intr_trigger_level = 1;
			break;
		case UART_FCR_TRIGGER_4:
			uart->intr_trigger_level = 4;
			break;
		case UART_FCR_TRIGGER_8:
			uart->intr_trigger_level = 8;
			break;
		case UART_FCR_TRIGGER_14:
			uart->intr_trigger_level = 14;
			break;
		}
		if (val & (UART_FCR_CLEAR_RCVR | UART_FCR_CLEAR_XMIT))
			pci_sim_uart_clear_fifo(uart);
		break;
	case UART_LCR:
		uart->regs[UART_LCR] = val;
		uart->dlab = !!(val & UART_LCR_DLAB);
		break;
	case UART_MCR:
		uart->regs[UART_MCR] = val;
		break;
	case UART_SCR:
		uart->regs[UART_SCR] = val;
		break;
	default:
		break;
	}
}

static void pci_sim_vf_flush_to_tty(struct pci_sim_vf_tty *vf)
{
	u8 buf[PCI_SIM_UART_CHUNK];
	size_t copied, pending;
	unsigned int room;
	bool pushed = false;

	for (;;) {
		room = tty_buffer_space_avail(&vf->port);
		if (!room)
			break;

		pending = pci_sim_uart_peek_data(&vf->uart, buf,
				min_t(size_t, sizeof(buf), room));
		if (!pending)
			break;

		copied = tty_insert_flip_string(&vf->port, buf, pending);
		if (!copied)
			break;

		pci_sim_uart_consume_data(&vf->uart, copied);
		pushed = true;

		if (copied < pending)
			break;
	}

	if (pushed)
		tty_flip_buffer_push(&vf->port);
}

static int pci_sim_tty_install(struct tty_driver *driver,
			       struct tty_struct *tty)
{
	struct pci_sim_vf_tty *vf;
	struct tty_port *port;
	int ret;

	mutex_lock(&pci_sim_tty_idr_lock);
	vf = idr_find(&pci_sim_tty_idr, tty->index);
	if (vf)
		port = tty_port_get(&vf->port);
	else
		port = NULL;
	mutex_unlock(&pci_sim_tty_idr_lock);

	if (!port)
		return -ENODEV;

	tty->driver_data = vf;
	ret = tty_port_install(port, driver, tty);
	if (ret)
		tty_port_put(port);

	return ret;
}

static void pci_sim_tty_cleanup(struct tty_struct *tty)
{
	tty_port_put(tty->port);
}

static int pci_sim_tty_open(struct tty_struct *tty, struct file *filp)
{
	struct pci_sim_vf_tty *vf = tty->driver_data;
	int ret;

	mutex_lock(&vf->state_lock);
	ret = vf->dead ? -ENODEV : 0;
	mutex_unlock(&vf->state_lock);
	if (ret)
		return ret;

	return tty_port_open(tty->port, tty, filp);
}

static void pci_sim_tty_close(struct tty_struct *tty, struct file *filp)
{
	tty_port_close(tty->port, tty, filp);
}

static ssize_t pci_sim_tty_write(struct tty_struct *tty, const u8 *buf,
				 size_t len)
{
	struct pci_sim_vf_tty *vf = tty->driver_data;
	size_t copied;

	if (!len)
		return 0;

	mutex_lock(&vf->state_lock);
	if (vf->dead) {
		mutex_unlock(&vf->state_lock);
		return -ENODEV;
	}

	copied = pci_sim_uart_write_data(&vf->uart, buf, len);
	pci_sim_vf_flush_to_tty(vf);
	mutex_unlock(&vf->state_lock);

	return copied;
}

static unsigned int pci_sim_tty_write_room(struct tty_struct *tty)
{
	struct pci_sim_vf_tty *vf = tty->driver_data;

	if (!vf)
		return 0;

	return pci_sim_uart_write_room(&vf->uart);
}

static unsigned int pci_sim_tty_chars_in_buffer(struct tty_struct *tty)
{
	struct pci_sim_vf_tty *vf = tty->driver_data;

	if (!vf)
		return 0;

	return pci_sim_uart_chars_in_buffer(&vf->uart);
}

static void pci_sim_tty_hangup(struct tty_struct *tty)
{
	tty_port_hangup(tty->port);
}

static const struct tty_operations pci_sim_tty_ops = {
	.install	= pci_sim_tty_install,
	.open		= pci_sim_tty_open,
	.close		= pci_sim_tty_close,
	.write		= pci_sim_tty_write,
	.write_room	= pci_sim_tty_write_room,
	.chars_in_buffer = pci_sim_tty_chars_in_buffer,
	.hangup		= pci_sim_tty_hangup,
	.cleanup	= pci_sim_tty_cleanup,
};

static void pci_sim_tty_destruct_port(struct tty_port *port)
{
	struct pci_sim_vf_tty *vf = container_of(port, struct pci_sim_vf_tty,
						   port);

	mutex_lock(&pci_sim_tty_idr_lock);
	if (vf->id >= 0)
		idr_remove(&pci_sim_tty_idr, vf->id);
	mutex_unlock(&pci_sim_tty_idr_lock);

	kfree(vf);
}

static const struct tty_port_operations pci_sim_tty_port_ops = {
	.destruct = pci_sim_tty_destruct_port,
};

static int pci_sim_vf_probe(struct pci_dev *pdev,
			    const struct pci_device_id *id)
{
	struct pci_sim_vf_tty *vf;
	struct device *tty_dev;
	int ret;

	ret = pci_enable_device(pdev);
	if (ret)
		return ret;

	vf = kzalloc(sizeof(*vf), GFP_KERNEL);
	if (!vf) {
		ret = -ENOMEM;
		goto err_disable;
	}

	vf->pdev = pdev;
	vf->id = -1;
	mutex_init(&vf->state_lock);
	pci_sim_uart_init(&vf->uart);

	mutex_lock(&pci_sim_tty_idr_lock);
	ret = idr_alloc(&pci_sim_tty_idr, vf, 0, PCI_SIM_MAX_TTYS, GFP_KERNEL);
	mutex_unlock(&pci_sim_tty_idr_lock);
	if (ret < 0)
		goto err_free;
	vf->id = ret;

	tty_port_init(&vf->port);
	vf->port.ops = &pci_sim_tty_port_ops;

	tty_dev = tty_port_register_device(&vf->port, pci_sim_tty_driver,
					   vf->id, &pdev->dev);
	if (IS_ERR(tty_dev)) {
		ret = PTR_ERR(tty_dev);
		goto err_put_port;
	}

	pci_set_drvdata(pdev, vf);
	pci_info(pdev, "registered /dev/%s%d, lsr=0x%02x\n",
		 PCI_SIM_TTY_NAME, vf->id, pci_sim_uart_lsr(&vf->uart));

	return 0;

err_put_port:
	tty_port_put(&vf->port);
	pci_disable_device(pdev);
	return ret;
err_free:
	kfree(vf);
err_disable:
	pci_disable_device(pdev);
	return ret;
}

static void pci_sim_vf_remove(struct pci_dev *pdev)
{
	struct pci_sim_vf_tty *vf = pci_get_drvdata(pdev);

	if (!vf)
		return;

	mutex_lock(&vf->state_lock);
	vf->dead = true;
	mutex_unlock(&vf->state_lock);

	tty_port_tty_hangup(&vf->port, false);
	tty_unregister_device(pci_sim_tty_driver, vf->id);
	pci_set_drvdata(pdev, NULL);
	pci_disable_device(pdev);
	tty_port_put(&vf->port);
}

static const struct pci_device_id pci_sim_vf_ids[] = {
	{ PCI_DEVICE(FAKE_PCI_VENDOR_ID, FAKE_PCI_VF_DEVICE_ID) },
	{ }
};
MODULE_DEVICE_TABLE(pci, pci_sim_vf_ids);

static struct pci_driver pci_sim_vf_driver = {
	.name		= "pci_sim_loopback_vf",
	.id_table	= pci_sim_vf_ids,
	.probe		= pci_sim_vf_probe,
	.remove		= pci_sim_vf_remove,
};

/*
 * VFIO-facing VF driver.
 *
 * Generic vfio-pci expects BAR0 to be backed by real host MMIO.  These fake
 * VFs only exist behind fake pci_ops, so pci_iomap()/ioread() cannot service
 * guest BAR accesses.  This override-only driver still exposes the device via
 * VFIO, but traps BAR0 read/write and emulates a tiny 16550 loopback UART.
 */
static int pci_sim_vfio_open_device(struct vfio_device *core_vdev)
{
	struct pci_sim_vfio_vf *sim = container_of(core_vdev,
			struct pci_sim_vfio_vf, core.vdev);
	int ret;

	ret = vfio_pci_core_enable(&sim->core);
	if (ret)
		return ret;

	pci_sim_uart_reset(&sim->uart);
	vfio_pci_core_finish_enable(&sim->core);
	return 0;
}

static bool pci_sim_vfio_bar0_uart_reg(loff_t pos, u8 *reg)
{
	loff_t offset;

	if (vfio_guest_8250_compat) {
		if (pos < PCI_SIM_SGI_IOC3_UART_OFFSET ||
		    pos >= PCI_SIM_SGI_IOC3_UART_OFFSET + 8)
			return false;
		offset = pos - PCI_SIM_SGI_IOC3_UART_OFFSET;
	} else {
		if (pos >= 8)
			return false;
		offset = pos;
	}

	*reg = offset;
	return true;
}

static ssize_t pci_sim_vfio_bar0_rw(struct pci_sim_vfio_vf *sim,
				    char __user *buf, size_t count,
				    loff_t *ppos, bool iswrite)
{
	loff_t pos = *ppos & VFIO_PCI_OFFSET_MASK;
	size_t done;
	u8 reg, val;

	if (pos >= PCI_SIM_VFIO_BAR0_SIZE)
		return -EINVAL;

	count = min_t(size_t, count, PCI_SIM_VFIO_BAR0_SIZE - pos);

	mutex_lock(&sim->lock);
	for (done = 0; done < count; done++) {
		if (iswrite) {
			if (copy_from_user(&val, buf + done, 1)) {
				mutex_unlock(&sim->lock);
				return done ?: -EFAULT;
			}
			if (!pci_sim_vfio_bar0_uart_reg(pos + done, &reg))
				continue;
			if (vfio_uart_trace)
				pr_info("fake_pci: vfio uart W off=0x%llx reg=%u val=0x%02x\n",
					(unsigned long long)(pos + done), reg, val);
			pci_sim_uart_write_reg(&sim->uart, reg, val);
		} else {
			if (pci_sim_vfio_bar0_uart_reg(pos + done, &reg)) {
				val = pci_sim_uart_read_reg(&sim->uart, reg);
				if (vfio_uart_trace)
					pr_info("fake_pci: vfio uart R off=0x%llx reg=%u val=0x%02x\n",
						(unsigned long long)(pos + done), reg, val);
			} else {
				val = 0xff;
			}
			if (copy_to_user(buf + done, &val, 1)) {
				mutex_unlock(&sim->lock);
				return done ?: -EFAULT;
			}
		}
	}
	mutex_unlock(&sim->lock);

	*ppos += done;
	return done;
}

static int pci_sim_vfio_copy_config_value(char __user *buf,
					  loff_t pos, size_t count,
					  unsigned int reg, const void *val,
					  size_t val_size)
{
	loff_t copy_offset;
	size_t copy_count, register_offset;

	if (!vfio_pci_core_range_intersect_range(pos, count, reg, val_size,
					       &copy_offset, &copy_count,
					       &register_offset))
		return 0;

	if (copy_to_user(buf + copy_offset, val + register_offset, copy_count))
		return -EFAULT;

	return 0;
}

static ssize_t pci_sim_vfio_read_config(struct vfio_device *core_vdev,
					char __user *buf, size_t count,
					loff_t *ppos)
{
	loff_t pos = *ppos & VFIO_PCI_OFFSET_MASK;
	__le16 val16;
	__le32 val32;
	ssize_t ret;
	size_t done;

	ret = vfio_pci_core_read(core_vdev, buf, count, ppos);
	if (ret <= 0 || !vfio_guest_8250_compat)
		return ret;
	done = ret;

	/*
	 * The fake host keeps local experimental IDs (1d55:1001), but many guest
	 * kernels only have an 8250_pci explicit table entry for known MMIO PCI
	 * serial devices.  Present the SGI IOC3 serial ID through VFIO config
	 * space because that 8250_pci entry is MMIO and polling/no-IRQ based.
	 */
	val16 = cpu_to_le16(PCI_VENDOR_ID_SGI);
	if (pci_sim_vfio_copy_config_value(buf, pos, done, PCI_VENDOR_ID,
					   &val16, sizeof(val16)))
		return -EFAULT;

	val16 = cpu_to_le16(PCI_DEVICE_ID_SGI_IOC3);
	if (pci_sim_vfio_copy_config_value(buf, pos, done, PCI_DEVICE_ID,
					   &val16, sizeof(val16)))
		return -EFAULT;

	val32 = cpu_to_le32(FAKE_PCI_CLASS << 8);
	if (pci_sim_vfio_copy_config_value(buf, pos, done, PCI_CLASS_REVISION,
					   &val32, sizeof(val32)))
		return -EFAULT;

	val16 = cpu_to_le16(0xff00);
	if (pci_sim_vfio_copy_config_value(buf, pos, done,
					   PCI_SUBSYSTEM_VENDOR_ID,
					   &val16, sizeof(val16)))
		return -EFAULT;

	val16 = cpu_to_le16(0);
	if (pci_sim_vfio_copy_config_value(buf, pos, done, PCI_SUBSYSTEM_ID,
					   &val16, sizeof(val16)))
		return -EFAULT;

	return ret;
}

static ssize_t pci_sim_vfio_read(struct vfio_device *core_vdev,
				 char __user *buf, size_t count, loff_t *ppos)
{
	struct pci_sim_vfio_vf *sim = container_of(core_vdev,
			struct pci_sim_vfio_vf, core.vdev);
	unsigned int index = VFIO_PCI_OFFSET_TO_INDEX(*ppos);

	if (!count)
		return 0;

	if (index == VFIO_PCI_CONFIG_REGION_INDEX)
		return pci_sim_vfio_read_config(core_vdev, buf, count, ppos);

	if (index == VFIO_PCI_BAR0_REGION_INDEX)
		return pci_sim_vfio_bar0_rw(sim, buf, count, ppos, false);

	return vfio_pci_core_read(core_vdev, buf, count, ppos);
}

static ssize_t pci_sim_vfio_write(struct vfio_device *core_vdev,
				  const char __user *buf, size_t count, loff_t *ppos)
{
	struct pci_sim_vfio_vf *sim = container_of(core_vdev,
			struct pci_sim_vfio_vf, core.vdev);
	unsigned int index = VFIO_PCI_OFFSET_TO_INDEX(*ppos);

	if (!count)
		return 0;

	if (index == VFIO_PCI_BAR0_REGION_INDEX)
		return pci_sim_vfio_bar0_rw(sim, (char __user *)buf, count,
					       ppos, true);

	return vfio_pci_core_write(core_vdev, buf, count, ppos);
}

static int pci_sim_vfio_get_region_info(struct vfio_device *core_vdev,
					struct vfio_region_info *info,
					struct vfio_info_cap *caps)
{
	if (info->index != VFIO_PCI_BAR0_REGION_INDEX)
		return vfio_pci_ioctl_get_region_info(core_vdev, info, caps);

	info->offset = VFIO_PCI_INDEX_TO_OFFSET(info->index);
	info->size = vfio_guest_8250_compat ? PCI_SIM_VFIO_BAR0_SIZE : BAR0_SIZE;
	info->flags = VFIO_REGION_INFO_FLAG_READ | VFIO_REGION_INFO_FLAG_WRITE;
	return 0;
}

static int pci_sim_vfio_mmap(struct vfio_device *core_vdev,
			     struct vm_area_struct *vma)
{
	unsigned int index = vma->vm_pgoff >> (VFIO_PCI_OFFSET_SHIFT - PAGE_SHIFT);

	if (index == VFIO_PCI_BAR0_REGION_INDEX)
		return -EINVAL;

	return vfio_pci_core_mmap(core_vdev, vma);
}

static const struct vfio_device_ops pci_sim_vfio_ops = {
	.name		= "pci_sim_vfio_pci",
	.init		= vfio_pci_core_init_dev,
	.release	= vfio_pci_core_release_dev,
	.open_device	= pci_sim_vfio_open_device,
	.close_device	= vfio_pci_core_close_device,
	.ioctl		= vfio_pci_core_ioctl,
	.get_region_info_caps = pci_sim_vfio_get_region_info,
	.device_feature = vfio_pci_core_ioctl_feature,
	.read		= pci_sim_vfio_read,
	.write		= pci_sim_vfio_write,
	.mmap		= pci_sim_vfio_mmap,
	.request	= vfio_pci_core_request,
	.match		= vfio_pci_core_match,
	.match_token_uuid = vfio_pci_core_match_token_uuid,
	.bind_iommufd	= vfio_iommufd_physical_bind,
	.unbind_iommufd	= vfio_iommufd_physical_unbind,
	.attach_ioas	= vfio_iommufd_physical_attach_ioas,
	.detach_ioas	= vfio_iommufd_physical_detach_ioas,
	.pasid_attach_ioas = vfio_iommufd_physical_pasid_attach_ioas,
	.pasid_detach_ioas = vfio_iommufd_physical_pasid_detach_ioas,
};

static int pci_sim_vfio_probe(struct pci_dev *pdev,
			      const struct pci_device_id *id)
{
	struct pci_sim_vfio_vf *sim;
	int ret;

	sim = vfio_alloc_device(pci_sim_vfio_vf, core.vdev, &pdev->dev,
					&pci_sim_vfio_ops);
	if (IS_ERR(sim))
		return PTR_ERR(sim);

	mutex_init(&sim->lock);
	pci_sim_uart_init(&sim->uart);
	dev_set_drvdata(&pdev->dev, &sim->core);

	ret = vfio_pci_core_register_device(&sim->core);
	if (ret)
		goto err_put;

	pci_info(pdev, "registered VFIO UART BAR0 emulator\n");
	return 0;

err_put:
	vfio_put_device(&sim->core.vdev);
	return ret;
}

static void pci_sim_vfio_remove(struct pci_dev *pdev)
{
	struct vfio_pci_core_device *core = dev_get_drvdata(&pdev->dev);
	struct pci_sim_vfio_vf *sim;

	if (!core)
		return;

	sim = container_of(core, struct pci_sim_vfio_vf, core);
	vfio_pci_core_unregister_device(&sim->core);
	vfio_put_device(&sim->core.vdev);
}

static const struct pci_device_id pci_sim_vfio_ids[] = {
	{ PCI_DRIVER_OVERRIDE_DEVICE_VFIO(FAKE_PCI_VENDOR_ID,
					    FAKE_PCI_VF_DEVICE_ID) },
	{ }
};
MODULE_DEVICE_TABLE(pci, pci_sim_vfio_ids);

static struct pci_driver pci_sim_vfio_driver = {
	.name			= "pci_sim_vfio_pci",
	.id_table		= pci_sim_vfio_ids,
	.probe			= pci_sim_vfio_probe,
	.remove			= pci_sim_vfio_remove,
	.err_handler		= &vfio_pci_core_err_handlers,
	.driver_managed_dma	= true,
};

static int pci_sim_tty_register_driver(void)
{
	int ret;

	pci_sim_tty_driver = tty_alloc_driver(PCI_SIM_MAX_TTYS,
			TTY_DRIVER_REAL_RAW | TTY_DRIVER_DYNAMIC_DEV);
	if (IS_ERR(pci_sim_tty_driver))
		return PTR_ERR(pci_sim_tty_driver);

	pci_sim_tty_driver->driver_name = "pci_sim_loopback";
	pci_sim_tty_driver->name = PCI_SIM_TTY_NAME;
	pci_sim_tty_driver->major = 0;
	pci_sim_tty_driver->minor_start = 0;
	pci_sim_tty_driver->type = TTY_DRIVER_TYPE_SERIAL;
	pci_sim_tty_driver->subtype = SERIAL_TYPE_NORMAL;
	pci_sim_tty_driver->init_termios = tty_std_termios;
	pci_sim_tty_driver->init_termios.c_cflag = B9600 | CS8 | CREAD |
						    HUPCL | CLOCAL;
	pci_sim_tty_driver->init_termios.c_lflag &= ~(ECHO | ICANON);
	pci_sim_tty_driver->init_termios.c_oflag &= ~(OPOST | ONLCR);

	tty_set_operations(pci_sim_tty_driver, &pci_sim_tty_ops);

	ret = tty_register_driver(pci_sim_tty_driver);
	if (ret) {
		tty_driver_kref_put(pci_sim_tty_driver);
		pci_sim_tty_driver = NULL;
	}

	return ret;
}

static void pci_sim_tty_unregister_driver(void)
{
	if (!pci_sim_tty_driver)
		return;

	tty_unregister_driver(pci_sim_tty_driver);
	tty_driver_kref_put(pci_sim_tty_driver);
	pci_sim_tty_driver = NULL;
	idr_destroy(&pci_sim_tty_idr);
}

/*
 * ============================================================================
 * PCI Host Bridge Setup
 * ============================================================================
 */

static void fake_pci_release_host_bridge(struct pci_host_bridge *bridge)
{
	pr_info("fake_pci: releasing host bridge\n");
}

static int fake_pci_map_irq(const struct pci_dev *dev, u8 slot, u8 pin)
{
	return fake_intx_irq ?: -1;
}

static int fake_pci_host_probe(void)
{
	struct pci_host_bridge *bridge;
	int err;
	static int domain_nr;

	/* fake_host must be allocated by caller with pdev already set */

	mutex_init(&fake_host->lock);

	/*
	 * Get a unique conventional 16-bit PCI segment/domain.  Avoid domain 0
	 * because the real host bridge usually owns it, and avoid the emulated
	 * 32-bit domains that user space such as Nova cannot represent.
	 */
	if (!domain_nr)
		domain_nr = 1;
	fake_host->domain_nr = domain_nr++;
	if (domain_nr > 0xffff)
		domain_nr = 1;
	fake_host->sysdata.domain = fake_host->domain_nr;
	fake_host->sysdata.node = NUMA_NO_NODE;

	/* Initialize the PF */
	init_pf_config_space(&fake_host->pf);

	/* Allocate host bridge */
	bridge = pci_alloc_host_bridge(0);
	if (!bridge) {
		err = -ENOMEM;
		goto err_mutex;
	}

	fake_host->bridge = bridge;

	/* Set up the bridge */
	bridge->sysdata = &fake_host->sysdata;
	bridge->ops = &fake_pci_ops;
	bridge->map_irq = fake_pci_map_irq;
	bridge->busnr = 0;
	bridge->dev.parent = &fake_host->pdev->dev;

	/* Keep the host bridge's visible domain conventional as well. */
	bridge->domain_nr = fake_host->domain_nr;

	/* Set up release function */
	pci_set_host_bridge_release(bridge, fake_pci_release_host_bridge, NULL);

	/* Add bus number and MMIO window resources. */
	pci_add_resource(&bridge->windows, &fake_pci_bus_resource);
	pci_add_resource(&bridge->windows, &fake_pci_mem_resource);

	/* Probe the host bridge */
	err = pci_host_probe(bridge);
	if (err) {
		pr_err("fake_pci: pci_host_probe failed: %d\n", err);
		goto err_free_bridge;
	}

	pr_info("fake_pci: Host bridge created on domain %04x\n",
		fake_host->domain_nr);

	return 0;

err_free_bridge:
	pci_free_host_bridge(bridge);
err_mutex:
	mutex_destroy(&fake_host->lock);
	return err;
}

static void fake_pci_host_remove(void)
{
	if (!fake_host)
		return;

	if (fake_host->bridge && fake_host->bridge->bus) {
		pci_lock_rescan_remove();
		pci_stop_root_bus(fake_host->bridge->bus);
		pci_remove_root_bus(fake_host->bridge->bus);
		pci_unlock_rescan_remove();
	}

	mutex_destroy(&fake_host->lock);
	kfree(fake_host);
	fake_host = NULL;
}

/*
 * ============================================================================
 * Module Init/Exit
 * ============================================================================
 */

static int __init fake_pci_sriov_init(void)
{
	struct platform_device_info pdevinfo = {
		.name = "fake-pci-iommu",
		.id = PLATFORM_DEVID_AUTO,
	};
	int err;

	pr_info("fake_pci: Initializing fake PCI SR-IOV driver\n");

	/*
	 * Step 1: Create platform device for IOMMU
	 * This must be done first so the IOMMU can be registered before
	 * any PCI devices are created.
	 */
	fake_iommu_pdev = platform_device_register_full(&pdevinfo);
	if (IS_ERR(fake_iommu_pdev)) {
		err = PTR_ERR(fake_iommu_pdev);
		pr_err("fake_pci: Failed to register IOMMU platform device: %d\n",
		       err);
		return err;
	}

	/*
	 * Step 2: Register the software IOMMU driver
	 */
	err = iommu_device_sysfs_add(&fake_iommu_dev, &fake_iommu_pdev->dev,
				     NULL, "fake-pci-iommu");
	if (err) {
		pr_err("fake_pci: Failed to add IOMMU sysfs: %d\n", err);
		goto err_platform_dev;
	}

	err = iommu_device_register(&fake_iommu_dev, &fake_iommu_ops,
				    &fake_iommu_pdev->dev);
	if (err) {
		pr_err("fake_pci: Failed to register IOMMU: %d\n", err);
		goto err_iommu_sysfs;
	}

	pr_info("fake_pci: Software IOMMU registered\n");

	err = pci_sim_tty_register_driver();
	if (err) {
		pr_err("fake_pci: Failed to register TTY driver: %d\n", err);
		goto err_iommu;
	}

	err = pci_register_driver(&pci_sim_vfio_driver);
	if (err) {
		pr_err("fake_pci: Failed to register VFIO UART driver: %d\n", err);
		goto err_tty;
	}

	err = pci_register_driver(&pci_sim_vf_driver);
	if (err) {
		pr_err("fake_pci: Failed to register VF loopback driver: %d\n", err);
		goto err_vfio_driver;
	}

	/* Register before host probe so our PF driver wins class-code races. */
	err = pci_register_driver(&fake_pci_pf_driver);
	if (err) {
		pr_err("fake_pci: Failed to register PF driver: %d\n", err);
		goto err_vf_driver;
	}

	/*
	 * Step 3: Create platform device for the PCI host controller
	 */
	pdevinfo.name = "fake-pci-host";

	fake_host = kzalloc(sizeof(*fake_host), GFP_KERNEL);
	if (!fake_host) {
		err = -ENOMEM;
		goto err_pf_driver;
	}

	fake_host->pdev = platform_device_register_full(&pdevinfo);
	if (IS_ERR(fake_host->pdev)) {
		err = PTR_ERR(fake_host->pdev);
		pr_err("fake_pci: Failed to register PCI host platform device: %d\n",
		       err);
		kfree(fake_host);
		fake_host = NULL;
		goto err_pf_driver;
	}

	/*
	 * Step 4: Create the fake PCI host bridge
	 * Now devices created here will be claimed by our IOMMU.
	 */
	err = fake_pci_host_probe();
	if (err) {
		platform_device_unregister(fake_host->pdev);
		kfree(fake_host);
		fake_host = NULL;
		goto err_pf_driver;
	}

	pr_info("fake_pci: Module loaded successfully\n");
	pr_info("fake_pci: Use 'echo N > /sys/bus/pci/devices/.../sriov_numvfs' to enable VFs\n");

	return 0;

err_pf_driver:
	pci_unregister_driver(&fake_pci_pf_driver);
err_vf_driver:
	pci_unregister_driver(&pci_sim_vf_driver);
err_vfio_driver:
	pci_unregister_driver(&pci_sim_vfio_driver);
err_tty:
	pci_sim_tty_unregister_driver();
err_iommu:
	iommu_device_unregister(&fake_iommu_dev);
err_iommu_sysfs:
	iommu_device_sysfs_remove(&fake_iommu_dev);
err_platform_dev:
	platform_device_unregister(fake_iommu_pdev);
	return err;
}

static void __exit fake_pci_sriov_exit(void)
{
	pr_info("fake_pci: Unloading module\n");

	/* Remove PCI devices before unregistering their drivers. */
	if (fake_host) {
		struct platform_device *pdev = fake_host->pdev;

		fake_pci_host_remove();
		if (pdev)
			platform_device_unregister(pdev);
	}

	pci_unregister_driver(&fake_pci_pf_driver);
	pci_unregister_driver(&pci_sim_vf_driver);
	pci_unregister_driver(&pci_sim_vfio_driver);
	pci_sim_tty_unregister_driver();

	/* Then remove IOMMU */
	iommu_device_unregister(&fake_iommu_dev);
	iommu_device_sysfs_remove(&fake_iommu_dev);
	platform_device_unregister(fake_iommu_pdev);

	pr_info("fake_pci: Module unloaded\n");
}

module_init(fake_pci_sriov_init);
module_exit(fake_pci_sriov_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Linux Kernel Community");
MODULE_DESCRIPTION("Fake PCI Host Controller with Software IOMMU for SR-IOV Testing");
