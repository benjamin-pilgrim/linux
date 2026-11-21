// SPDX-License-Identifier: GPL-2.0-only
/* M270 Guard indicator and Reset button over the native GPIO consumers. */
#include <linux/cleanup.h>
#include <linux/gpio/driver.h>
#include <linux/gpio/machine.h>
#include <linux/gpio/property.h>
#include <linux/input-event-codes.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/property.h>

#include "m270.h"

static const struct software_node m270_gpio7 = {
	.name = "gpio-f7188x-7",
};

static const struct software_node m270_leds = {
	.name = "m270-leds",
};

static const struct property_entry m270_green_properties[] = {
	PROPERTY_ENTRY_STRING("label", "m270:green:guard"),
	PROPERTY_ENTRY_GPIO("gpios", &m270_gpio7, 2, GPIO_ACTIVE_HIGH),
	PROPERTY_ENTRY_STRING("default-state", "keep"),
	{ }
};

static const struct software_node m270_green = {
	.name = "guard-green",
	.parent = &m270_leds,
	.properties = m270_green_properties,
};

static const struct property_entry m270_red_properties[] = {
	PROPERTY_ENTRY_STRING("label", "m270:red:guard"),
	PROPERTY_ENTRY_GPIO("gpios", &m270_gpio7, 3, GPIO_ACTIVE_HIGH),
	PROPERTY_ENTRY_STRING("default-state", "keep"),
	{ }
};

static const struct software_node m270_red = {
	.name = "guard-red",
	.parent = &m270_leds,
	.properties = m270_red_properties,
};

static const struct property_entry m270_keys_properties[] = {
	PROPERTY_ENTRY_U32("poll-interval", 20),
	{ }
};

static const struct software_node m270_keys = {
	.name = "m270-keys",
	.properties = m270_keys_properties,
};

static const struct property_entry m270_reset_properties[] = {
	PROPERTY_ENTRY_STRING("label", "Reset"),
	PROPERTY_ENTRY_U32("linux,code", KEY_RESTART),
	PROPERTY_ENTRY_GPIO("gpios", &m270_gpio7, 4, GPIO_ACTIVE_LOW),
	PROPERTY_ENTRY_U32("debounce-interval", 60),
	{ }
};

static const struct software_node m270_reset = {
	.name = "reset",
	.parent = &m270_keys,
	.properties = m270_reset_properties,
};

static const struct software_node *m270_nodes[] = {
	&m270_gpio7, &m270_leds, &m270_green, &m270_red,
	&m270_keys, &m270_reset, NULL
};

static void m270_put_gpio(void *data)
{
	gpio_device_put(data);
}

static void m270_unregister_nodes(void *data)
{
	software_node_unregister_node_group(data);
}

static void m270_detach_gpio_node(void *data)
{
	device_remove_software_node(data);
}

static void m270_unregister_device(void *data)
{
	platform_device_unregister(data);
}

static int m270_add_panel_device(struct device *parent, const char *name,
				 const struct software_node *node)
{
	const struct platform_device_info info = {
		.parent = parent,
		.name = name,
		.id = PLATFORM_DEVID_AUTO,
		.fwnode = software_node_fwnode(node),
	};
	struct platform_device *child;

	child = platform_device_register_full(&info);
	if (IS_ERR(child))
		return PTR_ERR(child);
	return devm_add_action_or_reset(parent, m270_unregister_device, child);
}

static int m270_panel_setup(struct device *dev, struct device *gpio)
{
	int err;

	err = software_node_register_node_group(m270_nodes);
	if (err)
		return err;
	err = devm_add_action_or_reset(dev, m270_unregister_nodes, m270_nodes);
	if (err)
		return err;
	/* Upstream GPIO lookup requires the referenced node on the provider. */
	err = device_add_software_node(gpio, &m270_gpio7);
	if (err)
		return err;
	err = devm_add_action_or_reset(dev, m270_detach_gpio_node, gpio);
	if (err)
		return err;
	err = m270_add_panel_device(dev, "leds-gpio", &m270_leds);
	if (err)
		return err;
	return m270_add_panel_device(dev, "gpio-keys-polled", &m270_keys);
}

static int m270_panel_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct gpio_device *provider;
	struct device *gpio, *supplier;
	void *group;
	int err;

	provider = gpio_device_find_by_label(m270_gpio7.name);
	if (!provider)
		return -EPROBE_DEFER;
	err = devm_add_action_or_reset(dev, m270_put_gpio, provider);
	if (err)
		return err;
	gpio = gpio_device_to_device(provider);
	supplier = gpio->parent;
	if (!supplier)
		return -ENODEV;
	if (!device_link_add(dev, supplier, DL_FLAG_AUTOPROBE_CONSUMER))
		return -ENOMEM;
	guard(device)(supplier);
	if (!device_is_bound(supplier))
		return -EPROBE_DEFER;

	group = devres_open_group(dev, NULL, GFP_KERNEL);
	if (!group)
		return -ENOMEM;
	err = m270_panel_setup(dev, gpio);
	if (err) {
		/* Roll back consumers and nodes before releasing the supplier lock. */
		devres_release_group(dev, group);
		return dev_err_probe(dev, err, "panel setup unavailable\n");
	}
	devres_remove_group(dev, group);
	return 0;
}

static const struct platform_device_id m270_panel_ids[] = {
	{ NCB_WG2511_PANEL_DEVICE_NAME, 0 },
	{ }
};
MODULE_DEVICE_TABLE(platform, m270_panel_ids);

static struct platform_driver ncb_wg2511_panel_driver = {
	.probe = m270_panel_probe,
	.id_table = m270_panel_ids,
	.driver.name = NCB_WG2511_PANEL_DEVICE_NAME,
};
module_platform_driver(ncb_wg2511_panel_driver);

MODULE_SOFTDEP("pre: gpio-f7188x");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("WatchGuard Firebox M270 front panel");
