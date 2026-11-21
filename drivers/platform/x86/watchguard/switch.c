// SPDX-License-Identifier: GPL-2.0-only
/* M270 wiring; switch programming and link management belong to DSA/phylink. */
#include <linux/mdio.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/property.h>

#include "m270.h"

struct ncb_wg2511_switch {
	struct ncb_wg2511_board board;
	struct software_node root;
	struct software_node ports;
	struct software_node mdio_node;
	struct software_node phy[8];
	struct software_node port[10];
	struct software_node fixed[2];
	struct software_node_ref_args phy_ref[8][1];
	struct software_node_ref_args cpu_ref[2][1];
	struct property_entry phy_props[8][3];
	struct property_entry port_props[10][5];
	const struct software_node *nodes[24];
};

static const struct property_entry ncb_wg2511_switch_properties[] = {
	PROPERTY_ENTRY_STRING("compatible", "marvell,mv88e6190"),
	{ }
};

static const struct property_entry ncb_wg2511_fixed_properties[] = {
	PROPERTY_ENTRY_U32("speed", 2500),
	PROPERTY_ENTRY_BOOL("full-duplex"),
	PROPERTY_ENTRY_BOOL("pause"),
	{ }
};

static int ncb_wg2511_mdio_match(struct device *dev,
				 const struct device_driver *drv)
{
	return !strcmp(drv->name, "mv88e6085");
}

static int ncb_wg2511_create_nodes(struct ncb_wg2511_switch *sw)
{
	static const char * const phy_names[] = {
		"ethernet-phy@1", "ethernet-phy@2", "ethernet-phy@3", "ethernet-phy@4",
		"ethernet-phy@5", "ethernet-phy@6", "ethernet-phy@7", "ethernet-phy@8",
	};
	static const char * const port_names[] = {
		"port@1", "port@2", "port@3", "port@4", "port@5",
		"port@6", "port@7", "port@8", "port@9", "port@a",
	};
	static const char * const labels[] = {
		"swp0", "swp1", "swp2", "swp3", "swp4", "swp5", "swp6", "swp7",
	};
	int i, n = 0;

	sw->root.name = "ncb-wg2511-switch";
	sw->root.properties = ncb_wg2511_switch_properties;
	sw->ports.name = "ethernet-ports";
	sw->ports.parent = &sw->root;
	sw->mdio_node.name = "mdio";
	sw->mdio_node.parent = &sw->root;
	sw->nodes[n++] = &sw->root;
	sw->nodes[n++] = &sw->ports;
	sw->nodes[n++] = &sw->mdio_node;
	for (i = 0; i < 8; i++) {
		sw->phy_props[i][0] = PROPERTY_ENTRY_U32("reg", i + 1);
		sw->phy_props[i][1] =
			PROPERTY_ENTRY_STRING("compatible", "ethernet-phy-ieee802.3-c22");
		sw->phy[i].name = phy_names[i];
		sw->phy[i].parent = &sw->mdio_node;
		sw->phy[i].properties = sw->phy_props[i];
		sw->nodes[n++] = &sw->phy[i];
		sw->phy_ref[i][0] = SOFTWARE_NODE_REFERENCE(&sw->phy[i]);
		sw->port_props[i][0] = PROPERTY_ENTRY_U32("reg", i + 1);
		sw->port_props[i][1] = PROPERTY_ENTRY_STRING("label", labels[i]);
		sw->port_props[i][2] = PROPERTY_ENTRY_STRING("phy-mode", "internal");
		sw->port_props[i][3] = PROPERTY_ENTRY_REF_ARRAY("phy-handle", sw->phy_ref[i]);
	}
	for (i = 0; i < 2; i++) {
		sw->cpu_ref[i][0] = SOFTWARE_NODE_REFERENCE(sw->board.nic_fwnode[i]);
		sw->port_props[8 + i][0] = PROPERTY_ENTRY_U32("reg", 9 + i);
		sw->port_props[8 + i][1] = PROPERTY_ENTRY_STRING("phy-mode", "2500base-x");
		sw->port_props[8 + i][2] = PROPERTY_ENTRY_REF_ARRAY("ethernet", sw->cpu_ref[i]);
	}
	for (i = 0; i < 10; i++) {
		sw->port[i].name = port_names[i];
		sw->port[i].parent = &sw->ports;
		sw->port[i].properties = sw->port_props[i];
		sw->nodes[n++] = &sw->port[i];
	}
	for (i = 0; i < 2; i++) {
		sw->fixed[i].name = "fixed-link";
		sw->fixed[i].parent = &sw->port[8 + i];
		sw->fixed[i].properties = ncb_wg2511_fixed_properties;
		sw->nodes[n++] = &sw->fixed[i];
	}
	return software_node_register_node_group(sw->nodes);
}

