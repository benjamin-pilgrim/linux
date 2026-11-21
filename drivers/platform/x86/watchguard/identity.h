/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _WATCHGUARD_IDENTITY_H
#define _WATCHGUARD_IDENTITY_H

#include <linux/types.h>

#define WATCHGUARD_SERIAL_LEN 13
#define WATCHGUARD_MODEL_M270 0x14
#define WATCHGUARD_MODEL_M370 0x13

struct i2c_adapter;

struct watchguard_identity {
	char serial[WATCHGUARD_SERIAL_LEN + 1];
	u8 model_code;
};

/*
 * Read the board-selected EEPROM address and return a CRC-validated identity,
 * including unsupported model codes.
 * On failure the caller's identity is unchanged. Release the temporary
 * EEPROM client on every path after reserving its address.
 */
int watchguard_read_identity(struct i2c_adapter *adapter, u16 address,
			     struct watchguard_identity *identity);

#endif
