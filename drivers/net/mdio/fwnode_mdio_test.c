// SPDX-License-Identifier: GPL-2.0-only
/* Exercise firmware-described PHY registration without physical hardware. */
#include <kunit/test.h>
#include <linux/device.h>
#include <linux/fwnode_mdio.h>
#include <linux/mii.h>
#include <linux/phy.h>
#include <linux/property.h>

struct fwnode_mdio_fixture {
	struct mii_bus *bus;
	struct device *parent;
	struct software_node node;
	int read_error;
};

static int fwnode_mdio_test_read(struct mii_bus *bus, int addr, int reg)
{
	struct fwnode_mdio_fixture *fixture = bus->priv;

	if (fixture->read_error)
		return fixture->read_error;
	if (addr != 1)
		return 0xffff;
	if (reg == MII_PHYSID1)
		return 0x1234;
	if (reg == MII_PHYSID2)
		return 0x5678;
	return 0;
}

static int fwnode_mdio_test_write(struct mii_bus *bus, int addr, int reg, u16 val)
{
	return 0;
}

static void fwnode_mdio_test_cleanup(void *data)
{
	struct fwnode_mdio_fixture *fixture = data;

	mdiobus_unregister(fixture->bus);
	mdiobus_free(fixture->bus);
	software_node_unregister(&fixture->node);
	root_device_unregister(fixture->parent);
}

static int fwnode_mdio_test_init(struct kunit *test)
{
	static const struct property_entry properties[] = {
		PROPERTY_ENTRY_STRING("compatible", "ethernet-phy-ieee802.3-c22"),
		{ }
	};
	struct fwnode_mdio_fixture *fixture;
	int err;

	fixture = kunit_kzalloc(test, sizeof(*fixture), GFP_KERNEL);
	if (!fixture)
		return -ENOMEM;
	test->priv = fixture;
	fixture->parent = root_device_register("fwnode-mdio-test");
	if (IS_ERR(fixture->parent))
		return PTR_ERR(fixture->parent);
	fixture->node.name = "fwnode-mdio-test-phy";
	fixture->node.properties = properties;
	err = software_node_register(&fixture->node);
	if (err)
		goto unregister_parent;
	fixture->bus = mdiobus_alloc();
	if (!fixture->bus) {
		err = -ENOMEM;
		goto unregister_node;
	}
	fixture->bus->name = "fwnode MDIO test";
	strscpy(fixture->bus->id, "fwnode-mdio-test");
	fixture->bus->parent = fixture->parent;
	fixture->bus->priv = fixture;
	fixture->bus->read = fwnode_mdio_test_read;
	fixture->bus->write = fwnode_mdio_test_write;
	fixture->bus->phy_mask = ~0U;
	err = mdiobus_register(fixture->bus);
	if (err)
		goto free_bus;
	return kunit_add_action_or_reset(test, fwnode_mdio_test_cleanup, fixture);

free_bus:
	mdiobus_free(fixture->bus);
unregister_node:
	software_node_unregister(&fixture->node);
unregister_parent:
	root_device_unregister(fixture->parent);
	return err;
}

/* Firmware PHY references must resolve to the registered bus device. */
static void software_node_phy_is_registered(struct kunit *test)
{
	struct fwnode_mdio_fixture *fixture = test->priv;
	struct fwnode_handle *node = software_node_fwnode(&fixture->node);
	struct phy_device *phy;

	KUNIT_ASSERT_EQ(test, fwnode_mdiobus_register_phy(fixture->bus, node, 1), 0);
	phy = fwnode_phy_find_device(node);
	KUNIT_ASSERT_NOT_NULL(test, phy);
	KUNIT_EXPECT_PTR_EQ(test, phy, mdiobus_get_phy(fixture->bus, 1));
	KUNIT_EXPECT_EQ(test, phy->phy_id, 0x12345678U);
	put_device(&phy->mdio.dev);
}

/* Failed registration must leave an existing PHY and its node intact. */
static void software_node_phy_preserves_occupied_address(struct kunit *test)
{
	struct fwnode_mdio_fixture *fixture = test->priv;
	struct fwnode_handle *node = software_node_fwnode(&fixture->node);
	struct phy_device *phy, *found;

	KUNIT_ASSERT_EQ(test, fwnode_mdiobus_register_phy(fixture->bus, node, 1), 0);
	phy = mdiobus_get_phy(fixture->bus, 1);
	KUNIT_ASSERT_NOT_NULL(test, phy);
	KUNIT_EXPECT_EQ(test, fwnode_mdiobus_register_phy(fixture->bus, node, 1),
			-EBUSY);
	found = fwnode_phy_find_device(node);
	KUNIT_EXPECT_PTR_EQ(test, found, phy);
	if (found)
		put_device(&found->mdio.dev);
}

/* MDIO read failure must not publish a PHY; a later retry may succeed. */
static void software_node_phy_can_retry_read_failure(struct kunit *test)
{
	struct fwnode_mdio_fixture *fixture = test->priv;
	struct fwnode_handle *node = software_node_fwnode(&fixture->node);
	struct phy_device *phy;

	fixture->read_error = -ETIMEDOUT;
	KUNIT_EXPECT_EQ(test, fwnode_mdiobus_register_phy(fixture->bus, node, 1),
			-EIO);
	phy = fwnode_phy_find_device(node);
	KUNIT_EXPECT_PTR_EQ(test, phy, NULL);
	if (phy)
		put_device(&phy->mdio.dev);
	KUNIT_EXPECT_PTR_EQ(test, mdiobus_get_phy(fixture->bus, 1), NULL);
	fixture->read_error = 0;
	KUNIT_ASSERT_EQ(test, fwnode_mdiobus_register_phy(fixture->bus, node, 1), 0);
	KUNIT_EXPECT_NOT_NULL(test, mdiobus_get_phy(fixture->bus, 1));
}

static struct kunit_case fwnode_mdio_test_cases[] = {
	KUNIT_CASE(software_node_phy_is_registered),
	KUNIT_CASE(software_node_phy_preserves_occupied_address),
	KUNIT_CASE(software_node_phy_can_retry_read_failure),
	{ }
};

static struct kunit_suite fwnode_mdio_test_suite = {
	.name = "fwnode-mdio",
	.init = fwnode_mdio_test_init,
	.test_cases = fwnode_mdio_test_cases,
};

kunit_test_suite(fwnode_mdio_test_suite);

MODULE_LICENSE("GPL");
