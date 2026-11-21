/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _WATCHGUARD_M270_H
#define _WATCHGUARD_M270_H

#define NCB_WG2511_PANEL_DEVICE_NAME "ncb-wg2511-panel"
#define NCB_WG2511_SWITCH_DEVICE_NAME "ncb-wg2511-switch"

struct device;
struct mii_bus;
struct fwnode_handle;
struct pci_dev;

struct ncb_wg2511_board {
	struct mii_bus *bus;
	struct fwnode_handle *nic_fwnode[2];
};

struct ncb_wg2511_board_data {
	struct pci_dev *smbus;
};

/* Managed children belong to the identity consumer until probe failure/unbind. */
int ncb_wg2511_m270_register_children(struct device *parent, struct pci_dev *smbus);

int ncb_wg2511_board_get(struct device *dev,
			 const struct ncb_wg2511_board_data *data,
			 struct ncb_wg2511_board *board);
void ncb_wg2511_board_put(struct ncb_wg2511_board *board);

#endif
