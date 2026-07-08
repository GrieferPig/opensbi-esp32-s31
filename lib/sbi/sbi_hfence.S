/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2019 Western Digital Corporation or its affiliates.
 *
 * Authors:
 *   Anup Patel <anup.patel@wdc.com>
 *   Atish Patra <anup.patel@wdc.com>
 */

	/*
	 * HFENCE.GVMA rs1, rs2
	 * HFENCE.GVMA zero, rs2
	 * HFENCE.GVMA rs1
	 * HFENCE.GVMA
	 *
	 * rs1!=zero and rs2!=zero ==> HFENCE.GVMA rs1, rs2
	 * rs1==zero and rs2!=zero ==> HFENCE.GVMA zero, rs2
	 * rs1!=zero and rs2==zero ==> HFENCE.GVMA rs1
	 * rs1==zero and rs2==zero ==> HFENCE.GVMA
	 *
	 * Instruction encoding of HFENCE.GVMA is:
	 * 0110001 rs2(5) rs1(5) 000 00000 1110011
	 */

	.align 3
	.global __sbi_hfence_gvma_vmid_gpa
__sbi_hfence_gvma_vmid_gpa:
	ret

	.align 3
	.global __sbi_hfence_gvma_vmid
__sbi_hfence_gvma_vmid:
	ret

	.align 3
	.global __sbi_hfence_gvma_gpa
__sbi_hfence_gvma_gpa:
	ret

	.align 3
	.global __sbi_hfence_gvma_all
__sbi_hfence_gvma_all:
	ret

	/*
	 * HFENCE.VVMA rs1, rs2
	 * HFENCE.VVMA zero, rs2
	 * HFENCE.VVMA rs1
	 * HFENCE.VVMA
	 *
	 * rs1!=zero and rs2!=zero ==> HFENCE.VVMA rs1, rs2
	 * rs1==zero and rs2!=zero ==> HFENCE.VVMA zero, rs2
	 * rs1!=zero and rs2==zero ==> HFENCE.VVMA rs1
	 * rs1==zero and rs2==zero ==> HFENCE.vVMA
	 *
	 * Instruction encoding of HFENCE.VVMA is:
	 * 0010001 rs2(5) rs1(5) 000 00000 1110011
	 */

	.align 3
	.global __sbi_hfence_vvma_asid_va
__sbi_hfence_vvma_asid_va:
	ret

	.align 3
	.global __sbi_hfence_vvma_asid
__sbi_hfence_vvma_asid:
	ret

	.align 3
	.global __sbi_hfence_vvma_va
__sbi_hfence_vvma_va:
	ret

	.align 3
	.global __sbi_hfence_vvma_all
__sbi_hfence_vvma_all:
	ret
