// SPDX-License-Identifier: GPL-2.0-only
#include <linux/crc16.h>
#include <linux/err.h>
#include <linux/hex.h>
#include <linux/i2c.h>
#include <linux/string.h>

#include "identity.h"

#define WATCHGUARD_SERIAL_OFFSET 0x25
#define WATCHGUARD_SERIAL_DATA_LEN 9
#define WATCHGUARD_MODEL_OFFSET 2

static int watchguard_validate_serial(const char *serial)
{
	u8 stored[2];
	u16 expected, actual;

	if (memchr(serial, '\0', WATCHGUARD_SERIAL_DATA_LEN) ||
	    hex2bin(stored, serial + WATCHGUARD_SERIAL_DATA_LEN, sizeof(stored)))
		return -EBADMSG;
	expected = (stored[0] << 8) | stored[1];
	actual = crc16(0, (const u8 *)serial, WATCHGUARD_SERIAL_DATA_LEN);
	return actual == expected ? 0 : -EBADMSG;
}

int watchguard_read_identity(struct i2c_adapter *adapter, u16 address,
			     struct watchguard_identity *identity)
{
	struct watchguard_identity result = {};
	struct i2c_client *eeprom;
	int i, err;

	if (!i2c_check_functionality(adapter, I2C_FUNC_SMBUS_READ_BYTE_DATA))
		return -EOPNOTSUPP;
	eeprom = i2c_new_dummy_device(adapter, address);
	if (IS_ERR(eeprom))
		return PTR_ERR(eeprom);
	for (i = 0; i < WATCHGUARD_SERIAL_LEN; i++) {
		err = i2c_smbus_read_byte_data(eeprom, WATCHGUARD_SERIAL_OFFSET + i);
		if (err < 0)
			goto release;
		result.serial[i] = err;
	}
	err = watchguard_validate_serial(result.serial);
	if (err)
		goto release;
	/* The model field contains two ASCII hexadecimal digits. */
	if (hex2bin(&result.model_code, result.serial + WATCHGUARD_MODEL_OFFSET, 1)) {
		err = -EBADMSG;
		goto release;
	}
	*identity = result;
	err = 0;
release:
	i2c_unregister_device(eeprom);
	return err;
}
