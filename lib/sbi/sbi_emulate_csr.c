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
#include <sbi/sbi_bitops.h>
#include <sbi/sbi_emulate_csr.h>
#include <sbi/sbi_error.h>
#include <sbi/sbi_hart.h>
#include <sbi/sbi_hartmask.h>
#include <sbi/sbi_scratch.h>
#include <sbi/sbi_timer.h>
#include <sbi/sbi_trap.h>

static bool hpm_allowed(int hpm_num, ulong prev_mode, bool virt)
{
	ulong cen = -1UL;
	struct sbi_scratch *scratch = sbi_scratch_thishart_ptr();

	if (prev_mode <= PRV_S) {
		if (sbi_hart_priv_version(scratch) >= SBI_HART_PRIV_VER_1_10) {
			cen &= csr_read(CSR_MCOUNTEREN);
			if (virt)
				cen &= csr_read(CSR_HCOUNTEREN);
		} else {
			cen = 0;
		}
	}
	if (prev_mode == PRV_U) {
		if (sbi_hart_priv_version(scratch) >= SBI_HART_PRIV_VER_1_10)
			cen &= csr_read(CSR_SCOUNTEREN);
		else
			cen = 0;
	}

	return ((cen >> hpm_num) & 1) ? true : false;
}

int sbi_emulate_csr_read(int csr_num, struct sbi_trap_regs *regs,
			 ulong *csr_val)
{
	int ret = 0;
	struct sbi_scratch *scratch = sbi_scratch_thishart_ptr();
	ulong prev_mode = sbi_mstatus_prev_mode(regs->mstatus);
	bool virt = sbi_regs_from_virt(regs);

	switch (csr_num) {
	case CSR_HTIMEDELTA:
		if (prev_mode == PRV_S && !virt)
			*csr_val = sbi_timer_get_delta();
		else
			ret = SBI_ENOTSUPP;
		break;
	case CSR_CYCLE:
		if (!hpm_allowed(csr_num - CSR_CYCLE, prev_mode, virt))
			return SBI_ENOTSUPP;
		*csr_val = csr_read(CSR_MCYCLE);
		break;
	case CSR_TIME:
		if (!hpm_allowed(csr_num - CSR_CYCLE, prev_mode, virt))
			return SBI_ENOTSUPP;
		/*
		 * We emulate TIME CSR for both Host (HS/U-mode) and
		 * Guest (VS/VU-mode).
		 */
		*csr_val = (virt) ? sbi_timer_virt_value():
				    sbi_timer_value();
		break;
	case CSR_INSTRET:
		if (!hpm_allowed(csr_num - CSR_CYCLE, prev_mode, virt))
			return SBI_ENOTSUPP;
		*csr_val = csr_read(CSR_MINSTRET);
		break;

#if __riscv_xlen == 32
	case CSR_HTIMEDELTAH:
		if (prev_mode == PRV_S && !virt)
			*csr_val = sbi_timer_get_delta() >> 32;
		else
			ret = SBI_ENOTSUPP;
		break;
	case CSR_CYCLEH:
		if (!hpm_allowed(csr_num - CSR_CYCLEH, prev_mode, virt))
			return SBI_ENOTSUPP;
		*csr_val = csr_read(CSR_MCYCLEH);
		break;
	case CSR_TIMEH:
		/* Refer comments on TIME CSR above. */
		*csr_val = (virt) ? sbi_timer_virt_value() >> 32:
				    sbi_timer_value() >> 32;
		break;
	case CSR_INSTRETH:
		if (!hpm_allowed(csr_num - CSR_CYCLEH, prev_mode, virt))
			return SBI_ENOTSUPP;
		*csr_val = csr_read(CSR_MINSTRETH);
		break;
#endif

#define switchcase_hpm(__uref, __mref, __csr)				\
	case __csr:							\
		if (sbi_hart_mhpm_mask(scratch) & (1 << (__csr - __uref)))\
			return SBI_ENOTSUPP;				\
		if (!hpm_allowed(__csr - __uref, prev_mode, virt))	\
			return SBI_ENOTSUPP;				\
		*csr_val = csr_read(__mref + __csr - __uref);		\
		break;
#define switchcase_hpm_2(__uref, __mref, __csr)			\
	switchcase_hpm(__uref, __mref, __csr + 0)			\
	switchcase_hpm(__uref, __mref, __csr + 1)
#define switchcase_hpm_4(__uref, __mref, __csr)			\
	switchcase_hpm_2(__uref, __mref, __csr + 0)			\
	switchcase_hpm_2(__uref, __mref, __csr + 2)
#define switchcase_hpm_8(__uref, __mref, __csr)			\
	switchcase_hpm_4(__uref, __mref, __csr + 0)			\
	switchcase_hpm_4(__uref, __mref, __csr + 4)
#define switchcase_hpm_16(__uref, __mref, __csr)			\
	switchcase_hpm_8(__uref, __mref, __csr + 0)			\
	switchcase_hpm_8(__uref, __mref, __csr + 8)

	switchcase_hpm(CSR_CYCLE, CSR_MCYCLE, CSR_HPMCOUNTER3)
	switchcase_hpm_4(CSR_CYCLE, CSR_MCYCLE, CSR_HPMCOUNTER4)
	switchcase_hpm_8(CSR_CYCLE, CSR_MCYCLE, CSR_HPMCOUNTER8)
	switchcase_hpm_16(CSR_CYCLE, CSR_MCYCLE, CSR_HPMCOUNTER16)

#if __riscv_xlen == 32
	switchcase_hpm(CSR_CYCLEH, CSR_MCYCLEH, CSR_HPMCOUNTER3H)
	switchcase_hpm_4(CSR_CYCLEH, CSR_MCYCLEH, CSR_HPMCOUNTER4H)
	switchcase_hpm_8(CSR_CYCLEH, CSR_MCYCLEH, CSR_HPMCOUNTER8H)
	switchcase_hpm_16(CSR_CYCLEH, CSR_MCYCLEH, CSR_HPMCOUNTER16H)
#endif

#undef switchcase_hpm_16
#undef switchcase_hpm_8
#undef switchcase_hpm_4
#undef switchcase_hpm_2
#undef switchcase_hpm

	default:
		ret = SBI_ENOTSUPP;
		break;
	}

	return ret;
}

