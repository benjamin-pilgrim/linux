/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _WATCHGUARD_BOARD_ID_H
#define _WATCHGUARD_BOARD_ID_H

#include <linux/types.h>

#define WATCHGUARD_BOARD_DEVICE_NAME "watchguard-board"
#define WATCHGUARD_ROM_BASE 0xff000U
#define WATCHGUARD_ROM_SIZE 0x1000

enum watchguard_board {
	WATCHGUARD_NCB_WG2511,
	WATCHGUARD_NCB_4210WG,
	WATCHGUARD_MB_UP2010W,
	WATCHGUARD_MB_WG_8771W,
	WATCHGUARD_MB_WG_8877W,
	WATCHGUARD_MB_WG8758W,
	WATCHGUARD_MB_WG8865W,
	WATCHGUARD_T55,
	WATCHGUARD_T70,
};

/*
 * Match firmware-published data without accessing management buses.
 * Return 0 or -ENODEV, leaving board unchanged on failure.
 */
int watchguard_identify_board(const u8 *rom, size_t len, const char *dmi_board_name,
			      enum watchguard_board *board);

#endif
