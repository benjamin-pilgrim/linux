// SPDX-License-Identifier: GPL-2.0-only
/* Exercise M270 enumeration through native LED and input consumer drivers. */
#include <kunit/test.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/gpio/machine.h>
#include <linux/input.h>
#include <linux/leds.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/property.h>

#include "m270.h"

struct model_test_data {
	struct pci_dev *smbus;
	int probe_error;
};

struct model_fixture {
	struct gpio_chip gpio;
	struct platform_device *identity;
	struct platform_device *provider;
	struct pci_dev smbus;
	struct pci_bus bus;
	struct input_handler handler;
	struct input_handle handle;
	struct completion press;
	struct completion release;
	unsigned int restart_events;
	unsigned long values;
	unsigned long inputs;
	bool handler_registered;
	bool gpio_registered;
	unsigned int supplier_removals;
	unsigned long busy_on_supplier_remove;
};

static int model_gpio_get(struct gpio_chip *gc, unsigned int offset)
{
	struct model_fixture *f = gpiochip_get_data(gc);

	return !!(READ_ONCE(f->values) & BIT(offset));
}

static int model_gpio_store(struct gpio_chip *gc, unsigned int offset, int value)
{
	struct model_fixture *f = gpiochip_get_data(gc);

	if (value)
		f->values |= BIT(offset);
	else
		f->values &= ~BIT(offset);
	return 0;
}

static int model_gpio_set(struct gpio_chip *gc, unsigned int offset, int value)
{
	return model_gpio_store(gc, offset, value);
}

static int model_gpio_direction(struct gpio_chip *gc, unsigned int offset)
{
	struct model_fixture *f = gpiochip_get_data(gc);

	return !!(f->inputs & BIT(offset));
}

static int model_gpio_input(struct gpio_chip *gc, unsigned int offset)
{
	struct model_fixture *f = gpiochip_get_data(gc);

	f->inputs |= BIT(offset);
	return 0;
}

static int model_gpio_output(struct gpio_chip *gc, unsigned int offset, int value)
{
	struct model_fixture *f = gpiochip_get_data(gc);

	f->inputs &= ~BIT(offset);
	return model_gpio_store(gc, offset, value);
}

static void model_cleanup(void *data)
{
	struct model_fixture *f = data;

	if (f->handler_registered)
		input_unregister_handler(&f->handler);
	if (f->identity)
		platform_device_unregister(f->identity);
	if (f->gpio_registered) {
		gpiochip_remove(&f->gpio);
		f->gpio_registered = false;
	}
	platform_device_unregister(f->provider);
}

static int model_init(struct kunit *test)
{
	struct model_fixture *f;
	int err;

	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	if (!f)
		return -ENOMEM;
	test->priv = f;
	/* Initial red/released levels; unrelated GP71 must remain unchanged. */
	f->values = BIT(1) | BIT(3) | BIT(4);
	f->inputs = BIT(4);
	f->smbus.bus = &f->bus;
	f->provider = platform_device_register_data(NULL, "m270-test-gpio",
						    PLATFORM_DEVID_AUTO, &f, sizeof(f));
	if (IS_ERR(f->provider))
		return PTR_ERR(f->provider);
	f->gpio.parent = &f->provider->dev;
	f->gpio.label = "gpio-f7188x-7";
	f->gpio.owner = THIS_MODULE;
	f->gpio.base = -1;
	f->gpio.ngpio = 6;
	f->gpio.can_sleep = true;
	f->gpio.get = model_gpio_get;
	f->gpio.set = model_gpio_set;
	f->gpio.get_direction = model_gpio_direction;
	f->gpio.direction_input = model_gpio_input;
	f->gpio.direction_output = model_gpio_output;
	init_completion(&f->press);
	init_completion(&f->release);
	err = gpiochip_add_data(&f->gpio, f);
	if (err) {
		platform_device_unregister(f->provider);
		return err;
	}
	f->gpio_registered = true;
	return kunit_add_action_or_reset(test, model_cleanup, f);
}

static void model_register(struct kunit *test, int probe_error)
{
	struct model_fixture *f = test->priv;
	struct model_test_data data = {
		.smbus = &f->smbus,
		.probe_error = probe_error,
	};

	f->identity = platform_device_register_data(NULL, "m270-test-identity",
						    PLATFORM_DEVID_NONE, &data, sizeof(data));
	if (IS_ERR(f->identity)) {
		f->identity = NULL;
		KUNIT_FAIL(test, "identity consumer registration failed");
		return;
	}
	if (!probe_error)
		KUNIT_ASSERT_TRUE(test, device_is_bound(&f->identity->dev));
}

