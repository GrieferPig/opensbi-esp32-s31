/* SPDX-License-Identifier: BSD-2-Clause */
#include <sbi/riscv_io.h>
#include <sbi/sbi_console.h>

#include "platform.h"

#define S31_UART_BASE		0x2038a000UL
#define S31_UART_STATUS_REG	(S31_UART_BASE + 0x1c)
#define S31_UART_TXFIFO_CNT	GENMASK(23, 16)

static void s31_console_putc(char ch)
{
	while ((readl((void *)S31_UART_STATUS_REG) & S31_UART_TXFIFO_CNT) >= 127)
		;
	writel((u32)(u8)ch, (void *)S31_UART_BASE);
}

static struct sbi_console_device s31_console = {
	.name = "esp32s31-uart0",
	.console_putc = s31_console_putc,
};

void s31_console_init(void)
{
	sbi_console_set_device(&s31_console);
}

void sbi_platform_console_release(void)
{
	u32 i;

	for (i = 0; i < 1000000; i++) {
		if (!(readl((void *)S31_UART_STATUS_REG) & S31_UART_TXFIFO_CNT))
			break;
	}
	sbi_console_release_device();
}
