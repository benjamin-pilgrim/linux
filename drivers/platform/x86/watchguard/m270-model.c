// SPDX-License-Identifier: GPL-2.0-only
/* M270 enumeration, called after shared board/model identity validation. */
#include <linux/platform_device.h>
#include <linux/slab.h>

#include "m270.h"

struct m270_devices {
	struct platform_device *panel;
	struct platform_device *switch_device;
};

static void m270_unregister_children(void *data)
{
	struct m270_devices *devices = data;

	if (!IS_ERR_OR_NULL(devices->switch_device))
		platform_device_unregister(devices->switch_device);
	if (!IS_ERR_OR_NULL(devices->panel))
		platform_device_unregister(devices->panel);
}

int ncb_wg2511_m270_register_children(struct device *parent, struct pci_dev *smbus)
{
	struct ncb_wg2511_board_data data = { .smbus = smbus };
	struct m270_devices *devices;
	int err;

	devices = devm_kzalloc(parent, sizeof(*devices), GFP_KERNEL);
	if (!devices)
		return -ENOMEM;
	/* Release children before their private state on failed probe or unbind. */
	err = devm_add_action_or_reset(parent, m270_unregister_children, devices);
	if (err)
		return err;
	devices->panel = platform_device_register_data(parent, NCB_WG2511_PANEL_DEVICE_NAME,
						       PLATFORM_DEVID_NONE, NULL, 0);
	if (IS_ERR(devices->panel))
		return PTR_ERR(devices->panel);
	devices->switch_device =
		platform_device_register_data(parent, NCB_WG2511_SWITCH_DEVICE_NAME,
					      PLATFORM_DEVID_NONE, &data, sizeof(data));
	return PTR_ERR_OR_ZERO(devices->switch_device);
}
