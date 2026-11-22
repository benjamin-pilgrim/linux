// SPDX-License-Identifier: GPL-2.0-only
/* Register a synthetic switch through the public DSA interface. */
#include <kunit/test.h>
#include <linux/user_namespace.h>
#include <linux/etherdevice.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/sockios.h>
#include <net/dsa.h>
#include <net/net_namespace.h>

struct dsa_fwnode_fixture {
	struct platform_device *pdev;
	struct device *parent[2];
	struct net_device *nic[3];
	struct software_node identity[2];
	struct software_node node[8];
	const struct software_node *nodes[9];
	struct software_node_ref_args ref[2][1];
	struct property_entry cpu_props[2][4];
	struct dsa_switch ds;
	struct net *net;
	bool nodes_registered;
	bool switch_registered;
	int setup_error;
	bool omit_cpu_fixed;
	bool duplicate_port;
};

static const struct net_device_ops dsa_fwnode_netdev_ops;

static void dsa_fwnode_cleanup(void *data)
{
	struct dsa_fwnode_fixture *f = data;
	int i;

	if (f->switch_registered)
		dsa_unregister_switch(&f->ds);
	if (f->nodes_registered) {
		device_set_node(&f->pdev->dev, NULL);
		software_node_unregister_node_group(f->nodes);
	}
	for (i = 0; i < ARRAY_SIZE(f->nic); i++) {
		if (!f->nic[i])
			continue;
		unregister_netdev(f->nic[i]);
		free_netdev(f->nic[i]);
	}
	for (i = 0; i < ARRAY_SIZE(f->parent); i++) {
		if (!f->parent[i])
			continue;
		device_remove_software_node(f->parent[i]);
		root_device_unregister(f->parent[i]);
	}
	if (!IS_ERR_OR_NULL(f->net))
		put_net(f->net);
	platform_device_unregister(f->pdev);
}

static int dsa_fwnode_add_nic(struct dsa_fwnode_fixture *f, int i, int parent)
{
	struct net_device *nic;
	int err;

	nic = alloc_netdev(0, "fwcon%d", NET_NAME_UNKNOWN, ether_setup);
	if (!nic)
		return -ENOMEM;
	nic->netdev_ops = &dsa_fwnode_netdev_ops;
	eth_hw_addr_random(nic);
	SET_NETDEV_DEV(nic, f->parent[parent]);
	err = register_netdev(nic);
	if (err) {
		free_netdev(nic);
		return err;
	}
	f->nic[i] = nic;
	return 0;
}

static enum dsa_tag_protocol dsa_fwnode_tag_protocol(struct dsa_switch *ds,
						     int port,
						     enum dsa_tag_protocol proto)
{
	return DSA_TAG_PROTO_NONE;
}

static int dsa_fwnode_setup(struct dsa_switch *ds)
{
	struct dsa_fwnode_fixture *f = ds->priv;

	return f->setup_error;
}

static void dsa_fwnode_get_caps(struct dsa_switch *ds, int port,
				struct phylink_config *config)
{
	__set_bit(PHY_INTERFACE_MODE_GMII, config->supported_interfaces);
	__set_bit(PHY_INTERFACE_MODE_2500BASEX, config->supported_interfaces);
	config->mac_capabilities = MAC_1000FD | MAC_2500FD | MAC_SYM_PAUSE;
}

static const struct dsa_switch_ops dsa_fwnode_ops = {
	.get_tag_protocol = dsa_fwnode_tag_protocol,
	.setup = dsa_fwnode_setup,
	.phylink_get_caps = dsa_fwnode_get_caps,
};

static int dsa_fwnode_probe(struct platform_device *pdev)
{
	return 0;
}

static struct platform_driver dsa_fwnode_driver = {
	.probe = dsa_fwnode_probe,
	.driver.name = "dsa-fwnode-test",
};

static int dsa_fwnode_suite_init(struct kunit_suite *suite)
{
	return platform_driver_register(&dsa_fwnode_driver);
}

static void dsa_fwnode_suite_exit(struct kunit_suite *suite)
{
	platform_driver_unregister(&dsa_fwnode_driver);
}

