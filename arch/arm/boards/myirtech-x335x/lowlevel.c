#include <io.h>
#include <asm/barebox-arm-head.h>
#include <asm/barebox-arm.h>
#include <debug_ll.h>
#include <init.h>
#include <linux/sizes.h>
#include <mach/am33xx-clock.h>
#include <mach/am33xx-generic.h>
#include <mach/am33xx-mux.h>
#include <mach/generic.h>
#include <mach/sdrc.h>
#include <mach/sys_info.h>
#include <mach/wdt.h>

#define AM335X_ZCZ_1000		0x1c2f

static const struct am33xx_ddr_data ddr3_data = {
	.rd_slave_ratio0	= 0x38,
	.wr_dqs_slave_ratio0	= 0x44,
	.fifo_we_slave_ratio0	= 0x94,
	.wr_slave_ratio0	= 0x7D,
	.use_rank0_delay	= 0x01,//
	.dll_lock_diff0		= 0x0,//
};

static const struct am33xx_cmd_control ddr3_cmd_ctrl = {
	.slave_ratio0	= 0x80,
	.dll_lock_diff0	= 0x1,//
	.invert_clkout0	= 0x0,
	.slave_ratio1	= 0x80,
	.dll_lock_diff1	= 0x1,//
	.invert_clkout1	= 0x0,
	.slave_ratio2	= 0x80,
	.dll_lock_diff2	= 0x1,//
	.invert_clkout2	= 0x0,
};

/* CPU module contains 512MB (2*256MB) DDR3 SDRAM (2*128MB compatible),
 * so we configure EMIF for 512MB then detect real size of memory.
 */
static const struct am33xx_emif_regs ddr3_regs = {
	.emif_read_latency	= 0x00100007,
	.emif_tim1		= 0x0aaad4db,
	.emif_tim2		= 0x266b7fda,
	.emif_tim3		= 0x501f867f,
	.zq_config		= 0x50074be4,
	.sdram_config		= 0x61c05332,
	.sdram_config2		= 0x0,
	.sdram_ref_ctrl		= 0xc30,
};

extern char __dtb_z_am335x_myirtech_myd_am335x_start[];

ENTRY_FUNCTION(start_am33xx_myirtech_sram, bootinfo, r1, r2)
{
	int mpupll;
	void *fdt;

	am33xx_save_bootinfo((void *)bootinfo);

	relocate_to_current_adr();
	setup_c();

	fdt = __dtb_z_am335x_myirtech_myd_am335x_start;

	/* WDT1 is already running when the bootloader gets control
	 * Disable it to avoid "random" resets
	 */
	__raw_writel(WDT_DISABLE_CODE1, AM33XX_WDT_REG(WSPR));
	while (__raw_readl(AM33XX_WDT_REG(WWPS)) != 0x0);
	__raw_writel(WDT_DISABLE_CODE2, AM33XX_WDT_REG(WSPR));
	while (__raw_readl(AM33XX_WDT_REG(WWPS)) != 0x0);

	mpupll = MPUPLL_M_800;
	if (am33xx_get_cpu_rev() == AM335X_ES2_1) {
		u32 deviceid = readl(AM33XX_EFUSE_SMA) & 0x1fff;
		if (deviceid == AM335X_ZCZ_1000)
			mpupll = MPUPLL_M_1000;
	}

	am33xx_pll_init(mpupll, DDRPLL_M_400);

	am335x_sdram_init(0x18b, &ddr3_cmd_ctrl, &ddr3_regs, &ddr3_data);

	if (IS_ENABLED(CONFIG_DEBUG_LL)) {
		am33xx_uart_soft_reset((void *)AM33XX_UART0_BASE);
		am33xx_enable_uart0_pin_mux();
		omap_uart_lowlevel_init((void *)AM33XX_UART0_BASE);
		putc_ll('>');
	}

	barebox_arm_entry(AM33XX_DRAM_ADDR_SPACE_START, SZ_256M, fdt);
}

ENTRY_FUNCTION(start_am33xx_myirtech_sdram, r0, r1, r2)
{
	void *fdt;
	u32 sdram_size;

	fdt = __dtb_z_am335x_myirtech_myd_am335x_start;

	fdt += get_runtime_offset();

	/* Detect 256M/512M module variant */
	__raw_writel(SZ_512M, AM33XX_DRAM_ADDR_SPACE_START + SZ_256M);
	__raw_writel(SZ_256M, AM33XX_DRAM_ADDR_SPACE_START + 0);
	sdram_size = __raw_readl(AM33XX_DRAM_ADDR_SPACE_START + SZ_256M);

	barebox_arm_entry(AM33XX_DRAM_ADDR_SPACE_START, sdram_size, fdt);
}