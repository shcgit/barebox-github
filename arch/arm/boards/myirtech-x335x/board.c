// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2008 Raghavendra KH <r-khandenahally@ti.com>, Texas Instruments (http://www.ti.com/)
// SPDX-FileCopyrightText: 2012 Jan Luebbe <j.luebbe@pengutronix.de>

/**
 * @file
 * @brief BeagleBone Specific Board Initialization routines
 */

#include <common.h>
#include <init.h>
#include <driver.h>
#include <envfs.h>
#include <environment.h>
#include <globalvar.h>
#include <linux/sizes.h>
#include <net.h>
#include <bootsource.h>
#include <asm/armlinux.h>
#include <generated/mach-types.h>
#include <mach/am33xx-silicon.h>
#include <mach/sys_info.h>
#include <mach/syslib.h>
#include <mach/gpmc.h>
#include <linux/err.h>
#include <mach/am33xx-generic.h>
#include <i2c/i2c.h>

static struct omap_barebox_part myir_barebox_part = {
	.nand_offset = SZ_128K,
	.nand_size = SZ_512K,
};

static void myir_disable_device(struct device_node *root, const char *label)
{
	struct device_node *np = of_find_node_by_name(root, label);
	if (np)
		of_device_disable(np);
}

static int myir_probe_i2c(struct i2c_adapter *adapter, int addr)
{
	u8 buf[1];
	struct i2c_msg msg = {
		.addr = addr,
		.buf = buf,
		.len = sizeof(buf),
		.flags = I2C_M_RD,
	};

	return (i2c_transfer(adapter, &msg, 1) == 1) ? 0 : -ENODEV;
}

#define SGTL5000_ADDR	0x0a
#define AIC3100_ADDR	0x18

static int myir_board_fixup(struct device_node *root, void *unused)
{
	struct i2c_adapter *adapter = i2c_get_adapter(0);
	if (!adapter)
		return -ENODEV;

	if (myir_probe_i2c(adapter, SGTL5000_ADDR))
		myir_disable_device(root, "sound");
	else if (myir_probe_i2c(adapter, AIC3100_ADDR))
		myir_disable_device(root, "sound1");

	return 0;
}

static __init int myir_i2c_fixup(void)
{
	if (!of_machine_is_compatible("milas,informer-am335x") &&
	    !of_machine_is_compatible("milas,spider-am335x"))
		return 0;

	of_register_fixup(myir_board_fixup, NULL);

	return 0;
}
postcore_initcall(myir_i2c_fixup);

static __init int myir_devices_init(void)
{
	if (!of_machine_is_compatible("myir,myc-am335x"))
		return 0;

	am33xx_register_ethaddr(0, 0);
	am33xx_register_ethaddr(1, 1);

	switch (bootsource_get()) {
	case BOOTSOURCE_MMC:
		omap_set_bootmmc_devname("mmc0");
		break;
	case BOOTSOURCE_NAND:
		omap_set_barebox_part(&myir_barebox_part);
		break;
	default:
		break;
	}

	defaultenv_append_directory(defaultenv_beaglebone);

	if (IS_ENABLED(CONFIG_SHELL_NONE))
		return am33xx_of_register_bootdevice();

	return 0;
}
coredevice_initcall(myir_devices_init);