static int dsa_fwnode_init(struct kunit *test)
{
	static const char * const names[] = { "dsa-fw-lan0", "dsa-fw-lan1" };
	struct dsa_fwnode_fixture *f;
	int i, err;

	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	if (!f)
		return -ENOMEM;
	test->priv = f;
	f->pdev = platform_device_register_simple("dsa-fwnode-test", -1, NULL, 0);
	if (IS_ERR(f->pdev))
		return PTR_ERR(f->pdev);
	err = kunit_add_action_or_reset(test, dsa_fwnode_cleanup, f);
	if (err)
		return err;
	for (i = 0; i < 2; i++) {
		f->parent[i] = root_device_register(names[i]);
		if (IS_ERR(f->parent[i])) {
			err = PTR_ERR(f->parent[i]);
			f->parent[i] = NULL;
			return err;
		}
		f->identity[i].name = names[i];
		err = device_add_software_node(f->parent[i], &f->identity[i]);
		if (err)
			return err;
		err = dsa_fwnode_add_nic(f, i, i);
		if (err)
			return err;
	}
	f->ds.dev = &f->pdev->dev;
	f->ds.num_ports = 3;
	f->ds.ops = &dsa_fwnode_ops;
	f->ds.priv = f;
	return 0;
}

static int dsa_fwnode_topology(struct dsa_fwnode_fixture *f, bool second_only,
			       bool invalid_port)
{
	static const struct property_entry user_props[] = {
		PROPERTY_ENTRY_U32("reg", 0),
		PROPERTY_ENTRY_STRING("label", "fwuser%d"),
		PROPERTY_ENTRY_STRING("phy-mode", "gmii"),
		{ }
	};
	static const struct property_entry user_fixed[] = {
		PROPERTY_ENTRY_U32("speed", 1000),
		PROPERTY_ENTRY_BOOL("full-duplex"),
		{ }
	};
	static const struct property_entry cpu_fixed[] = {
		PROPERTY_ENTRY_U32("speed", 2500),
		PROPERTY_ENTRY_BOOL("full-duplex"),
		{ }
	};
	int i, n = 0, err;

	f->node[0].name = "switch";
	f->node[1].name = "ports";
	f->node[1].parent = &f->node[0];
	f->node[2].name = "port@0";
	f->node[2].parent = &f->node[1];
	f->node[2].properties = user_props;
	f->node[3].name = "fixed-link";
	f->node[3].parent = &f->node[2];
	f->node[3].properties = user_fixed;
	for (i = 0; i < 4; i++)
		f->nodes[n++] = &f->node[i];
	for (i = second_only ? 1 : 0; i < 2; i++) {
		struct software_node *cpu = &f->node[4 + 2 * i];
		struct software_node *fixed = &f->node[5 + 2 * i];

		f->ref[i][0] = SOFTWARE_NODE_REFERENCE(dev_fwnode(f->parent[i]));
		f->cpu_props[i][0] = PROPERTY_ENTRY_U32("reg", invalid_port ? 3 :
						f->duplicate_port ? 1 : i + 1);
		f->cpu_props[i][1] = PROPERTY_ENTRY_STRING("phy-mode", "2500base-x");
		f->cpu_props[i][2] = PROPERTY_ENTRY_REF_ARRAY("ethernet", f->ref[i]);
		cpu->name = i ? "port@2" : "port@1";
		cpu->parent = &f->node[1];
		cpu->properties = f->cpu_props[i];
		fixed->name = "fixed-link";
		fixed->parent = cpu;
		fixed->properties = cpu_fixed;
		f->nodes[n++] = cpu;
		if (!f->omit_cpu_fixed)
			f->nodes[n++] = fixed;
	}
	err = software_node_register_node_group(f->nodes);
	if (err)
		return err;
	f->nodes_registered = true;
	device_set_node(&f->pdev->dev, software_node_fwnode(&f->node[0]));
	return 0;
}

static int dsa_fwnode_register(struct dsa_fwnode_fixture *f)
{
	int err = dsa_register_switch(&f->ds);

	if (!err)
		f->switch_registered = true;
	return err;
}

