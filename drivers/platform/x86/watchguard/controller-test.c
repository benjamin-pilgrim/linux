// SPDX-License-Identifier: GPL-2.0-only
/* Enumerate an in-memory PCI fabric; no real configuration space is accessed. */
#include <kunit/test.h>
#include <linux/pci.h>
#include <asm/pci.h>

#include "controller.h"

#define CONTROLLER_TEST_BUS 0xf0
#define CONTROLLER_TEST_DOMAIN 0x7ffe

struct controller_fixture {
	struct pci_sysdata sysdata;
	struct device *parent;
	struct pci_bus *bus;
	struct resource bus_range;
	u8 config[2][3][PCI_CFG_SPACE_SIZE];
	bool present[2][3];
};

static u8 *controller_config(struct pci_bus *bus, unsigned int devfn)
{
	struct controller_fixture *f = container_of(bus->sysdata,
					struct controller_fixture, sysdata);
	unsigned int level = bus->number - CONTROLLER_TEST_BUS;
	unsigned int slot = PCI_SLOT(devfn);

	if (level >= 2 || slot >= 3 || PCI_FUNC(devfn) || !f->present[level][slot])
		return NULL;
	return f->config[level][slot];
}

static int controller_read(struct pci_bus *bus, unsigned int devfn,
			   int where, int size, u32 *value)
{
	u8 *config = controller_config(bus, devfn);
	unsigned int i;

	*value = ~0U;
	if (!config)
		return PCIBIOS_DEVICE_NOT_FOUND;
	if (where < 0 || where + size > PCI_CFG_SPACE_SIZE)
		return PCIBIOS_BAD_REGISTER_NUMBER;
	*value = 0;
	for (i = 0; i < size; i++)
		*value |= (u32)config[where + i] << (8 * i);
	return PCIBIOS_SUCCESSFUL;
}

static int controller_write(struct pci_bus *bus, unsigned int devfn,
			    int where, int size, u32 value)
{
	u8 *config = controller_config(bus, devfn);
	unsigned int last_bar, rom, i;

	if (!config)
		return PCIBIOS_DEVICE_NOT_FOUND;
	if (where < 0 || where + size > PCI_CFG_SPACE_SIZE)
		return PCIBIOS_BAD_REGISTER_NUMBER;
	/* The synthetic devices implement no BARs or expansion ROM. */
	if (config[PCI_HEADER_TYPE] == PCI_HEADER_TYPE_BRIDGE) {
		last_bar = PCI_BASE_ADDRESS_1;
		rom = PCI_ROM_ADDRESS1;
	} else {
		last_bar = PCI_BASE_ADDRESS_5;
		rom = PCI_ROM_ADDRESS;
	}
	if ((where >= PCI_BASE_ADDRESS_0 && where <= last_bar) || where == rom)
		return PCIBIOS_SUCCESSFUL;
	for (i = 0; i < size; i++)
		config[where + i] = value >> (8 * i);
	return PCIBIOS_SUCCESSFUL;
}

static struct pci_ops controller_ops = {
	.read = controller_read,
	.write = controller_write,
};

static void controller_set_config(u8 *config, unsigned int offset, u32 value)
{
	unsigned int i;

	for (i = 0; i < sizeof(value); i++)
		config[offset + i] = value >> (8 * i);
}

static void controller_add_device(struct controller_fixture *f, unsigned int level,
				  unsigned int slot, u16 vendor, u16 device,
				  unsigned int class)
{
	u8 *config = f->config[level][slot];

	f->present[level][slot] = true;
	controller_set_config(config, PCI_VENDOR_ID, vendor | ((u32)device << 16));
	controller_set_config(config, PCI_CLASS_REVISION, class << 8);
}

static void controller_cleanup(void *data)
{
	struct controller_fixture *f = data;

	if (f->bus) {
		pci_lock_rescan_remove();
		pci_stop_root_bus(f->bus);
		pci_remove_root_bus(f->bus);
		pci_unlock_rescan_remove();
	}
	root_device_unregister(f->parent);
}

static int controller_init(struct kunit *test)
{
	struct controller_fixture *f;

	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	if (!f)
		return -ENOMEM;
	test->priv = f;
	f->sysdata.domain = CONTROLLER_TEST_DOMAIN;
	f->sysdata.node = NUMA_NO_NODE;
	f->parent = root_device_register("watchguard-pci-test");
	if (IS_ERR(f->parent))
		return PTR_ERR(f->parent);
	return kunit_add_action_or_reset(test, controller_cleanup, f);
}

static void controller_scan(struct kunit *test)
{
	struct controller_fixture *f = test->priv;
	LIST_HEAD(resources);

	f->bus_range = (struct resource) {
		.name = "WatchGuard PCI test buses",
		.start = CONTROLLER_TEST_BUS,
		.end = CONTROLLER_TEST_BUS + 1,
		.flags = IORESOURCE_BUS,
	};
	pci_add_resource(&resources, &f->bus_range);
	f->bus = pci_scan_root_bus(f->parent, CONTROLLER_TEST_BUS,
				   &controller_ops, &f->sysdata, &resources);
	KUNIT_ASSERT_NOT_NULL(test, f->bus);
}

static void controller_selects_m270(struct kunit *test)
{
	struct controller_fixture *f = test->priv;
	struct pci_dev *found;

	controller_add_device(f, 0, 1, PCI_VENDOR_ID_INTEL, 0x19df,
			      PCI_CLASS_SERIAL_SMBUS << 8);
	controller_scan(test);
	found = watchguard_find_smbus(WATCHGUARD_NCB_WG2511);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, found);
	KUNIT_EXPECT_STREQ(test, pci_name(found), "7ffe:f0:01.0");
	pci_dev_put(found);
}