static void ncb_wg2511_put_board(void *data)
{
	ncb_wg2511_board_put(data);
}

static void ncb_wg2511_remove_nodes(void *data)
{
	struct ncb_wg2511_switch *sw = data;

	software_node_unregister_node_group(sw->nodes);
}

static void ncb_wg2511_remove_mdio(void *data)
{
	struct mdio_device *mdio = data;

	mdio_device_remove(mdio);
	device_remove_software_node(&mdio->dev);
	mdio_device_free(mdio);
}

static int ncb_wg2511_register_mdio(struct device *dev,
				    struct ncb_wg2511_switch *sw)
{
	struct mdio_device *mdio;
	int err;

	if (IS_MODULE(CONFIG_NET_DSA_MV88E6XXX)) {
		err = request_module("mv88e6xxx");
		if (err)
			return err;
	}
	mdio = mdio_device_create(sw->board.bus, 0);
	if (IS_ERR(mdio))
		return PTR_ERR(mdio);
	mdio->bus_match = ncb_wg2511_mdio_match;
	err = device_add_software_node(&mdio->dev, &sw->root);
	if (err)
		goto free_mdio;
	err = mdio_device_register(mdio);
	if (err)
		goto remove_node;
	return devm_add_action_or_reset(dev, ncb_wg2511_remove_mdio, mdio);

remove_node:
	device_remove_software_node(&mdio->dev);
free_mdio:
	mdio_device_free(mdio);
	return err;
}

static int ncb_wg2511_switch_probe(struct platform_device *pdev)
{
	const struct ncb_wg2511_board_data *data = dev_get_platdata(&pdev->dev);
	struct device *dev = &pdev->dev;
	struct ncb_wg2511_switch *sw;
	int err;

	if (!data)
		return -EINVAL;
	sw = devm_kzalloc(dev, sizeof(*sw), GFP_KERNEL);
	if (!sw)
		return -ENOMEM;
	err = ncb_wg2511_board_get(dev, data, &sw->board);
	if (err)
		return err;
	err = devm_add_action_or_reset(dev, ncb_wg2511_put_board, &sw->board);
	if (err)
		return err;
	err = ncb_wg2511_create_nodes(sw);
	if (err)
		return dev_err_probe(dev, err, "switch nodes registration failed\n");
	err = devm_add_action_or_reset(dev, ncb_wg2511_remove_nodes, sw);
	if (err)
		return err;

	err = ncb_wg2511_register_mdio(dev, sw);
	if (err)
		return dev_err_probe(dev, err, "switch registration failed\n");
	return 0;
}

static const struct platform_device_id ncb_wg2511_switch_ids[] = {
	{ NCB_WG2511_SWITCH_DEVICE_NAME, 0 },
	{ }
};
MODULE_DEVICE_TABLE(platform, ncb_wg2511_switch_ids);

static struct platform_driver ncb_wg2511_switch_driver = {
	.probe = ncb_wg2511_switch_probe,
	.id_table = ncb_wg2511_switch_ids,
	.driver.name = NCB_WG2511_SWITCH_DEVICE_NAME,
};

module_platform_driver(ncb_wg2511_switch_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("WatchGuard Firebox M270 Ethernet switch");