static int model_match_name(struct device *dev, const void *name)
{
	return dev->bus == &platform_bus_type &&
	       !strcmp(to_platform_device(dev)->name, name);
}

static struct device *model_child(struct model_fixture *f, const char *name)
{
	struct device *panel, *child;

	if (strcmp(name, "leds-gpio") && strcmp(name, "gpio-keys-polled"))
		return device_find_child(&f->identity->dev, name, model_match_name);
	panel = device_find_child(&f->identity->dev, NCB_WG2511_PANEL_DEVICE_NAME,
				  model_match_name);
	if (!panel)
		return NULL;
	child = device_find_child(panel, name, model_match_name);
	put_device(panel);
	return child;
}

/* Missing NIC suppliers must not prevent native panel consumers from binding. */
static void model_enumerates_independent_children(struct kunit *test)
{
	static const char * const names[] = {
		"leds-gpio", "gpio-keys-polled", NCB_WG2511_SWITCH_DEVICE_NAME,
	};
	struct model_fixture *f = test->priv;
	struct device *child;
	int i;

	model_register(test, 0);
	KUNIT_ASSERT_NOT_NULL(test, f->identity);
	for (i = 0; i < ARRAY_SIZE(names); i++) {
		child = model_child(f, names[i]);
		KUNIT_ASSERT_NOT_NULL(test, child);
		if (i < 2)
			KUNIT_EXPECT_TRUE(test, device_is_bound(child));
		put_device(child);
	}
}

/* A missing GPIO provider must not block enumeration of the switch child. */
static void model_waits_for_gpio_independently(struct kunit *test)
{
	struct model_fixture *f = test->priv;
	struct device *panel, *sw;

	gpiochip_remove(&f->gpio);
	f->gpio_registered = false;
	model_register(test, 0);
	KUNIT_ASSERT_NOT_NULL(test, f->identity);
	panel = model_child(f, NCB_WG2511_PANEL_DEVICE_NAME);
	sw = model_child(f, NCB_WG2511_SWITCH_DEVICE_NAME);
	KUNIT_EXPECT_NOT_NULL(test, panel);
	KUNIT_EXPECT_NOT_NULL(test, sw);
	if (panel)
		KUNIT_EXPECT_FALSE(test, device_is_bound(panel));
	put_device(sw);
	/* Retry the deferred panel after its supplier becomes available. */
	f->gpio.base = -1;
	if (gpiochip_add_data(&f->gpio, f)) {
		put_device(panel);
		KUNIT_FAIL(test, "GPIO provider registration failed");
		return;
	}
	f->gpio_registered = true;
	if (panel) {
		KUNIT_EXPECT_EQ(test, device_attach(panel), 1);
		put_device(panel);
	}
}

static int model_match_led(struct device *dev, const void *name)
{
	return !strcmp(dev_name(dev), name);
}

/* Standard LED brightness controls the documented pins and preserves others. */
static void model_leds_use_guard_pins(struct kunit *test)
{
	struct model_fixture *f = test->priv;
	struct device *leds, *green, *red;

	model_register(test, 0);
	KUNIT_ASSERT_NOT_NULL(test, f->identity);
	KUNIT_EXPECT_EQ(test, f->values, BIT(1) | BIT(3) | BIT(4));
	leds = model_child(f, "leds-gpio");
	KUNIT_ASSERT_NOT_NULL(test, leds);
	green = device_find_child(leds, "m270:green:guard", model_match_led);
	red = device_find_child(leds, "m270:red:guard", model_match_led);
	KUNIT_EXPECT_NOT_NULL(test, green);
	KUNIT_EXPECT_NOT_NULL(test, red);
	if (green && red) {
		KUNIT_EXPECT_EQ(test, led_set_brightness_sync(dev_get_drvdata(red), LED_OFF), 0);
		KUNIT_EXPECT_EQ(test, led_set_brightness_sync(dev_get_drvdata(green), LED_FULL), 0);
		KUNIT_EXPECT_EQ(test, f->values, BIT(1) | BIT(2) | BIT(4));
		KUNIT_EXPECT_EQ(test, led_set_brightness_sync(dev_get_drvdata(green), LED_OFF), 0);
		KUNIT_EXPECT_EQ(test, f->values, BIT(1) | BIT(4));
	}
	put_device(red);
	put_device(green);
	put_device(leds);
}

