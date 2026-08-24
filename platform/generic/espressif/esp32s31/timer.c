/* SPDX-License-Identifier: BSD-2-Clause */
#include <sbi/riscv_io.h>
#include <sbi/sbi_timer.h>

#include "platform.h"

#define S31_CLINT_BASE		0x10000000UL
#define S31_MTIMECMP_LO		(S31_CLINT_BASE + 0x4000)
#define S31_MTIMECMP_HI		(S31_CLINT_BASE + 0x4004)
#define S31_MTIMECTL		(S31_CLINT_BASE + 0x4010)
#define S31_MTIME_LO		(S31_CLINT_BASE + 0xbff8)
#define S31_MTIME_HI		(S31_CLINT_BASE + 0xbffc)
#define S31_TIMEBASE_HZ		320000000UL

static u64 s31_timer_value(void)
{
	u32 lo, hi, tmp;

	do {
		hi = readl_relaxed((void *)S31_MTIME_HI);
		lo = readl_relaxed((void *)S31_MTIME_LO);
		tmp = readl_relaxed((void *)S31_MTIME_HI);
	} while (hi != tmp);
	return ((u64)hi << 32) | lo;
}

static void s31_timer_event_stop(void)
{
	volatile u8 *timer = (u8 *)S31_CLIC_WORD(7);

	writel_relaxed(0xffffffff, (void *)S31_MTIMECMP_HI);
	writel_relaxed(0xffffffff, (void *)S31_MTIMECMP_LO);
	timer[0] = 0;
}

static void s31_timer_event_start(u64 next_event)
{
	volatile u8 *timer = (u8 *)S31_CLIC_WORD(7);

	timer[0] = 0;
	timer[1] = 1;
	timer[2] = S31_CLIC_ATTR_S_EDGE;
	timer[3] = S31_CLIC_SINGLE_LEVEL;
	writel_relaxed(0xffffffff, (void *)S31_MTIMECMP_HI);
	writel_relaxed((u32)next_event, (void *)S31_MTIMECMP_LO);
	writel_relaxed((u32)(next_event >> 32), (void *)S31_MTIMECMP_HI);
}

static struct sbi_timer_device s31_timer = {
	.name = "esp32s31-mtimer",
	.timer_freq = S31_TIMEBASE_HZ,
	.timer_value = s31_timer_value,
	.timer_event_start = s31_timer_event_start,
	.timer_event_stop = s31_timer_event_stop,
};

int s31_timer_init(void)
{
	s31_clic_delegate_runtime_sources();
	writel_relaxed(1, (void *)S31_MTIMECTL);
	s31_timer_event_stop();
	sbi_timer_set_device(&s31_timer);
	return 0;
}
