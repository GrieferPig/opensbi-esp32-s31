/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2019 Western Digital Corporation or its affiliates.
 *
 * Authors:
 *   Anup Patel <anup.patel@wdc.com>
 */

#ifndef __SBI_EMULATE_CSR_H__
#define __SBI_EMULATE_CSR_H__

#include <sbi/sbi_types.h>

struct sbi_trap_regs;

int sbi_emulate_csr_read(int csr_num, struct sbi_trap_regs *regs,
			 ulong *csr_val);

int sbi_emulate_csr_write(int csr_num, struct sbi_trap_regs *regs,
			  ulong csr_val);

/*
 * Some platforms only break a subset of S-mode CSR accesses from M-mode.
 * On ESP32-S31 this currently applies to the standard S-mode interrupt CSRs
 * (sie/sip and their RV32 high halves), while trap/MMU CSRs such as
 * stvec/sepc/stval/sstatus/sscratch/satp are expected to work normally.
 */
bool sbi_scsr_needs_shadow(int csr_num);
void sbi_scsr_write(int csr_num, ulong val);
ulong sbi_scsr_read(int csr_num);
ulong sbi_scsr_swap(int csr_num, ulong val);

/*
 * Raw shadow storage accessors for the CSRs that still require emulation.
 */
void sbi_scsr_shadow_write(int csr_num, ulong val);
ulong sbi_scsr_shadow_read(int csr_num);

#endif
