// SPDX-License-Identifier: GPL-2.0-only
#include <linux/dmi.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include "board-id.h"

static struct platform_device *watchguard_device;

static int __init watchguard_platform_init(void)
{
	const char *board_name = dmi_get_system_info(DMI_BOARD_NAME);
	enum watchguard_board board;
	void __iomem *mapping;
	u8 *rom;
	int err;

	err = watchguard_identify_board(NULL, 0, board_name, &board);
	if (err == -ENODEV) {
		rom = kmalloc(WATCHGUARD_ROM_SIZE, GFP_KERNEL);
		if (!rom)
			return -ENOMEM;
		mapping = ioremap(WATCHGUARD_ROM_BASE, WATCHGUARD_ROM_SIZE);
		if (!mapping) {
			kfree(rom);
			return -ENOMEM;
		}
		/* Copy the BIOS-published window once, then compare normal memory. */
		memcpy_fromio(rom, mapping, WATCHGUARD_ROM_SIZE);
		iounmap(mapping);
		err = watchguard_identify_board(rom, WATCHGUARD_ROM_SIZE, NULL, &board);
		kfree(rom);
	}
	if (err)
		return err;

	watchguard_device =
		platform_device_register_data(NULL, WATCHGUARD_BOARD_DEVICE_NAME,
					      PLATFORM_DEVID_NONE, &board, sizeof(board));
	return PTR_ERR_OR_ZERO(watchguard_device);
}

static void __exit watchguard_platform_exit(void)
{
	platform_device_unregister(watchguard_device);
}

module_init(watchguard_platform_init);
module_exit(watchguard_platform_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("WatchGuard ROM and DMI board enumerator");
