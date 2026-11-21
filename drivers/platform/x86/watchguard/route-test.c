// SPDX-License-Identifier: GPL-2.0-only
/* Exercise the real PCA954x driver over a synthetic parent SMBus. */
#include <kunit/test.h>
#include <linux/i2c.h>

#include "route.h"

struct route_fixture {
	struct i2c_adapter root;
	struct device *parent;
	struct watchguard_route route;
	u8 selected;
	bool direct;
	int read_error;
	unsigned int transactions;
	unsigned int serial_reads;
	unsigned int wrong_selections;
};

static u32 route_functionality(struct i2c_adapter *adapter)
{
	return I2C_FUNC_SMBUS_BYTE | I2C_FUNC_SMBUS_BYTE_DATA;
}

static s32 route_transfer(struct i2c_adapter *adapter, u16 address,
			  unsigned short flags, char direction, u8 command,
			  int size, union i2c_smbus_data *data)
{
	static const char serial[] = "0014000017DE9";
	struct route_fixture *fixture = i2c_get_adapdata(adapter);

	fixture->transactions++;
	if (address == 0x73 && size == I2C_SMBUS_BYTE) {
		if (direction == I2C_SMBUS_WRITE)
			fixture->selected = command & 0x0f;
		else
			data->byte = fixture->selected;
		return 0;
	}
	if (address != 0x56 || direction != I2C_SMBUS_READ ||
	    size != I2C_SMBUS_BYTE_DATA || command < 0x25 || command > 0x31)
		return -EOPNOTSUPP;
	fixture->serial_reads++;
	if (!fixture->direct && fixture->selected != 0x08) {
		fixture->wrong_selections++;
		return -ENXIO;
	}
	if (fixture->read_error)
		return fixture->read_error;
	data->byte = serial[command - 0x25];
	return 0;
}

static const struct i2c_algorithm route_algorithm = {
	.smbus_xfer = route_transfer,
	.functionality = route_functionality,
};

static void release_route_fixture(void *data)
{
	struct route_fixture *fixture = data;

	watchguard_route_destroy(&fixture->route);
	i2c_del_adapter(&fixture->root);
	root_device_unregister(fixture->parent);
}

static int route_init(struct kunit *test)
{
	struct route_fixture *fixture;
	int err;

	fixture = kunit_kzalloc(test, sizeof(*fixture), GFP_KERNEL);
	if (!fixture)
		return -ENOMEM;
	test->priv = fixture;
	fixture->parent = root_device_register("watchguard-route-test");
	if (IS_ERR(fixture->parent))
		return PTR_ERR(fixture->parent);
	fixture->root.dev.parent = fixture->parent;
	fixture->root.algo = &route_algorithm;
	strscpy(fixture->root.name, "WatchGuard route test");
	i2c_set_adapdata(&fixture->root, fixture);
	err = i2c_add_adapter(&fixture->root);
	if (err) {
		root_device_unregister(fixture->parent);
		return err;
	}
	return kunit_add_action_or_reset(test, release_route_fixture, fixture);
}

/* Recognition of another board must cause zero SMBus transactions. */
static void route_disables_unverified_boards(struct kunit *test)
{
	static const enum watchguard_board boards[] = {
		(enum watchguard_board)-1,
		WATCHGUARD_MB_WG_8771W, WATCHGUARD_MB_WG_8877W,
		WATCHGUARD_MB_WG8758W, WATCHGUARD_MB_WG8865W,
	};
	struct route_fixture *fixture = test->priv;
	struct watchguard_identity identity;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(boards); i++) {
		KUNIT_EXPECT_EQ(test, watchguard_route_create(boards[i], &fixture->root,
							      &fixture->route), -ENODEV);
		KUNIT_EXPECT_EQ(test, watchguard_route_read(&fixture->route, &identity),
				-ENODEV);
	}
	KUNIT_EXPECT_EQ(test, fixture->transactions, 0);
}

static void route_reads_direct_boards(struct kunit *test)
{
	static const enum watchguard_board boards[] = {
		WATCHGUARD_NCB_WG2511, WATCHGUARD_MB_UP2010W,
		WATCHGUARD_T55, WATCHGUARD_T70,
	};
	struct route_fixture *fixture = test->priv;
	struct watchguard_identity identity;
	unsigned int i;

	fixture->direct = true;
	for (i = 0; i < ARRAY_SIZE(boards); i++) {
		fixture->transactions = 0;
		KUNIT_ASSERT_EQ(test, watchguard_route_create(boards[i], &fixture->root,
							      &fixture->route), 0);
		KUNIT_ASSERT_EQ(test, watchguard_route_read(&fixture->route, &identity), 0);
		KUNIT_EXPECT_STREQ(test, identity.serial, "0014000017DE9");
		KUNIT_EXPECT_EQ(test, fixture->transactions, 13);
		KUNIT_EXPECT_EQ(test, fixture->selected, 0);
		watchguard_route_destroy(&fixture->route);
	}
}

