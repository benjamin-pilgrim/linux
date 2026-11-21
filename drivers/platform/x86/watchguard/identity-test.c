// SPDX-License-Identifier: GPL-2.0-only
/* Exercise manufacturing identity reads through a synthetic SMBus adapter. */
#include <kunit/test.h>
#include <linux/device.h>
#include <linux/i2c.h>

#include "identity.h"

struct model_fixture {
	struct i2c_adapter adapter;
	struct device *parent;
	u8 serial[13];
	u16 address;
	int failure_register;
	int failure;
	unsigned int reads;
	bool supports_reads;
};

static u32 model_functionality(struct i2c_adapter *adapter)
{
	struct model_fixture *fixture = i2c_get_adapdata(adapter);

	return fixture->supports_reads ? I2C_FUNC_SMBUS_READ_BYTE_DATA : 0;
}

static s32 model_transfer(struct i2c_adapter *adapter, u16 address,
			  unsigned short flags, char direction, u8 command,
			  int size, union i2c_smbus_data *data)
{
	struct model_fixture *fixture = i2c_get_adapdata(adapter);

	if (address != fixture->address || direction != I2C_SMBUS_READ ||
	    size != I2C_SMBUS_BYTE_DATA || command < 0x25 || command > 0x31)
		return -EOPNOTSUPP;
	fixture->reads++;
	if (command == fixture->failure_register)
		return fixture->failure;
	data->byte = fixture->serial[command - 0x25];
	return 0;
}

static const struct i2c_algorithm model_algorithm = {
	.smbus_xfer = model_transfer,
	.functionality = model_functionality,
};

static void release_adapter(void *data)
{
	struct model_fixture *fixture = data;

	i2c_del_adapter(&fixture->adapter);
	root_device_unregister(fixture->parent);
}

static int model_init(struct kunit *test)
{
	struct model_fixture *fixture;
	int err;

	fixture = kunit_kzalloc(test, sizeof(*fixture), GFP_KERNEL);
	if (!fixture)
		return -ENOMEM;
	test->priv = fixture;
	/* CRC from the inspected vendor routine, also matching CRC16/ARC. */
	memcpy(fixture->serial, "0014000017DE9", sizeof(fixture->serial));
	fixture->address = 0x56;
	fixture->failure_register = -1;
	fixture->supports_reads = true;
	fixture->parent = root_device_register("ncb-model-test");
	if (IS_ERR(fixture->parent))
		return PTR_ERR(fixture->parent);
	fixture->adapter.dev.parent = fixture->parent;
	fixture->adapter.algo = &model_algorithm;
	strscpy(fixture->adapter.name, "WatchGuard identity test");
	i2c_set_adapdata(&fixture->adapter, fixture);
	err = i2c_add_adapter(&fixture->adapter);
	if (err) {
		root_device_unregister(fixture->parent);
		return err;
	}
	return kunit_add_action_or_reset(test, release_adapter, fixture);
}

/* Every completed identification must release its temporary EEPROM client. */
static void expect_address_available(struct kunit *test)
{
	struct model_fixture *fixture = test->priv;
	struct i2c_client *client;

	client = i2c_new_dummy_device(&fixture->adapter, fixture->address);
	KUNIT_EXPECT_NOT_ERR_OR_NULL(test, client);
	if (!IS_ERR(client))
		i2c_unregister_device(client);
}

/* A valid serial identifies the model and is returned with a terminating NUL. */
static void model_selects_m270(struct kunit *test)
{
	struct model_fixture *fixture = test->priv;
	struct watchguard_identity identity;

	KUNIT_ASSERT_EQ(test, watchguard_read_identity(&fixture->adapter,
						       fixture->address, &identity), 0);
	KUNIT_EXPECT_EQ(test, identity.model_code, WATCHGUARD_MODEL_M270);
	KUNIT_EXPECT_STREQ(test, identity.serial, "0014000017DE9");
	expect_address_available(test);
}

/* The same record format works at a board-selected EEPROM address. */
static void model_reads_selected_address(struct kunit *test)
{
	struct model_fixture *fixture = test->priv;
	struct watchguard_identity identity = {};
	struct i2c_client *other;

	fixture->address = 0x57;
	other = i2c_new_dummy_device(&fixture->adapter, 0x56);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, other);
	KUNIT_EXPECT_EQ(test, watchguard_read_identity(&fixture->adapter, 0x57,
						       &identity), 0);
	KUNIT_EXPECT_STREQ(test, identity.serial, "0014000017DE9");
	expect_address_available(test);
	i2c_unregister_device(other);
}