static void model_input_event(struct input_handle *handle, unsigned int type,
			      unsigned int code, int value)
{
	struct model_fixture *f = container_of(handle->handler, struct model_fixture, handler);

	if (type == EV_KEY && code == KEY_RESTART) {
		WRITE_ONCE(f->restart_events, READ_ONCE(f->restart_events) + 1);
		if (value == 1)
			complete(&f->press);
		else if (!value)
			complete(&f->release);
	}
}

static int model_input_connect(struct input_handler *handler, struct input_dev *dev,
			       const struct input_device_id *id)
{
	struct model_fixture *f = container_of(handler, struct model_fixture, handler);
	struct device *keys = dev->dev.parent;
	int err;

	if (!keys || !model_match_name(keys, "gpio-keys-polled") ||
	    !keys->parent || !model_match_name(keys->parent, NCB_WG2511_PANEL_DEVICE_NAME))
		return -ENODEV;
	f->handle.dev = dev;
	f->handle.handler = handler;
	f->handle.name = "m270-model-test";
	err = input_register_handle(&f->handle);
	if (err)
		return err;
	err = input_open_device(&f->handle);
	if (err)
		input_unregister_handle(&f->handle);
	return err;
}

static void model_input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
}

static const struct input_device_id model_input_ids[] = {
	{ .flags = INPUT_DEVICE_ID_MATCH_BUS, .bustype = BUS_HOST },
	{ }
};

/* A released Reset button must not generate an event while its device binds. */
static void model_released_reset_is_silent_at_registration(struct kunit *test)
{
	struct model_fixture *f = test->priv;
	unsigned int events;

	f->handler.event = model_input_event;
	f->handler.connect = model_input_connect;
	f->handler.disconnect = model_input_disconnect;
	f->handler.name = "m270-model-test";
	f->handler.id_table = model_input_ids;
	KUNIT_ASSERT_EQ(test, input_register_handler(&f->handler), 0);
	f->handler_registered = true;
	model_register(test, 0);
	KUNIT_ASSERT_NOT_NULL(test, f->identity);
	KUNIT_ASSERT_NOT_NULL(test, f->handle.dev);
	msleep(100);
	events = READ_ONCE(f->restart_events);
	KUNIT_EXPECT_EQ(test, events, 0U);
}

/* A held button must report a real press, without an invented release. */
static void model_held_reset_reports_initial_press(struct kunit *test)
{
	struct model_fixture *f = test->priv;
	unsigned int events;

	WRITE_ONCE(f->values, READ_ONCE(f->values) & ~BIT(4));
	f->handler.event = model_input_event;
	f->handler.connect = model_input_connect;
	f->handler.disconnect = model_input_disconnect;
	f->handler.name = "m270-model-test";
	f->handler.id_table = model_input_ids;
	KUNIT_ASSERT_EQ(test, input_register_handler(&f->handler), 0);
	f->handler_registered = true;
	model_register(test, 0);
	KUNIT_ASSERT_NOT_NULL(test, f->identity);
	KUNIT_ASSERT_NOT_NULL(test, f->handle.dev);
	msleep(100);
	events = READ_ONCE(f->restart_events);
	KUNIT_EXPECT_EQ(test, events, 1U);
	KUNIT_EXPECT_TRUE(test, completion_done(&f->press));
	KUNIT_EXPECT_FALSE(test, completion_done(&f->release));
}

/* GP74 low/high must deliver KEY_RESTART press/release through native polling. */
static void model_reset_reports_input_events(struct kunit *test)
{
	struct model_fixture *f = test->priv;

	model_register(test, 0);
	KUNIT_ASSERT_NOT_NULL(test, f->identity);
	f->handler.event = model_input_event;
	f->handler.connect = model_input_connect;
	f->handler.disconnect = model_input_disconnect;
	f->handler.name = "m270-model-test";
	f->handler.id_table = model_input_ids;
	KUNIT_ASSERT_EQ(test, input_register_handler(&f->handler), 0);
	f->handler_registered = true;
	KUNIT_ASSERT_NOT_NULL(test, f->handle.dev);
	WRITE_ONCE(f->values, READ_ONCE(f->values) & ~BIT(4));
	KUNIT_ASSERT_NE(test, wait_for_completion_timeout(&f->press, HZ), 0UL);
	WRITE_ONCE(f->values, READ_ONCE(f->values) | BIT(4));
	KUNIT_EXPECT_NE(test, wait_for_completion_timeout(&f->release, HZ), 0UL);
}

