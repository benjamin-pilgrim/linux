// SPDX-License-Identifier: GPL-2.0-only
#include <linux/i2c.h>
#include <linux/property.h>

#include "route.h"

static const struct property_entry watchguard_mux_properties[] = {
	PROPERTY_ENTRY_BOOL("i2c-mux-idle-disconnect"),
	{ }
};

static const struct property_entry watchguard_channel_properties[] = {
	PROPERTY_ENTRY_U32("reg", 3),
	{ }
};

int watchguard_route_create(enum watchguard_board board,
			    struct i2c_adapter *root, struct watchguard_route *route)
{
	struct i2c_board_info info = { I2C_BOARD_INFO("pca9546", 0x73) };
	int err;

	memset(route, 0, sizeof(*route));
	switch (board) {
	case WATCHGUARD_NCB_WG2511:
	case WATCHGUARD_MB_UP2010W:
	case WATCHGUARD_T55:
	case WATCHGUARD_T70:
		route->eeprom_address = 0x56;
		route->root = root;
		return 0;
	case WATCHGUARD_NCB_4210WG:
		break;
	default:
		return -ENODEV;
	}

	/* Four-channel PCA954x-compatible controls; fitted manufacturer unknown. */
	route->nodes[0].name = "watchguard-management-mux";
	route->nodes[0].properties = watchguard_mux_properties;
	route->nodes[1].name = "channel-3";
	route->nodes[1].parent = &route->nodes[0];
	route->nodes[1].properties = watchguard_channel_properties;
	route->group[0] = &route->nodes[0];
	route->group[1] = &route->nodes[1];
	err = software_node_register_node_group(route->group);
	if (err)
		return err;
	info.swnode = &route->nodes[0];
	route->mux = i2c_new_client_device(root, &info);
	if (IS_ERR(route->mux)) {
		err = PTR_ERR(route->mux);
		route->mux = NULL;
		software_node_unregister_node_group(route->group);
		return err;
	}
	route->eeprom_address = 0x56;
	route->root = root;
	return 0;
}

int watchguard_route_read(struct watchguard_route *route,
			  struct watchguard_identity *identity)
{
	struct i2c_adapter *adapter;
	int err = -EPROBE_DEFER;

	if (!route->root)
		return -ENODEV;
	if (!route->mux)
		return watchguard_read_identity(route->root, route->eeprom_address,
						identity);

	device_lock(&route->mux->dev);
	if (!device_is_bound(&route->mux->dev))
		goto unlock;
	adapter = i2c_get_adapter_by_fwnode(software_node_fwnode(&route->nodes[1]));
	if (!adapter)
		goto unlock;
	err = watchguard_read_identity(adapter, route->eeprom_address, identity);
	i2c_put_adapter(adapter);
unlock:
	device_unlock(&route->mux->dev);
	return err;
}

void watchguard_route_destroy(struct watchguard_route *route)
{
	if (route->mux) {
		i2c_unregister_device(route->mux);
		software_node_unregister_node_group(route->group);
		route->mux = NULL;
	}
	route->root = NULL;
}
