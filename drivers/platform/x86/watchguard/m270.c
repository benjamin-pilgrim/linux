// SPDX-License-Identifier: GPL-2.0-only
/* M270 PCI and MDIO suppliers; manufacturing identity is already validated. */
#include <linux/mdio.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/pci.h>
#include <linux/phy.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include "m270.h"

#define M270_X553_DEVICE_ID 0x15c2
#define M270_LAN_ROOT_PORT_DEVICE_ID 0x19d1
#define M270_SWITCH_PRODUCT_ID 0x1900

static struct pci_dev *ncb_wg2511_find_nic(struct pci_dev *smbus)
{
	struct pci_dev *candidate = NULL, *found = NULL;
	struct pci_dev *root, *peer;

	while ((candidate = pci_get_device(PCI_VENDOR_ID_INTEL, M270_X553_DEVICE_ID,
					   candidate))) {
		if (PCI_FUNC(candidate->devfn) != 0)
			continue;
		root = candidate->bus->self;
		if (!root || root->vendor != PCI_VENDOR_ID_INTEL ||
		    root->device != M270_LAN_ROOT_PORT_DEVICE_ID ||
		    root->bus != smbus->bus)
			continue;
		peer = pci_get_slot(candidate->bus,
				    PCI_DEVFN(PCI_SLOT(candidate->devfn), 1));
		if (!peer)
			continue;
		if (peer->vendor != PCI_VENDOR_ID_INTEL ||
		    peer->device != M270_X553_DEVICE_ID) {
			pci_dev_put(peer);
			continue;
		}
		pci_dev_put(peer);
		if (found) {
			pci_dev_put(candidate);
			pci_dev_put(found);
			return ERR_PTR(-ENODEV);
		}
		found = pci_dev_get(candidate);
	}
	return found ? found : ERR_PTR(-ENODEV);
}

static int ncb_wg2511_match_class(struct device *dev, const void *name)
{
	return dev->class && !strcmp(dev->class->name, name);
}

static struct mii_bus *ncb_wg2511_find_mdio(struct pci_dev *nic)
{
	struct device *netdev, *busdev;
	struct mii_bus *bus;

	netdev = device_find_child(&nic->dev, "net", ncb_wg2511_match_class);
	if (!netdev)
		return ERR_PTR(-EPROBE_DEFER);
	busdev = device_find_child(netdev, "mdio_bus", ncb_wg2511_match_class);
	put_device(netdev);
	if (!busdev)
		return ERR_PTR(-EPROBE_DEFER);
	bus = to_mii_bus(busdev);
	if (strcmp(bus->name, "ixgbe-mdio") ||
	    bus->state != MDIOBUS_REGISTERED) {
		put_device(busdev);
		return ERR_PTR(-ENODEV);
	}
	return bus;
}

int ncb_wg2511_board_get(struct device *dev,
			 const struct ncb_wg2511_board_data *data,
			 struct ncb_wg2511_board *board)
{
	struct pci_dev *nic, *smbus = data->smbus, *peer;
	struct mii_bus *bus;
	struct device *netdev;
	int port, id, err;

	memset(board, 0, sizeof(*board));

	/* The manufacturing model has selected the M270 child device. */
	nic = ncb_wg2511_find_nic(smbus);
	if (IS_ERR(nic)) {
		err = PTR_ERR(nic);
		dev_err_probe(dev, err, "M270 X553 PCI pair unavailable\n");
		return err;
	}
	peer = pci_get_slot(nic->bus, PCI_DEVFN(PCI_SLOT(nic->devfn), 1));
	if (!peer) {
		err = -ENODEV;
		goto put_nic;
	}
	if (!device_link_add(dev, &nic->dev, DL_FLAG_AUTOPROBE_CONSUMER) ||
	    !device_link_add(dev, &peer->dev, DL_FLAG_AUTOPROBE_CONSUMER)) {
		err = -ENOMEM;
		goto put_peer;
	}
	/* Keep both NIC suppliers stable while inspecting their children. */
	device_lock(&nic->dev);
	device_lock(&peer->dev);
	if (!device_is_bound(&nic->dev) || !device_is_bound(&peer->dev)) {
		err = -EPROBE_DEFER;
		goto unlock;
	}
	bus = ncb_wg2511_find_mdio(nic);
	if (IS_ERR(bus)) {
		err = PTR_ERR(bus);
		goto unlock;
	}

	/* Check the expected chip once; mv88e6xxx owns port discovery. */
	id = mdiobus_read(bus, 0, 3);
	/* Transport arbitration can recover after reporting busy. */
	if (id == -EBUSY) {
		dev_info(dev, "MDIO busy; retrying switch ID\n");
		id = mdiobus_read(bus, 0, 3);
	}
	if (id < 0 || (id & 0xfff0) != M270_SWITCH_PRODUCT_ID) {
		err = id < 0 ? id : -ENODEV;
		goto put_bus;
	}
	for (port = 0; port < 2; port++) {
		struct device *supplier = port ? &peer->dev : &nic->dev;

		netdev = device_find_child(supplier, "net", ncb_wg2511_match_class);
		if (!netdev) {
			err = -EPROBE_DEFER;
			goto put_bus;
		}
		board->nic_fwnode[port] = fwnode_handle_get(dev_fwnode(supplier));
		put_device(netdev);
		if (!board->nic_fwnode[port]) {
			err = -ENODEV;
			goto put_bus;
		}
	}
	board->bus = bus;
	get_device(&bus->dev);
	err = 0;
	dev_info(dev, "M270 detected: X553 %s, 88E6190 on %s\n",
		 pci_name(nic), bus->id);
put_bus:
	put_device(&bus->dev);
unlock:
	device_unlock(&peer->dev);
	device_unlock(&nic->dev);
put_peer:
	pci_dev_put(peer);
put_nic:
	pci_dev_put(nic);
	if (err)
		ncb_wg2511_board_put(board);
	return err ? dev_err_probe(dev, err, "board verification incomplete\n") : 0;
}

void ncb_wg2511_board_put(struct ncb_wg2511_board *board)
{
	int i;

	for (i = 0; i < 2; i++) {
		fwnode_handle_put(board->nic_fwnode[i]);
		board->nic_fwnode[i] = NULL;
	}
	if (board->bus)
		put_device(&board->bus->dev);
	board->bus = NULL;
}
