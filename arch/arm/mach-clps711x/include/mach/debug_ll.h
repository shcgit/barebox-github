/* SPDX-License-Identifier: GPL-2.0+ */
/* Author: Alexander Shiyan <shc_work@mail.ru> */

#ifndef __INCLUDE_MACH_DEBUG_LL_H__
#define __INCLUDE_MACH_DEBUG_LL_H__

#include <asm/io.h>
#include <mach/clps711x.h>

static inline void PUTC_LL(char ch)
{
	do {
	} while (readl(SYSFLG1) & SYSFLG_UTXFF);

	writew(ch, UARTDR1);

	do {
	} while (readl(SYSFLG1) & SYSFLG_UBUSY);
}

#endif