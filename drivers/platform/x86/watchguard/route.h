/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _WATCHGUARD_ROUTE_H
#define _WATCHGUARD_ROUTE_H

#include <linux/property.h>

#include "board-id.h"
#include "identity.h"

struct i2c_adapter;
struct i2c_client;

struct watchguard_route {
	struct i2c_adapter *root;
	struct i2c_client *mux;
	u16 eeprom_address;
	struct software_node nodes[2];
	const struct software_node *group[3];
};

/*
 * Only boards with established controller and access descriptions have routes.
 * The caller holds the root adapter reference until route destruction.
 * Destroy a successful route before creating another in the same storage.
 */
int watchguard_route_create(enum watchguard_board board,
			    struct i2c_adapter *root, struct watchguard_route *route);
int watchguard_route_read(struct watchguard_route *route,
			  struct watchguard_identity *identity);
void watchguard_route_destroy(struct watchguard_route *route);

#endif
