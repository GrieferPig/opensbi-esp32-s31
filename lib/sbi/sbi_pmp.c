/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2019 Western Digital Corporation or its affiliates.
 *
 * Authors:
 *   Anup Patel <anup.patel@wdc.com>
 */

#include <sbi/riscv_asm.h>
#include <sbi/riscv_encoding.h>
#include <sbi/sbi_error.h>
#include <sbi/sbi_console.h>
#include <sbi/sbi_pmp.h>

static unsigned long ctz(unsigned long x)
{
	unsigned long ret = 0;

	if (x == 0)
		return 8 * sizeof(x);

	while (!(x & 1UL)) {
		ret++;
		x = x >> 1;
	}

	return ret;
}

bool sbi_pmp_is_enabled(pmp_t *pmp)
{
	/* If address matching bits are non-zero, the entry is enable */
	if (pmp->cfg & PMP_A)
		return true;

	return false;
}

int sbi_pmp_encode(pmp_t *pmp, unsigned long prot, unsigned long addr,
		    unsigned long log2len)
{
	/* check parameters */
	if (log2len > __riscv_xlen || log2len < PMP_SHIFT)
		return SBI_EINVAL;

	/* encode PMP config */
	prot &= ~PMP_A;
	prot |= (log2len == PMP_SHIFT) ? PMP_A_NA4 : PMP_A_NAPOT;
	pmp->cfg = prot;

	/* encode PMP address */
	if (log2len == PMP_SHIFT) {
		pmp->addr = (addr >> PMP_SHIFT);
	} else {
		unsigned long addrmask;
		if (log2len == __riscv_xlen) {
			/* For a region covering the entire address space,
			 * we need exactly (XLEN - PMP_SHIFT - 1) ones.
			 * -1UL would set all bits, which is invalid NAPOT. */
			addrmask = (1UL << (log2len - PMP_SHIFT - 1)) - 1;
			pmp->addr = ((addr >> PMP_SHIFT) & ~addrmask) | addrmask;
		} else {
			addrmask = (1UL << (log2len - PMP_SHIFT)) - 1;
			pmp->addr = ((addr >> PMP_SHIFT) & ~addrmask);
			pmp->addr |= (addrmask >> 1);
		}
	}

	return SBI_OK;
}

int sbi_pmp_decode(pmp_t *pmp, unsigned long *prot_out, unsigned long *addr_out,
		    unsigned long *log2len)
{
	unsigned long prot;
	unsigned long t1, addr, len;

	/* check parameters */
	if (!prot_out || !addr_out || !log2len)
		return SBI_EINVAL;
	*prot_out = *addr_out = *log2len = 0;

	/* decode PMP config */
	prot = pmp->cfg;

	/* decode PMP address */
	if ((prot & PMP_A) == PMP_A_NAPOT) {
		addr = pmp->addr;
		t1 = ctz(~addr);
		if (t1 + PMP_SHIFT + 1 == __riscv_xlen || addr == -1UL) {
			/* Full-address-space NAPOT encoding (or legacy -1UL) */
			addr	= 0;
			len	= __riscv_xlen;
		} else {
			addr	= (addr & ~((1UL << t1) - 1)) << PMP_SHIFT;
			len	= (t1 + PMP_SHIFT + 1);
		}
	} else {
		addr	= pmp->addr << PMP_SHIFT;
		len	= PMP_SHIFT;
	}

	/* return details */
	*prot_out    = prot;
	*addr_out    = addr;
	*log2len     = len;

	return SBI_OK;
}
