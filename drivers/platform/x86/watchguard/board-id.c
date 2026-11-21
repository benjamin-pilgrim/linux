// SPDX-License-Identifier: GPL-2.0-only
#include <linux/kernel.h>
#include <linux/string.h>

#include "board-id.h"

/* Fireware's board predicates compare these BIOS-published prefixes. */
static const struct {
	const char *marker;
	unsigned int offset;
	enum watchguard_board board;
} watchguard_rom_markers[] = {
	{ "NCB-WG2511", 0x58b, WATCHGUARD_NCB_WG2511 },
	{ "NCB-4210WG", 0x599, WATCHGUARD_NCB_4210WG },
	{ "MB-UP2010W", 0x58b, WATCHGUARD_MB_UP2010W },
	{ "MB-WG-8771W", 0x58b, WATCHGUARD_MB_WG_8771W },
	{ "MB-WG-8877W", 0x58b, WATCHGUARD_MB_WG_8877W },
	{ "MB-WG8758W", 0x58b, WATCHGUARD_MB_WG8758W },
	{ "MB-WG8865W", 0x58b, WATCHGUARD_MB_WG8865W },
};

int watchguard_identify_board(const u8 *rom, size_t len, const char *dmi_board_name,
			      enum watchguard_board *board)
{
	unsigned int i;

	if (dmi_board_name) {
		if (!strcmp(dmi_board_name, "T55")) {
			*board = WATCHGUARD_T55;
			return 0;
		}
		if (!strcmp(dmi_board_name, "T70")) {
			*board = WATCHGUARD_T70;
			return 0;
		}
	}
	if (!rom)
		return -ENODEV;
	for (i = 0; i < ARRAY_SIZE(watchguard_rom_markers); i++) {
		size_t size = strlen(watchguard_rom_markers[i].marker);
		unsigned int offset = watchguard_rom_markers[i].offset;

		if (offset <= len && size <= len - offset &&
		    !memcmp(rom + offset, watchguard_rom_markers[i].marker, size)) {
			*board = watchguard_rom_markers[i].board;
			return 0;
		}
	}
	return -ENODEV;
}
