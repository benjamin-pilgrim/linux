// SPDX-License-Identifier: GPL-2.0-only
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/platform_device.h>

#include "board-id.h"
#include "controller.h"
#include "m270.h"
#include "route.h"

#define WATCHGUARD_MODEL_DEVICE_NAME "watchguard-model"

struct watchguard_bootstrap {
	enum watchguard_board board;
	struct pci_dev *smbus;
	struct i2c_adapter *root;
	struct watchguard_route route;
	struct platform_device *model;
};

static int watchguard_model_probe(struct platform_device *pdev)
{
	struct watchguard_bootstrap **board_data = dev_get_platdata(&pdev->dev);
	struct watchguard_bootstrap *bootstrap;
	struct watchguard_identity *identity;
	struct device *supplier;
	int err;

	if (!board_data || !*board_data)
		return -EINVAL;
	bootstrap = *board_data;
	supplier = bootstrap->route.mux ? &bootstrap->route.mux->dev : &bootstrap->smbus->dev;
	if (!device_link_add(&pdev->dev, supplier, DL_FLAG_AUTOPROBE_CONSUMER))
		return -ENOMEM;
	identity = devm_kzalloc(&pdev->dev, sizeof(*identity), GFP_KERNEL);
	if (!identity)
		return -ENOMEM;
	err = watchguard_route_read(&bootstrap->route, identity);
	if (err)
		return dev_err_probe(&pdev->dev, err, "manufacturing identity unavailable\n");

	switch (bootstrap->board) {
	case WATCHGUARD_NCB_WG2511:
		if (identity->model_code != WATCHGUARD_MODEL_M270)
			return -ENODEV;
		err = ncb_wg2511_m270_register_children(&pdev->dev, bootstrap->smbus);
		if (err)
			return err;
		break;
	case WATCHGUARD_NCB_4210WG:
		if (identity->model_code != WATCHGUARD_MODEL_M370)
			return -ENODEV;
		/* M370 Ethernet is enumerated by PCI; no DSA description is needed. */
		break;
	case WATCHGUARD_MB_UP2010W:
	case WATCHGUARD_T55:
	case WATCHGUARD_T70:
		/* Identity-only support; no model-specific setup is described. */
		break;
	default:
		return -ENODEV;
	}
	platform_set_drvdata(pdev, identity);
	return 0;
}

static struct platform_driver watchguard_model_driver = {
	.probe = watchguard_model_probe,
	.driver.name = WATCHGUARD_MODEL_DEVICE_NAME,
};

static int watchguard_board_probe(struct platform_device *pdev)
{
	const enum watchguard_board *board_id = dev_get_platdata(&pdev->dev);
	struct watchguard_bootstrap *bootstrap;
	int err = -EPROBE_DEFER;

	if (!board_id)
		return -EINVAL;
	bootstrap = devm_kzalloc(&pdev->dev, sizeof(*bootstrap), GFP_KERNEL);
	if (!bootstrap)
		return -ENOMEM;
	bootstrap->board = *board_id;
	bootstrap->smbus = watchguard_find_smbus(*board_id);
	if (IS_ERR(bootstrap->smbus))
		return PTR_ERR(bootstrap->smbus);
	if (!dev_fwnode(&bootstrap->smbus->dev)) {
		err = -ENODEV;
		goto put_smbus;
	}
	if (!device_link_add(&pdev->dev, &bootstrap->smbus->dev,
			     DL_FLAG_AUTOPROBE_CONSUMER)) {
		err = -ENOMEM;
		goto put_smbus;
	}
	device_lock(&bootstrap->smbus->dev);
	if (device_is_bound(&bootstrap->smbus->dev)) {
		bootstrap->root =
			i2c_get_adapter_by_fwnode(dev_fwnode(&bootstrap->smbus->dev));
		if (bootstrap->root && bootstrap->root->dev.parent != &bootstrap->smbus->dev) {
			i2c_put_adapter(bootstrap->root);
			bootstrap->root = NULL;
			err = -ENODEV;
		}
	}
	device_unlock(&bootstrap->smbus->dev);
	if (!bootstrap->root)
		goto put_smbus;

	err = watchguard_route_create(*board_id, bootstrap->root, &bootstrap->route);
	if (err)
		goto put_adapter;
	/* Keep mux creation alive while its supplier driver binds asynchronously. */
	bootstrap->model =
		platform_device_register_data(&pdev->dev, WATCHGUARD_MODEL_DEVICE_NAME,
					      PLATFORM_DEVID_NONE, &bootstrap, sizeof(bootstrap));
	if (IS_ERR(bootstrap->model)) {
		err = PTR_ERR(bootstrap->model);
		goto destroy_route;
	}
	platform_set_drvdata(pdev, bootstrap);
	return 0;

destroy_route:
	watchguard_route_destroy(&bootstrap->route);
put_adapter:
	i2c_put_adapter(bootstrap->root);
put_smbus:
	pci_dev_put(bootstrap->smbus);
	return dev_err_probe(&pdev->dev, err, "manufacturing route unavailable\n");
}

static void watchguard_board_remove(struct platform_device *pdev)
{
	struct watchguard_bootstrap *bootstrap = platform_get_drvdata(pdev);

	platform_device_unregister(bootstrap->model);
	watchguard_route_destroy(&bootstrap->route);
	i2c_put_adapter(bootstrap->root);
	pci_dev_put(bootstrap->smbus);
}

static const struct platform_device_id watchguard_board_ids[] = {
	{ WATCHGUARD_BOARD_DEVICE_NAME, 0 },
	{ }
};
MODULE_DEVICE_TABLE(platform, watchguard_board_ids);

static struct platform_driver watchguard_board_driver = {
	.probe = watchguard_board_probe,
	.remove = watchguard_board_remove,
	.id_table = watchguard_board_ids,
	.driver.name = WATCHGUARD_BOARD_DEVICE_NAME,
};

static struct platform_driver * const watchguard_drivers[] = {
	&watchguard_model_driver,
	&watchguard_board_driver,
};

static int __init watchguard_init(void)
{
	return platform_register_drivers(watchguard_drivers, ARRAY_SIZE(watchguard_drivers));
}
module_init(watchguard_init);

static void __exit watchguard_exit(void)
{
	platform_unregister_drivers(watchguard_drivers, ARRAY_SIZE(watchguard_drivers));
}
module_exit(watchguard_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("WatchGuard manufacturing EEPROM and model selection");
