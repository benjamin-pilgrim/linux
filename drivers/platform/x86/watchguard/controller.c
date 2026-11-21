// SPDX-License-Identifier: GPL-2.0-only
#include <linux/pci.h>

#include "controller.h"

#define WATCHGUARD_DNV_SMBUS_ID 0x19df
#define WATCHGUARD_SPT_SMBUS_ID 0xa123
#define WATCHGUARD_C2000_SMBUS_ID 0x1f3c
#define WATCHGUARD_BSW_SMBUS_ID 0x2292

static const struct {
	enum watchguard_board board;
	u16 device;
} watchguard_smbus_ids[] = {
	{ WATCHGUARD_NCB_WG2511, WATCHGUARD_DNV_SMBUS_ID },
	{ WATCHGUARD_NCB_4210WG, WATCHGUARD_SPT_SMBUS_ID },
	{ WATCHGUARD_MB_UP2010W, WATCHGUARD_C2000_SMBUS_ID },
	{ WATCHGUARD_T55, WATCHGUARD_BSW_SMBUS_ID },
	{ WATCHGUARD_T70, WATCHGUARD_BSW_SMBUS_ID },
};

struct pci_dev *watchguard_find_smbus(enum watchguard_board board)
{
	struct pci_dev *candidate = NULL, *found = NULL;
	unsigned int i;
	u16 id;

	for (i = 0; i < ARRAY_SIZE(watchguard_smbus_ids); i++)
		if (watchguard_smbus_ids[i].board == board)
			break;
	if (i == ARRAY_SIZE(watchguard_smbus_ids))
		return ERR_PTR(-ENODEV);
	id = watchguard_smbus_ids[i].device;

	while ((candidate = pci_get_device(PCI_VENDOR_ID_INTEL, id, candidate))) {
		if ((candidate->class >> 8) != PCI_CLASS_SERIAL_SMBUS)
			continue;
		if (found) {
			pci_dev_put(candidate);
			pci_dev_put(found);
			return ERR_PTR(-ENODEV);
		}
		found = pci_dev_get(candidate);
	}
	return found ? found : ERR_PTR(-ENODEV);
}