/* Registration publishes a user interface using the intended CPU conduit. */
static void dsa_fwnode_registers_ports(struct kunit *test)
{
	struct dsa_fwnode_fixture *f = test->priv;
	struct net_device *user;

	KUNIT_ASSERT_EQ(test, dsa_fwnode_topology(f, false, false), 0);
	KUNIT_ASSERT_EQ(test, dsa_fwnode_register(f), 0);
	user = dev_get_by_name(&init_net, "fwuser0");
	KUNIT_ASSERT_NOT_NULL(test, user);
	KUNIT_EXPECT_EQ(test, dev_get_iflink(user), f->nic[0]->ifindex);
	dev_put(user);
}

/* A topology with only the second CPU port must select the second NIC. */
static void dsa_fwnode_selects_second_conduit(struct kunit *test)
{
	struct dsa_fwnode_fixture *f = test->priv;
	struct net_device *user;

	KUNIT_ASSERT_EQ(test, dsa_fwnode_topology(f, true, false), 0);
	KUNIT_ASSERT_EQ(test, dsa_fwnode_register(f), 0);
	user = dev_get_by_name(&init_net, "fwuser0");
	KUNIT_ASSERT_NOT_NULL(test, user);
	KUNIT_EXPECT_EQ(test, dev_get_iflink(user), f->nic[1]->ifindex);
	dev_put(user);
}

/* A supplier without a registered NIC is a deferred probe, not a user port. */
static void dsa_fwnode_defers_missing_conduit(struct kunit *test)
{
	struct dsa_fwnode_fixture *f = test->priv;

	KUNIT_ASSERT_EQ(test, dsa_fwnode_topology(f, false, false), 0);
	unregister_netdev(f->nic[0]);
	free_netdev(f->nic[0]);
	f->nic[0] = NULL;
	KUNIT_EXPECT_EQ(test, dsa_fwnode_register(f), -EPROBE_DEFER);
	KUNIT_ASSERT_EQ(test, dsa_fwnode_add_nic(f, 0, 0), 0);
	KUNIT_EXPECT_EQ(test, dsa_fwnode_register(f), 0);
}

/* Shared identity must fail rather than choose a conduit by registration order. */
static void dsa_fwnode_rejects_ambiguous_conduit(struct kunit *test)
{
	struct dsa_fwnode_fixture *f = test->priv;

	KUNIT_ASSERT_EQ(test, dsa_fwnode_topology(f, false, false), 0);
	KUNIT_ASSERT_EQ(test, dsa_fwnode_add_nic(f, 2, 0), 0);
	KUNIT_EXPECT_EQ(test, dsa_fwnode_register(f), -ENOTUNIQ);
}

/* A description must not address a port beyond the driver's advertised range. */
static void dsa_fwnode_rejects_invalid_port(struct kunit *test)
{
	struct dsa_fwnode_fixture *f = test->priv;

	KUNIT_ASSERT_EQ(test, dsa_fwnode_topology(f, false, true), 0);
	KUNIT_EXPECT_EQ(test, dsa_fwnode_register(f), -EINVAL);
}

/* A failed switch setup must release its CPU references and allow a retry. */
static void dsa_fwnode_retries_failed_setup(struct kunit *test)
{
	struct dsa_fwnode_fixture *f = test->priv;

	KUNIT_ASSERT_EQ(test, dsa_fwnode_topology(f, false, false), 0);
	f->setup_error = -EIO;
	KUNIT_ASSERT_EQ(test, dsa_fwnode_register(f), -EIO);
	f->setup_error = 0;
	KUNIT_EXPECT_EQ(test, dsa_fwnode_register(f), 0);
}

/* Interface names must not change firmware-reference resolution. */
static void dsa_fwnode_survives_rename(struct kunit *test)
{
	struct dsa_fwnode_fixture *f = test->priv;
	struct ifreq request = {};
	struct net_device *user;

	strscpy(request.ifr_name, f->nic[0]->name);
	strscpy(request.ifr_newname, "dsa-renamed0");
	KUNIT_ASSERT_EQ(test, dev_ioctl(&init_net, SIOCSIFNAME, &request, NULL, NULL), 0);
	KUNIT_ASSERT_EQ(test, dsa_fwnode_topology(f, false, false), 0);
	KUNIT_ASSERT_EQ(test, dsa_fwnode_register(f), 0);
	user = dev_get_by_name(&init_net, "fwuser0");
	KUNIT_ASSERT_NOT_NULL(test, user);
	KUNIT_EXPECT_EQ(test, dev_get_iflink(user), f->nic[0]->ifindex);
	dev_put(user);
}

