/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _WATCHGUARD_CONTROLLER_H
#define _WATCHGUARD_CONTROLLER_H

#include "board-id.h"

struct pci_dev;

/* Return one matching SMBus controller with a reference, or an error pointer. */
struct pci_dev *watchguard_find_smbus(enum watchguard_board board);

#endif
