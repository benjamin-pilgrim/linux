// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>

#include "board-id.h"

static void board_matches_rom_markers(struct kunit *test)
{
	static const struct {
		const char *marker;
		unsigned int offset;
		enum watchguard_board board;
	} examples[] = {
		{ "NCB-WG2511", 0x58b, WATCHGUARD_NCB_WG2511 },
		{ "NCB-4210WG", 0x599, WATCHGUARD_NCB_4210WG },
		{ "MB-UP2010W", 0x58b, WATCHGUARD_MB_UP2010W },
		{ "MB-WG-8771W", 0x58b, WATCHGUARD_MB_WG_8771W },
		{ "MB-WG-8877W", 0x58b, WATCHGUARD_MB_WG_8877W },
		{ "MB-WG8758W", 0x58b, WATCHGUARD_MB_WG8758W },
		{ "MB-WG8865W", 0x58b, WATCHGUARD_MB_WG8865W },
	};
	enum watchguard_board board;
	u8 *rom = kunit_kzalloc(test, WATCHGUARD_ROM_SIZE, GFP_KERNEL);
	unsigned int i;

	KUNIT_ASSERT_NOT_NULL(test, rom);
	for (i = 0; i < ARRAY_SIZE(examples); i++) {
		memset(rom, 0, WATCHGUARD_ROM_SIZE);
		memcpy(rom + examples[i].offset, examples[i].marker,
		       strlen(examples[i].marker));
		memcpy(rom + examples[i].offset + strlen(examples[i].marker), " Ver.TEST", 9);
		KUNIT_ASSERT_EQ(test, watchguard_identify_board(rom, WATCHGUARD_ROM_SIZE,
								"SKYBAY", &board), 0);
		KUNIT_EXPECT_EQ(test, board, examples[i].board);
	}
}

static void board_requires_correct_address(struct kunit *test)
{
	enum watchguard_board board;
	u8 *rom = kunit_kzalloc(test, WATCHGUARD_ROM_SIZE, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, rom);
	memcpy(rom + 0x58b, "NCB-4210WG", 10);
	KUNIT_EXPECT_EQ(test, watchguard_identify_board(rom, WATCHGUARD_ROM_SIZE, NULL,
							&board), -ENODEV);
	memset(rom, 0, WATCHGUARD_ROM_SIZE);
	memcpy(rom + 0x599, "NCB-WG2511", 10);
	KUNIT_EXPECT_EQ(test, watchguard_identify_board(rom, WATCHGUARD_ROM_SIZE, NULL,
							&board), -ENODEV);
}

static void board_rejects_missing_or_truncated_marker(struct kunit *test)
{
	enum watchguard_board board;
	u8 *rom = kunit_kzalloc(test, WATCHGUARD_ROM_SIZE, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, rom);
	KUNIT_EXPECT_EQ(test, watchguard_identify_board(NULL, 0, NULL, &board), -ENODEV);
	KUNIT_EXPECT_EQ(test, watchguard_identify_board(rom, WATCHGUARD_ROM_SIZE, NULL,
							&board), -ENODEV);
	memcpy(rom + 0x58b, "NCB-WG2511", 10);
	KUNIT_EXPECT_EQ(test, watchguard_identify_board(rom, 0x58b + 9, NULL, &board),
			-ENODEV);
	KUNIT_ASSERT_EQ(test, watchguard_identify_board(rom, 0x58b + 10, NULL, &board), 0);
	KUNIT_EXPECT_EQ(test, board, WATCHGUARD_NCB_WG2511);
	rom[0x58b + 9] = '2';
	KUNIT_EXPECT_EQ(test, watchguard_identify_board(rom, WATCHGUARD_ROM_SIZE, NULL,
							&board), -ENODEV);
}

static void board_matches_exact_dmi_names(struct kunit *test)
{
	enum watchguard_board board;

	KUNIT_ASSERT_EQ(test, watchguard_identify_board(NULL, 0, "T55", &board), 0);
	KUNIT_EXPECT_EQ(test, board, WATCHGUARD_T55);
	KUNIT_ASSERT_EQ(test, watchguard_identify_board(NULL, 0, "T70", &board), 0);
	KUNIT_EXPECT_EQ(test, board, WATCHGUARD_T70);
	KUNIT_EXPECT_EQ(test, watchguard_identify_board(NULL, 0, "T550", &board), -ENODEV);
	KUNIT_EXPECT_EQ(test, watchguard_identify_board(NULL, 0, "T700", &board), -ENODEV);
	KUNIT_EXPECT_EQ(test, watchguard_identify_board(NULL, 0, "NCB-WG2511", &board),
			-ENODEV);
}

/* Failed detection must leave the caller's board unchanged. */
static void board_preserves_output_on_error(struct kunit *test)
{
	enum watchguard_board board = WATCHGUARD_T55;

	KUNIT_EXPECT_EQ(test, watchguard_identify_board(NULL, 0, "unrecognised", &board),
			-ENODEV);
	KUNIT_EXPECT_EQ(test, board, WATCHGUARD_T55);
}

static struct kunit_case board_cases[] = {
	KUNIT_CASE(board_matches_rom_markers),
	KUNIT_CASE(board_requires_correct_address),
	KUNIT_CASE(board_rejects_missing_or_truncated_marker),
	KUNIT_CASE(board_matches_exact_dmi_names),
	KUNIT_CASE(board_preserves_output_on_error),
	{ }
};

static struct kunit_suite board_suite = {
	.name = "watchguard-board-id",
	.test_cases = board_cases,
};

kunit_test_suite(board_suite);

MODULE_LICENSE("GPL");