int sbi_emulate_csr_write(int csr_num, struct sbi_trap_regs *regs,
			  ulong csr_val)
{
	int ret = 0;
	ulong prev_mode = sbi_mstatus_prev_mode(regs->mstatus);
	bool virt = sbi_regs_from_virt(regs);

	switch (csr_num) {
	case CSR_HTIMEDELTA:
		if (prev_mode == PRV_S && !virt)
			sbi_timer_set_delta(csr_val);
		else
			ret = SBI_ENOTSUPP;
		break;
#if __riscv_xlen == 32
	case CSR_HTIMEDELTAH:
		if (prev_mode == PRV_S && !virt)
			sbi_timer_set_delta_upper(csr_val);
		else
			ret = SBI_ENOTSUPP;
		break;
#endif
	default:
		ret = SBI_ENOTSUPP;
		break;
	}

	return ret;
}

/* Per-hart caches, matching the official CLIC CSR-emulation model. */
static ulong sbi_sie_cache[SBI_HARTMASK_MAX_BITS];
static ulong sbi_sip_cache[SBI_HARTMASK_MAX_BITS];
#if __riscv_xlen == 32
static ulong sbi_sieh_cache[SBI_HARTMASK_MAX_BITS];
static ulong sbi_siph_cache[SBI_HARTMASK_MAX_BITS];
#endif

bool sbi_scsr_needs_shadow(int csr_num)
{
#ifdef CONFIG_PLATFORM_ESPRESSIF_ESP32S31
	switch (csr_num) {
	case CSR_SIE:
	case CSR_SIP:
#if __riscv_xlen == 32
	case CSR_SIEH:
	case CSR_SIPH:
#endif
		return true;
	default:
		return false;
	}
#else
	return false;
#endif
}