/* Identity-consumer unbind releases children; rebinding recreates working LEDs. */
static void model_rebind_releases_consumers(struct kunit *test)
{
	struct model_fixture *f = test->priv;
	struct device *child;
	int i;

	model_register(test, 0);
	KUNIT_ASSERT_NOT_NULL(test, f->identity);
	device_release_driver(&f->identity->dev);
	child = model_child(f, "leds-gpio");
	KUNIT_EXPECT_PTR_EQ(test, child, NULL);
	put_device(child);
	for (i = 2; i <= 4; i++) {
		struct gpio_desc *desc;

		desc = gpiochip_request_own_desc(&f->gpio, i, "released panel pin",
						 GPIO_ACTIVE_HIGH, GPIOD_ASIS);
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, desc);
		gpiochip_free_own_desc(desc);
	}
	KUNIT_ASSERT_EQ(test, device_attach(&f->identity->dev), 1);
	child = model_child(f, "leds-gpio");
	KUNIT_ASSERT_NOT_NULL(test, child);
	KUNIT_EXPECT_TRUE(test, device_is_bound(child));
	put_device(child);
}

/* Supplier unbind removes panel consumers while leaving identity setup alive. */
static void model_gpio_unbind_releases_panel(struct kunit *test)
{
	struct model_fixture *f = test->priv;
	struct device *panel, *sw;
	int i;

	model_register(test, 0);
	KUNIT_ASSERT_NOT_NULL(test, f->identity);
	panel = model_child(f, NCB_WG2511_PANEL_DEVICE_NAME);
	KUNIT_ASSERT_NOT_NULL(test, panel);
	device_release_driver(&f->provider->dev);
	KUNIT_EXPECT_EQ(test, f->supplier_removals, 1U);
	KUNIT_EXPECT_EQ(test, f->busy_on_supplier_remove, 0UL);
	KUNIT_EXPECT_FALSE(test, device_is_bound(panel));
	KUNIT_EXPECT_TRUE(test, device_is_bound(&f->identity->dev));
	sw = model_child(f, NCB_WG2511_SWITCH_DEVICE_NAME);
	KUNIT_EXPECT_NOT_NULL(test, sw);
	put_device(sw);
	for (i = 2; i <= 4; i++) {
		struct gpio_desc *desc;

		desc = gpiochip_request_own_desc(&f->gpio, i, "released panel pin",
						 GPIO_ACTIVE_HIGH, GPIOD_ASIS);
		KUNIT_EXPECT_NOT_ERR_OR_NULL(test, desc);
		if (!IS_ERR(desc))
			gpiochip_free_own_desc(desc);
	}
	KUNIT_EXPECT_EQ(test, device_attach(&f->provider->dev), 1);
	KUNIT_EXPECT_EQ(test, device_attach(panel), 1);
	put_device(panel);
}

/* Failed parent probing must unwind successful child registrations and pins. */
static void model_failed_probe_releases_children(struct kunit *test)
{
	struct model_fixture *f = test->priv;
	struct model_test_data *data;
	struct device *child;
	int i;

	model_register(test, -EINVAL);
	KUNIT_ASSERT_NOT_NULL(test, f->identity);
	KUNIT_EXPECT_FALSE(test, device_is_bound(&f->identity->dev));
	child = model_child(f, NCB_WG2511_PANEL_DEVICE_NAME);
	KUNIT_EXPECT_PTR_EQ(test, child, NULL);
	put_device(child);
	child = model_child(f, NCB_WG2511_SWITCH_DEVICE_NAME);
	KUNIT_EXPECT_PTR_EQ(test, child, NULL);
	put_device(child);
	for (i = 2; i <= 4; i++) {
		struct gpio_desc *desc;

		desc = gpiochip_request_own_desc(&f->gpio, i, "released panel pin",
						 GPIO_ACTIVE_HIGH, GPIOD_ASIS);
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, desc);
		gpiochip_free_own_desc(desc);
	}
	data = dev_get_platdata(&f->identity->dev);
	data->probe_error = 0;
	KUNIT_ASSERT_EQ(test, device_attach(&f->identity->dev), 1);
	child = model_child(f, "leds-gpio");
	KUNIT_ASSERT_NOT_NULL(test, child);
	KUNIT_EXPECT_TRUE(test, device_is_bound(child));
	put_device(child);
}

static void model_remove_existing_node(void *data)
{
	device_remove_software_node(data);
}

