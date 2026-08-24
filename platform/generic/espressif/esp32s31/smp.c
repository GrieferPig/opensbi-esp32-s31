/* SPDX-License-Identifier: BSD-2-Clause */
#include <sbi/riscv_asm.h>
#include <sbi/riscv_io.h>

#include "platform.h"

#define S31_HP_CORESTALLED_ST		0x20586064UL
#define S31_HP_CORE1_STALLED		BIT(1)
#define S31_HP_CORE1_CTRL		0x20587020UL
#define S31_HP_CORE1_CLIC_CLK_EN	BIT(0)
#define S31_HP_CORE1_CPU_CLK_EN		BIT(1)
#define S31_HP_CORE1_GLOBAL_RST_EN	BIT(2)
#define S31_LP_CORE1_RESET_CTRL		0x20701040UL
#define S31_LP_CORE1_SW_RESET		BIT(20)
#define S31_PMU_CPU_STALL_SW		0x207041ecUL
#define S31_PMU_CORE1_STALL_SHIFT	16
#define S31_PMU_CORE1_STALL_MASK	(0xffUL << S31_PMU_CORE1_STALL_SHIFT)
#define S31_CACHE_L1_ICACHE_CTRL	0x2c000000UL
#define S31_CACHE_L1_ICACHE_SHUT_IBUS1	BIT(1)
#define S31_ROM_SET_APPCPU_BOOT_ADDR	0x2f8000a8UL
#define S31_CSR_PMACFG7			0xbc7
#define S31_CSR_PMAADDR7		0xbd7

static void __attribute__((naked, noreturn)) s31_hart1_entry(void)
{
	__asm__ __volatile__(
		"csrw 0xbc7, zero\n"
		"csrw 0xbd7, zero\n"
		"li t0, 0x147fffff\n"
		"csrw 0xbd7, t0\n"
		"li t0, 0xc000001d\n"
		"csrw 0xbc7, t0\n"
		"fence iorw, iorw\n"
		"li a0, 1\n"
		"li a1, 0\n"
		"li a2, 0\n"
		"j _start\n");
	__builtin_unreachable();
}

void s31_pma_init(void)
{
	csr_write_num(S31_CSR_PMACFG7, 0);
	csr_write_num(S31_CSR_PMAADDR7, 0);
	csr_write_num(S31_CSR_PMAADDR7, 0x147fffff);
	csr_write_num(S31_CSR_PMACFG7, 0xc000001d);
	RISCV_FENCE(iorw, iorw);
}

void s31_release_hart1(void)
{
	typedef void (*set_boot_addr_t)(u32 addr);
	u32 val;

	val = readl((void *)S31_CACHE_L1_ICACHE_CTRL);
	writel(val & ~S31_CACHE_L1_ICACHE_SHUT_IBUS1,
	       (void *)S31_CACHE_L1_ICACHE_CTRL);
	val = readl((void *)S31_PMU_CPU_STALL_SW);
	val = (val & ~S31_PMU_CORE1_STALL_MASK) |
	      (0x86UL << S31_PMU_CORE1_STALL_SHIFT);
	writel(val, (void *)S31_PMU_CPU_STALL_SW);
	val = readl((void *)S31_HP_CORE1_CTRL);
	val |= S31_HP_CORE1_CLIC_CLK_EN | S31_HP_CORE1_CPU_CLK_EN;
	val &= ~S31_HP_CORE1_GLOBAL_RST_EN;
	writel(val, (void *)S31_HP_CORE1_CTRL);
	((set_boot_addr_t)S31_ROM_SET_APPCPU_BOOT_ADDR)(
		(u32)(unsigned long)s31_hart1_entry);
	RISCV_FENCE(iorw, iorw);
	writel(readl((void *)S31_LP_CORE1_RESET_CTRL) |
	       S31_LP_CORE1_SW_RESET, (void *)S31_LP_CORE1_RESET_CTRL);
	val = readl((void *)S31_PMU_CPU_STALL_SW);
	val = (val & ~S31_PMU_CORE1_STALL_MASK) |
	      (0xffUL << S31_PMU_CORE1_STALL_SHIFT);
	writel(val, (void *)S31_PMU_CPU_STALL_SW);
	RISCV_FENCE(iorw, iorw);
	while (readl((void *)S31_HP_CORESTALLED_ST) & S31_HP_CORE1_STALLED)
		;
}
