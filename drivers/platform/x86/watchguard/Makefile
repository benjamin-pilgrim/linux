# SPDX-License-Identifier: GPL-2.0-only
obj-$(CONFIG_WATCHGUARD_PLATFORM) += watchguard-platform.o
watchguard-platform-y := platform.o board-id.o
obj-$(CONFIG_WATCHGUARD_BOARD) += watchguard-board.o
watchguard-board-y := board.o controller.o identity.o route.o m270-model.o
obj-$(CONFIG_NCB_WG2511_PANEL) += ncb-wg2511-panel.o
ncb-wg2511-panel-y := m270-panel.o
obj-$(CONFIG_NCB_WG2511_DSA) += ncb-wg2511-switch.o
ncb-wg2511-switch-y := m270.o switch.o
obj-$(CONFIG_WATCHGUARD_KUNIT_TEST) += board-id-test.o controller-test.o identity-test.o route-test.o
obj-$(CONFIG_WATCHGUARD_M270_KUNIT_TEST) += m270-test.o