/* A unique expected controller remains usable when it is behind a bridge. */
static void controller_selects_bridged_m370(struct kunit *test)
{
	struct controller_fixture *f = test->priv;
	struct pci_dev *found;
	u8 *bridge = f->config[0][0];

	controller_add_device(f, 0, 0, 0x1b36, 0x0001, PCI_CLASS_BRIDGE_PCI << 8);
	bridge[PCI_HEADER_TYPE] = PCI_HEADER_TYPE_BRIDGE;
	bridge[PCI_PRIMARY_BUS] = CONTROLLER_TEST_BUS;
	bridge[PCI_SECONDARY_BUS] = CONTROLLER_TEST_BUS + 1;
	bridge[PCI_SUBORDINATE_BUS] = CONTROLLER_TEST_BUS + 1;
	bridge[PCI_IO_BASE] = 0xf0;
	controller_set_config(bridge, PCI_MEMORY_BASE, 0x0000fff0);
	controller_set_config(bridge, PCI_PREF_MEMORY_BASE, 0x0000fff0);
	controller_add_device(f, 1, 1, PCI_VENDOR_ID_INTEL, 0xa123,
			      PCI_CLASS_SERIAL_SMBUS << 8);
	controller_scan(test);
	found = watchguard_find_smbus(WATCHGUARD_NCB_4210WG);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, found);
	KUNIT_EXPECT_STREQ(test, pci_name(found), "7ffe:f1:01.0");
	pci_dev_put(found);
}

static void controller_selects_source_described_boards(struct kunit *test)
{
	struct controller_fixture *f = test->priv;
	struct pci_dev *found;

	controller_add_device(f, 0, 1, PCI_VENDOR_ID_INTEL, 0x2292,
			      PCI_CLASS_SERIAL_SMBUS << 8);
	controller_add_device(f, 0, 2, PCI_VENDOR_ID_INTEL, 0x1f3c,
			      PCI_CLASS_SERIAL_SMBUS << 8);
	controller_scan(test);
	found = watchguard_find_smbus(WATCHGUARD_T55);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, found);
	KUNIT_EXPECT_STREQ(test, pci_name(found), "7ffe:f0:01.0");
	pci_dev_put(found);
	found = watchguard_find_smbus(WATCHGUARD_T70);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, found);
	KUNIT_EXPECT_STREQ(test, pci_name(found), "7ffe:f0:01.0");
	pci_dev_put(found);
	found = watchguard_find_smbus(WATCHGUARD_MB_UP2010W);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, found);
	KUNIT_EXPECT_STREQ(test, pci_name(found), "7ffe:f0:02.0");
	pci_dev_put(found);
}

static void controller_rejects_ambiguous_matches(struct kunit *test)
{
	struct controller_fixture *f = test->priv;
	struct pci_dev *found;

	controller_add_device(f, 0, 1, PCI_VENDOR_ID_INTEL, 0x19df,
			      PCI_CLASS_SERIAL_SMBUS << 8);
	controller_add_device(f, 0, 2, PCI_VENDOR_ID_INTEL, 0x19df,
			      PCI_CLASS_SERIAL_SMBUS << 8);
	controller_scan(test);
	found = watchguard_find_smbus(WATCHGUARD_NCB_WG2511);
	KUNIT_EXPECT_EQ(test, PTR_ERR(found), -ENODEV);
	if (!IS_ERR_OR_NULL(found))
		pci_dev_put(found);
}

static void controller_rejects_wrong_class(struct kunit *test)
{
	struct controller_fixture *f = test->priv;
	struct pci_dev *found;

	controller_add_device(f, 0, 1, PCI_VENDOR_ID_INTEL, 0x19df,
			      PCI_CLASS_NETWORK_ETHERNET << 8);
	controller_scan(test);
	found = watchguard_find_smbus(WATCHGUARD_NCB_WG2511);
	KUNIT_EXPECT_EQ(test, PTR_ERR(found), -ENODEV);
	if (!IS_ERR_OR_NULL(found))
		pci_dev_put(found);
}

static void controller_rejects_missing_hardware(struct kunit *test)
{
	struct pci_dev *found;

	controller_scan(test);
	found = watchguard_find_smbus(WATCHGUARD_NCB_WG2511);
	KUNIT_EXPECT_EQ(test, PTR_ERR(found), -ENODEV);
	if (!IS_ERR_OR_NULL(found))
		pci_dev_put(found);
}

static void controller_requires_board_mapping(struct kunit *test)
{
	struct controller_fixture *f = test->priv;
	struct pci_dev *found;

	controller_add_device(f, 0, 1, PCI_VENDOR_ID_INTEL, 0x19df,
			      PCI_CLASS_SERIAL_SMBUS << 8);
	controller_scan(test);
	found = watchguard_find_smbus(WATCHGUARD_MB_WG8758W);
	KUNIT_EXPECT_EQ(test, PTR_ERR(found), -ENODEV);
	if (!IS_ERR_OR_NULL(found))
		pci_dev_put(found);
}

static struct kunit_case controller_cases[] = {
	KUNIT_CASE(controller_selects_m270),
	KUNIT_CASE(controller_selects_bridged_m370),
	KUNIT_CASE(controller_selects_source_described_boards),
	KUNIT_CASE(controller_rejects_ambiguous_matches),
	KUNIT_CASE(controller_rejects_wrong_class),
	KUNIT_CASE(controller_rejects_missing_hardware),
	KUNIT_CASE(controller_requires_board_mapping),
	{ }
};

static struct kunit_suite controller_suite = {
	.name = "watchguard-controller",
	.init = controller_init,
	.test_cases = controller_cases,
};

kunit_test_suite(controller_suite);

MODULE_LICENSE("GPL");