/* Software-described shared ports require explicit link management. */
static void dsa_fwnode_rejects_missing_link(struct kunit *test)
{
	struct dsa_fwnode_fixture *f = test->priv;

	f->omit_cpu_fixed = true;
	KUNIT_ASSERT_EQ(test, dsa_fwnode_topology(f, false, false), 0);
	KUNIT_EXPECT_EQ(test, dsa_fwnode_register(f), -EINVAL);
}

/* Two descriptions must not overwrite the same physical port. */
static void dsa_fwnode_rejects_duplicate_port(struct kunit *test)
{
	struct dsa_fwnode_fixture *f = test->priv;

	f->duplicate_port = true;
	KUNIT_ASSERT_EQ(test, dsa_fwnode_topology(f, false, false), 0);
	KUNIT_EXPECT_EQ(test, dsa_fwnode_register(f), -EINVAL);
}

/* Removal retracts the user interface and permits registration again. */
static void dsa_fwnode_registers_after_removal(struct kunit *test)
{
	struct dsa_fwnode_fixture *f = test->priv;
	struct net_device *user;

	KUNIT_ASSERT_EQ(test, dsa_fwnode_topology(f, false, false), 0);
	KUNIT_ASSERT_EQ(test, dsa_fwnode_register(f), 0);
	dsa_unregister_switch(&f->ds);
	f->switch_registered = false;
	user = dev_get_by_name(&init_net, "fwuser0");
	KUNIT_EXPECT_PTR_EQ(test, user, NULL);
	if (user)
		dev_put(user);
	KUNIT_EXPECT_EQ(test, dsa_fwnode_register(f), 0);
}

/* Firmware identity must remain discoverable after a MAC changes namespace. */
static void dsa_fwnode_conduit_in_other_netns(struct kunit *test)
{
	struct dsa_fwnode_fixture *f = test->priv;
	int err;

	if (!IS_ENABLED(CONFIG_NET_NS)) {
		kunit_skip(test, "network namespaces are disabled");
		return;
	}

	f->net = copy_net_ns(CLONE_NEWNET, &init_user_ns, &init_net);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, f->net);
	rtnl_lock();
	err = dev_change_net_namespace(f->nic[0], f->net, NULL);
	rtnl_unlock();
	KUNIT_ASSERT_EQ(test, err, 0);
	KUNIT_ASSERT_EQ(test, dsa_fwnode_topology(f, false, false), 0);
	KUNIT_EXPECT_EQ(test, dsa_fwnode_register(f), 0);
}

static struct kunit_case dsa_fwnode_cases[] = {
	KUNIT_CASE(dsa_fwnode_conduit_in_other_netns),
	KUNIT_CASE(dsa_fwnode_registers_ports),
	KUNIT_CASE(dsa_fwnode_survives_rename),
	KUNIT_CASE(dsa_fwnode_rejects_missing_link),
	KUNIT_CASE(dsa_fwnode_rejects_duplicate_port),
	KUNIT_CASE(dsa_fwnode_registers_after_removal),
	KUNIT_CASE(dsa_fwnode_selects_second_conduit),
	KUNIT_CASE(dsa_fwnode_defers_missing_conduit),
	KUNIT_CASE(dsa_fwnode_rejects_ambiguous_conduit),
	KUNIT_CASE(dsa_fwnode_rejects_invalid_port),
	KUNIT_CASE(dsa_fwnode_retries_failed_setup),
	{ }
};

static struct kunit_suite dsa_fwnode_suite = {
	.name = "dsa-fwnode",
	.suite_init = dsa_fwnode_suite_init,
	.suite_exit = dsa_fwnode_suite_exit,
	.init = dsa_fwnode_init,
	.test_cases = dsa_fwnode_cases,
};

kunit_test_suite(dsa_fwnode_suite);
MODULE_LICENSE("GPL");