void sbi_scsr_write(int csr_num, ulong val)
{
	if (sbi_scsr_needs_shadow(csr_num)) {
		sbi_scsr_shadow_write(csr_num, val);
		return;
	}

	switch (csr_num) {
	case CSR_SSTATUS:
		csr_write(CSR_SSTATUS, val);
		break;
	case CSR_STVEC:
		csr_write(CSR_STVEC, val);
		break;
	case CSR_SSCRATCH:
		csr_write(CSR_SSCRATCH, val);
		break;
	case CSR_SEPC:
		csr_write(CSR_SEPC, val);
		break;
	case CSR_SCAUSE:
		csr_write(CSR_SCAUSE, val);
		break;
	case CSR_STVAL:
		csr_write(CSR_STVAL, val);
		break;
	case CSR_SATP:
		csr_write(CSR_SATP, val);
		break;
	default:
		break;
	}
}

ulong sbi_scsr_read(int csr_num)
{
	if (sbi_scsr_needs_shadow(csr_num))
		return sbi_scsr_shadow_read(csr_num);

	switch (csr_num) {
	case CSR_SSTATUS:
		return csr_read(CSR_SSTATUS);
	case CSR_STVEC:
		return csr_read(CSR_STVEC);
	case CSR_SSCRATCH:
		return csr_read(CSR_SSCRATCH);
	case CSR_SEPC:
		return csr_read(CSR_SEPC);
	case CSR_SCAUSE:
		return csr_read(CSR_SCAUSE);
	case CSR_STVAL:
		return csr_read(CSR_STVAL);
	case CSR_SATP:
		return csr_read(CSR_SATP);
	default:
		return 0;
	}
}

ulong sbi_scsr_swap(int csr_num, ulong val)
{
	if (sbi_scsr_needs_shadow(csr_num)) {
		ulong old = sbi_scsr_shadow_read(csr_num);

		sbi_scsr_shadow_write(csr_num, val);
		return old;
	}

	switch (csr_num) {
	case CSR_SSTATUS:
		return csr_swap(CSR_SSTATUS, val);
	case CSR_STVEC:
		return csr_swap(CSR_STVEC, val);
	case CSR_SSCRATCH:
		return csr_swap(CSR_SSCRATCH, val);
	case CSR_SEPC:
		return csr_swap(CSR_SEPC, val);
	case CSR_SCAUSE:
		return csr_swap(CSR_SCAUSE, val);
	case CSR_STVAL:
		return csr_swap(CSR_STVAL, val);
	case CSR_SATP:
		return csr_swap(CSR_SATP, val);
	default:
		return 0;
	}
}

void sbi_scsr_shadow_write(int csr_num, ulong val)
{
	ulong hartid = current_hartid();

	if (hartid >= SBI_HARTMASK_MAX_BITS)
		return;
	switch (csr_num) {
	case CSR_SIE:
		sbi_sie_cache[hartid] = val;
		break;
	case CSR_SIP:
		sbi_sip_cache[hartid] = val;
		break;
#if __riscv_xlen == 32
	case CSR_SIEH:
		sbi_sieh_cache[hartid] = val;
		break;
	case CSR_SIPH:
		sbi_siph_cache[hartid] = val;
		break;
#endif
	default:
		break;
	}
}

ulong sbi_scsr_shadow_read(int csr_num)
{
	ulong hartid = current_hartid();

	if (hartid >= SBI_HARTMASK_MAX_BITS)
		return 0;
	switch (csr_num) {
	case CSR_SIE:
		return sbi_sie_cache[hartid];
	case CSR_SIP:
		return sbi_sip_cache[hartid];
#if __riscv_xlen == 32
	case CSR_SIEH:
		return sbi_sieh_cache[hartid];
	case CSR_SIPH:
		return sbi_siph_cache[hartid];
#endif
	default:
		return 0;
	}
}