/* Failed panel setup must preserve foreign provider properties and allow retry. */
static void model_panel_preserves_existing_provider_node(struct kunit *test)
{
	static const struct property_entry properties[] = {
		PROPERTY_ENTRY_STRING("test-owner", "existing"),
		{ }
	};
	static const struct software_node existing = {
		.name = "m270-test-existing-gpio",
		.properties = properties,
	};
	struct model_fixture *f = test->priv;
	struct device *gpio = gpio_device_to_device(f->gpio.gpiodev);
	struct device *panel, *leds;
	const char *owner;

	KUNIT_ASSERT_EQ(test, device_add_software_node(gpio, &existing), 0);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, model_remove_existing_node, gpio), 0);
	model_register(test, 0);
	KUNIT_ASSERT_NOT_NULL(test, f->identity);
	panel = model_child(f, NCB_WG2511_PANEL_DEVICE_NAME);
	KUNIT_ASSERT_NOT_NULL(test, panel);
	KUNIT_EXPECT_FALSE(test, device_is_bound(panel));
	if (!device_property_read_string(gpio, "test-owner", &owner))
		KUNIT_EXPECT_STREQ(test, owner, "existing");
	else
		KUNIT_FAIL(test, "existing GPIO provider properties lost");
	leds = model_child(f, "leds-gpio");
	KUNIT_EXPECT_PTR_EQ(test, leds, NULL);
	put_device(leds);
	kunit_release_action(test, model_remove_existing_node, gpio);
	KUNIT_EXPECT_EQ(test, device_attach(panel), 1);
	put_device(panel);
	leds = model_child(f, "leds-gpio");
	KUNIT_ASSERT_NOT_NULL(test, leds);
	KUNIT_EXPECT_TRUE(test, device_is_bound(leds));
	put_device(leds);
}

/* The harness invokes the same interface as the production identity consumer. */
static int model_identity_probe(struct platform_device *pdev)
{
	const struct model_test_data *data = dev_get_platdata(&pdev->dev);
	int err;

	err = ncb_wg2511_m270_register_children(&pdev->dev, data->smbus);
	return err ? err : data->probe_error;
}

static struct platform_driver model_identity_driver = {
	.probe = model_identity_probe,
	.driver.name = "m270-test-identity",
};

static int model_provider_probe(struct platform_device *pdev)
{
	return 0;
}

/* Consumer GPIO ownership must be gone before the controller removes hardware. */
static void model_provider_remove(struct platform_device *pdev)
{
	struct model_fixture *f = *(struct model_fixture **)dev_get_platdata(&pdev->dev);
	int i;

	if (!f->gpio_registered)
		return;
	f->supplier_removals++;
	for (i = 2; i <= 4; i++) {
		struct gpio_desc *desc;

		desc = gpiochip_request_own_desc(&f->gpio, i, "supplier removal check",
						 GPIO_ACTIVE_HIGH, GPIOD_ASIS);
		if (IS_ERR(desc))
			f->busy_on_supplier_remove |= BIT(i);
		else
			gpiochip_free_own_desc(desc);
	}
}

static struct platform_driver model_provider_driver = {
	.probe = model_provider_probe,
	.remove = model_provider_remove,
	.driver.name = "m270-test-gpio",
};

static struct platform_driver * const model_test_drivers[] = {
	&model_provider_driver,
	&model_identity_driver,
};

static int model_suite_init(struct kunit_suite *suite)
{
	return platform_register_drivers(model_test_drivers, ARRAY_SIZE(model_test_drivers));
}

static void model_suite_exit(struct kunit_suite *suite)
{
	platform_unregister_drivers(model_test_drivers, ARRAY_SIZE(model_test_drivers));
}

static struct kunit_case model_cases[] = {
	KUNIT_CASE(model_enumerates_independent_children),
	KUNIT_CASE(model_waits_for_gpio_independently),
	KUNIT_CASE(model_leds_use_guard_pins),
	KUNIT_CASE(model_released_reset_is_silent_at_registration),
	KUNIT_CASE(model_held_reset_reports_initial_press),
	KUNIT_CASE(model_reset_reports_input_events),
	KUNIT_CASE(model_rebind_releases_consumers),
	KUNIT_CASE(model_gpio_unbind_releases_panel),
	KUNIT_CASE(model_failed_probe_releases_children),
	KUNIT_CASE(model_panel_preserves_existing_provider_node),
	{ }
};

static struct kunit_suite model_suite = {
	.name = "watchguard-m270",
	.suite_init = model_suite_init,
	.suite_exit = model_suite_exit,
	.init = model_init,
	.test_cases = model_cases,
};

kunit_test_suite(model_suite);

MODULE_LICENSE("GPL");