/* The normal mux driver must select only channel 3 and disconnect afterward. */
static void route_reads_m370_channel_three(struct kunit *test)
{
	struct route_fixture *fixture = test->priv;
	struct watchguard_identity identity;

	KUNIT_ASSERT_EQ(test, watchguard_route_create(WATCHGUARD_NCB_4210WG,
						      &fixture->root, &fixture->route), 0);
	KUNIT_ASSERT_EQ(test, watchguard_route_read(&fixture->route, &identity), 0);
	KUNIT_EXPECT_STREQ(test, identity.serial, "0014000017DE9");
	KUNIT_EXPECT_EQ(test, fixture->serial_reads, 13);
	KUNIT_EXPECT_EQ(test, fixture->wrong_selections, 0);
	KUNIT_EXPECT_EQ(test, fixture->selected, 0);
}

static void route_disconnects_on_read_failure(struct kunit *test)
{
	struct route_fixture *fixture = test->priv;
	struct watchguard_identity identity = { .model_code = 0xfe };

	KUNIT_ASSERT_EQ(test, watchguard_route_create(WATCHGUARD_NCB_4210WG,
						      &fixture->root, &fixture->route), 0);
	fixture->read_error = -ETIMEDOUT;
	KUNIT_EXPECT_EQ(test, watchguard_route_read(&fixture->route, &identity), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, fixture->selected, 0);
	KUNIT_EXPECT_EQ(test, identity.model_code, 0xfe);
}

static void route_waits_for_mux_rebind(struct kunit *test)
{
	struct route_fixture *fixture = test->priv;
	struct watchguard_identity identity;
	unsigned int transactions;

	KUNIT_ASSERT_EQ(test, watchguard_route_create(WATCHGUARD_NCB_4210WG,
						      &fixture->root, &fixture->route), 0);
	device_release_driver(&fixture->route.mux->dev);
	transactions = fixture->transactions;
	KUNIT_EXPECT_EQ(test, watchguard_route_read(&fixture->route, &identity), -EPROBE_DEFER);
	KUNIT_EXPECT_EQ(test, fixture->transactions, transactions);
	KUNIT_ASSERT_EQ(test, device_attach(&fixture->route.mux->dev), 1);
	KUNIT_EXPECT_EQ(test, watchguard_route_read(&fixture->route, &identity), 0);
}

static void route_respects_mux_address_owner(struct kunit *test)
{
	struct route_fixture *fixture = test->priv;
	struct i2c_client *owner = i2c_new_dummy_device(&fixture->root, 0x73);

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, owner);
	KUNIT_EXPECT_EQ(test, watchguard_route_create(WATCHGUARD_NCB_4210WG,
						      &fixture->root, &fixture->route), -EBUSY);
	KUNIT_EXPECT_EQ(test, fixture->transactions, 0);
	i2c_unregister_device(owner);
}

static void route_releases_mux_for_retry(struct kunit *test)
{
	struct route_fixture *fixture = test->priv;
	struct watchguard_identity identity;

	KUNIT_ASSERT_EQ(test, watchguard_route_create(WATCHGUARD_NCB_4210WG,
						      &fixture->root, &fixture->route), 0);
	watchguard_route_destroy(&fixture->route);
	KUNIT_ASSERT_EQ(test, watchguard_route_create(WATCHGUARD_NCB_4210WG,
						      &fixture->root, &fixture->route), 0);
	KUNIT_EXPECT_EQ(test, watchguard_route_read(&fixture->route, &identity), 0);
}

static struct kunit_case route_cases[] = {
	KUNIT_CASE(route_disables_unverified_boards),
	KUNIT_CASE(route_reads_direct_boards),
	KUNIT_CASE(route_reads_m370_channel_three),
	KUNIT_CASE(route_disconnects_on_read_failure),
	KUNIT_CASE(route_waits_for_mux_rebind),
	KUNIT_CASE(route_respects_mux_address_owner),
	KUNIT_CASE(route_releases_mux_for_retry),
	{ }
};

static struct kunit_suite route_suite = {
	.name = "watchguard-route",
	.init = route_init,
	.test_cases = route_cases,
};

kunit_test_suite(route_suite);

MODULE_LICENSE("GPL");