/* An invalid I2C address must fail before transfer or identity publication. */
static void model_rejects_invalid_address(struct kunit *test)
{
	struct model_fixture *fixture = test->priv;
	struct watchguard_identity identity = {
		.serial = "unchanged",
		.model_code = 0xa5,
	};

	KUNIT_EXPECT_EQ(test, watchguard_read_identity(&fixture->adapter, 0x80,
						       &identity), -EINVAL);
	KUNIT_EXPECT_EQ(test, fixture->reads, 0);
	KUNIT_EXPECT_STREQ(test, identity.serial, "unchanged");
	KUNIT_EXPECT_EQ(test, identity.model_code, 0xa5);
}

/* Unsupported model codes retain their firmware value without selecting M270. */
static void model_preserves_unsupported(struct kunit *test)
{
	struct model_fixture *fixture = test->priv;
	struct watchguard_identity identity;

	memcpy(fixture->serial, "001500001ACE8", sizeof(fixture->serial));
	KUNIT_ASSERT_EQ(test, watchguard_read_identity(&fixture->adapter,
						       fixture->address, &identity), 0);
	KUNIT_EXPECT_EQ(test, identity.model_code, 0x15);
	KUNIT_EXPECT_STREQ(test, identity.serial, "001500001ACE8");
	expect_address_available(test);
}

/* Both hex letter cases decode to the same raw model byte. */
static void model_decodes_hex_letters(struct kunit *test)
{
	static const char * const serials[] = { "00FA000011FE5", "00fa00001BDC3" };
	struct model_fixture *fixture = test->priv;
	struct watchguard_identity identity;
	int i;

	for (i = 0; i < ARRAY_SIZE(serials); i++) {
		memcpy(fixture->serial, serials[i], sizeof(fixture->serial));
		KUNIT_ASSERT_EQ(test, watchguard_read_identity(&fixture->adapter,
							       fixture->address, &identity), 0);
		KUNIT_EXPECT_EQ(test, identity.model_code, 0xfa);
		expect_address_available(test);
	}
}

/* A CRC-valid record must still have a hexadecimal model field. */
static void model_rejects_nonhex_model(struct kunit *test)
{
	struct model_fixture *fixture = test->priv;
	struct watchguard_identity identity;

	memcpy(fixture->serial, "00G400001BAFE", sizeof(fixture->serial));
	KUNIT_EXPECT_EQ(test, watchguard_read_identity(&fixture->adapter,
						       fixture->address, &identity),
			-EBADMSG);
	expect_address_available(test);
}

/* Corruption anywhere in the field must fail, including outside model bytes. */
static void model_rejects_corruption(struct kunit *test)
{
	struct model_fixture *fixture = test->priv;
	struct watchguard_identity identity;
	int i;

	for (i = 0; i < sizeof(fixture->serial); i++) {
		fixture->serial[i] ^= 1;
		KUNIT_EXPECT_EQ(test, watchguard_read_identity(&fixture->adapter,
							       fixture->address, &identity),
				-EBADMSG);
		fixture->serial[i] ^= 1;
		expect_address_available(test);
	}
}

/* The ASCII checksum must contain four hex digits. */
static void model_rejects_nonhex_checksum(struct kunit *test)
{
	struct model_fixture *fixture = test->priv;
	struct watchguard_identity identity;

	fixture->serial[9] = 'G';
	KUNIT_EXPECT_EQ(test, watchguard_read_identity(&fixture->adapter,
						       fixture->address, &identity),
			-EBADMSG);
	expect_address_available(test);
}

/* A CRC-valid record with an embedded NUL is not a 13-character serial. */
static void model_rejects_embedded_nul(struct kunit *test)
{
	struct model_fixture *fixture = test->priv;
	struct watchguard_identity identity;

	memcpy(fixture->serial, "0014\000000179A9", sizeof(fixture->serial));
	KUNIT_EXPECT_EQ(test, watchguard_read_identity(&fixture->adapter,
						       fixture->address, &identity),
			-EBADMSG);
	expect_address_available(test);
}

