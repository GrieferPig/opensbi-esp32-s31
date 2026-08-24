/* SPDX-License-Identifier: BSD-2-Clause */
#include <sbi/riscv_asm.h>
#include <sbi/riscv_io.h>

#include "platform.h"

#define S31_CLIC_NUM_SLOTS	128
#define S31_INTMATRIX_BASE	0x20585000UL
#define S31_INTMATRIX_STRIDE	0x800UL
#define S31_INTMATRIX_LAST_MAP	0x2a0UL
#define S31_CLIC_ATTR_M_EDGE	0xc2
#define S31_CSR_MINTTHRESH	0x347

static void s31_clic_local_reset(void)
{
	ulong hartid = current_hartid();
	ulong matrix = S31_INTMATRIX_BASE + hartid * S31_INTMATRIX_STRIDE;
	int i;

	for (i = 0; i <= S31_INTMATRIX_LAST_MAP; i += sizeof(u32))
		writel(0, (void *)(matrix + i));
	for (i = 0; i < S31_CLIC_NUM_SLOTS; i++) {
		volatile u8 *slot = (u8 *)S31_CLIC_WORD(i);

		slot[0] = 0;
		slot[1] = 0;
		slot[2] = 0;
		slot[3] = 0;
	}
	RISCV_FENCE(iorw, iorw);
}

void s31_clic_local_init(void)
{
	volatile u8 *sipi = (u8 *)S31_CLIC_WORD(1);
	volatile u8 *swi = (u8 *)S31_CLIC_WORD(3);
	volatile u8 *timer = (u8 *)S31_CLIC_WORD(7);

	s31_clic_local_reset();
	writel((readl((void *)S31_MCLICCFG) & ~S31_CLICCFG_NMBITS_MASK) |
	       S31_CLICCFG_NMBITS_1, (void *)S31_MCLICCFG);
	csr_write(S31_CSR_MINTTHRESH, S31_CLIC_SINGLE_LEVEL);
	csr_write(0x147, 0);

	sipi[0] = 0;
	sipi[2] = S31_CLIC_ATTR_S_EDGE;
	sipi[3] = S31_CLIC_SINGLE_LEVEL;
	sipi[1] = 1;

	/* Linux owns runtime IPIs through native S-mode doorbells ID40/41. */
	swi[1] = 0;
	swi[0] = 0;
	swi[2] = S31_CLIC_ATTR_M_EDGE;
	swi[3] = S31_CLIC_SINGLE_LEVEL;

	timer[0] = 0;
	timer[2] = S31_CLIC_ATTR_S_EDGE;
	timer[3] = S31_CLIC_SINGLE_LEVEL;
	timer[1] = 1;
}

void s31_clic_delegate_runtime_sources(void)
{
	volatile u8 *timer = (u8 *)S31_CLIC_WORD(7);
	const u8 s_level_attr = 0x40;
	int i;

	writel((readl((void *)S31_MCLICCFG) & ~S31_CLICCFG_NMBITS_MASK) |
	       S31_CLICCFG_NMBITS_1, (void *)S31_MCLICCFG);
	timer[0] = 0;
	timer[2] = S31_CLIC_ATTR_S_EDGE;
	timer[3] = S31_CLIC_SINGLE_LEVEL;
	timer[1] = 1;

	for (i = 16; i <= 47; i++) {
		volatile u8 *ext = (u8 *)S31_CLIC_WORD(i);

		ext[0] = 0;
		ext[2] = s_level_attr;
		ext[3] = S31_CLIC_SINGLE_LEVEL;
		ext[1] = 0;
	}
}