/* Hex checksum case does not alter its value. */
static void model_accepts_lowercase_checksum(struct kunit *test)
{
	struct model_fixture *fixture = test->priv;
	struct watchguard_identity identity;

	memcpy(fixture->serial, "0014000017de9", sizeof(fixture->serial));
	KUNIT_ASSERT_EQ(test, watchguard_read_identity(&fixture->adapter,
						       fixture->address, &identity), 0);
	KUNIT_EXPECT_EQ(test, identity.model_code, WATCHGUARD_MODEL_M270);
	expect_address_available(test);
}

/* Failed validation must not publish a partial identity. */
static void model_preserves_output_on_error(struct kunit *test)
{
	struct model_fixture *fixture = test->priv;
	struct watchguard_identity identity = {
		.serial = "unchanged",
		.model_code = 0xa5,
	};

	fixture->serial[12] = '0';
	KUNIT_EXPECT_EQ(test, watchguard_read_identity(&fixture->adapter,
						       fixture->address, &identity),
			-EBADMSG);
	KUNIT_EXPECT_STREQ(test, identity.serial, "unchanged");
	KUNIT_EXPECT_EQ(test, identity.model_code, 0xa5);
	expect_address_available(test);
}

/* An initial read failure propagates without attempting the next byte. */
static void model_stops_on_first_error(struct kunit *test)
{
	struct model_fixture *fixture = test->priv;
	struct watchguard_identity identity;

	fixture->failure_register = 0x25;
	fixture->failure = -ENXIO;
	KUNIT_EXPECT_EQ(test, watchguard_read_identity(&fixture->adapter,
						       fixture->address, &identity),
			-ENXIO);
	KUNIT_EXPECT_EQ(test, fixture->reads, 1);
	expect_address_available(test);
}

/* Later failures must also propagate and release the reserved address. */
static void model_propagates_second_error(struct kunit *test)
{
	struct model_fixture *fixture = test->priv;
	struct watchguard_identity identity;

	fixture->failure_register = 0x31;
	fixture->failure = -ETIMEDOUT;
	KUNIT_EXPECT_EQ(test, watchguard_read_identity(&fixture->adapter,
						       fixture->address, &identity),
			-ETIMEDOUT);
	expect_address_available(test);
}

/* Ownership by another client forbids even a model read. */
static void model_respects_address_owner(struct kunit *test)
{
	struct model_fixture *fixture = test->priv;
	struct i2c_client *owner;
	struct watchguard_identity identity;

	fixture->address = 0x57;
	owner = i2c_new_dummy_device(&fixture->adapter, fixture->address);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, owner);
	KUNIT_EXPECT_EQ(test, watchguard_read_identity(&fixture->adapter,
						       fixture->address, &identity),
			-EBUSY);
	KUNIT_EXPECT_EQ(test, fixture->reads, 0);
	i2c_unregister_device(owner);
}

/* A controller without byte-data reads cannot implement this protocol. */
static void model_requires_byte_reads(struct kunit *test)
{
	struct model_fixture *fixture = test->priv;
	struct watchguard_identity identity;

	fixture->supports_reads = false;
	KUNIT_EXPECT_EQ(test, watchguard_read_identity(&fixture->adapter,
						       fixture->address, &identity),
			-EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, fixture->reads, 0);
	expect_address_available(test);
}

static struct kunit_case model_cases[] = {
	KUNIT_CASE(model_selects_m270),
	KUNIT_CASE(model_reads_selected_address),
	KUNIT_CASE(model_rejects_invalid_address),
	KUNIT_CASE(model_preserves_unsupported),
	KUNIT_CASE(model_decodes_hex_letters),
	KUNIT_CASE(model_rejects_nonhex_model),
	KUNIT_CASE(model_rejects_corruption),
	KUNIT_CASE(model_rejects_nonhex_checksum),
	KUNIT_CASE(model_rejects_embedded_nul),
	KUNIT_CASE(model_accepts_lowercase_checksum),
	KUNIT_CASE(model_preserves_output_on_error),
	KUNIT_CASE(model_stops_on_first_error),
	KUNIT_CASE(model_propagates_second_error),
	KUNIT_CASE(model_respects_address_owner),
	KUNIT_CASE(model_requires_byte_reads),
	{}
};

static struct kunit_suite model_suite = {
	.name = "watchguard-identity",
	.init = model_init,
	.test_cases = model_cases,
};

kunit_test_suite(model_suite);

MODULE_LICENSE("GPL");
