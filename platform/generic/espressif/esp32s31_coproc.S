/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * ESP32-S31 HWLoop/PIE context accessors.
 *
 * The state-control CSRs are M-mode CSRs, so Linux asks OpenSBI to perform
 * these operations.  The raw PIE encodings are generated from ESP-IDF's
 * xespv2p2 assembler definitions.  Keeping the encodings here also lets the
 * normal RV32IMAC Linux/OpenSBI bootstrap compiler build the firmware.
 *
 * struct s31_coproc_state layout (shared with Linux):
 *   0x00..0x17  HWLoop loop0/loop1 start, end and count
 *   0x18..0x1f  padding
 *   0x20..0xf7  PIE state (216 bytes, ESP-IDF RvPIESaveArea layout)
 */

#include <sbi/riscv_asm.h>

#define CSR_HWLP_STATE          0x7f1
#define CSR_PIE_STATE           0x7f2
#define CSR_LOOP0_START         0x7c6
#define CSR_LOOP0_END           0x7c7
#define CSR_LOOP0_COUNT         0x7c8
#define CSR_LOOP1_START         0x7c9
#define CSR_LOOP1_END           0x7ca
#define CSR_LOOP1_COUNT         0x7cb
#define S31_PIE_OFFSET          0x20
#define COPROC_INITIAL_STATE    1
#define COPROC_CLEAN_STATE      2
#define COPROC_DIRTY_STATE      3

	.section .text
	.align 2

	.globl s31_coproc_save
	.type s31_coproc_save, @function
s31_coproc_save:
	/* Match IDF's hwlp_enable: enable before every register access. */
	csrwi	CSR_HWLP_STATE, COPROC_INITIAL_STATE
	csrr	t0, CSR_LOOP0_START
	sw	t0, 0(a0)
	csrr	t0, CSR_LOOP0_END
	sw	t0, 4(a0)
	csrr	t0, CSR_LOOP0_COUNT
	sw	t0, 8(a0)
	csrr	t0, CSR_LOOP1_START
	sw	t0, 12(a0)
	csrr	t0, CSR_LOOP1_END
	sw	t0, 16(a0)
	csrr	t0, CSR_LOOP1_COUNT
	sw	t0, 20(a0)

	/* Match IDF's pie_enable: enable before every register access. */
	csrwi	CSR_PIE_STATE, COPROC_INITIAL_STATE
	addi	a0, a0, S31_PIE_OFFSET
	/* q0..q7, 16 bytes each, post-increment by 16. */
	.word	0x8311009f
	.word	0x8311049f
	.word	0x8311089f
	.word	0x83110c9f
	.word	0x8311109f
	.word	0x8311149f
	.word	0x8311189f
	.word	0x83111c9f
	/* QACC_L/QACC_H (four 128-bit quarters) and UA_STATE. */
	.word	0x1011021f
	.word	0x1001021f
	.word	0x1811021f
	.word	0x1801021f
	.word	0x1011051f
	/* XACC occupies five bytes at the current address. */
	.word	0x0011019f
	/* Pack FFT_BIT_WIDTH, SAR_BYTES and SAR exactly as ESP-IDF does. */
	.word	0x88c0219b	/* esp.movx.r.sar.bytes a1 */
	slli	a2, a1, 4
	.word	0x8480219b	/* esp.movx.r.fft.bit.width a1 */
	or	a2, a2, a1
	sb	a2, 5(a0)
	.word	0x80c0219b	/* esp.movx.r.sar a1 */
	sb	a1, 6(a0)
	ret
	.size s31_coproc_save, .-s31_coproc_save

	.globl s31_coproc_restore
	.type s31_coproc_restore, @function
s31_coproc_restore:
	/* IDF restores registers from CLEAN and leaves active loops DIRTY. */
	csrwi	CSR_HWLP_STATE, COPROC_CLEAN_STATE
	lw	t0, 0(a0)
	csrw	CSR_LOOP0_START, t0
	lw	t0, 4(a0)
	csrw	CSR_LOOP0_END, t0
	lw	t0, 8(a0)
	csrw	CSR_LOOP0_COUNT, t0
	lw	t0, 12(a0)
	csrw	CSR_LOOP1_START, t0
	lw	t0, 16(a0)
	csrw	CSR_LOOP1_END, t0
	lw	t0, 20(a0)
	csrw	CSR_LOOP1_COUNT, t0
	lw	t0, 8(a0)
	bnez	t0, 1f
	lw	t0, 20(a0)
	bnez	t0, 1f
	csrwi	CSR_HWLP_STATE, COPROC_CLEAN_STATE
	j	2f
1:
	csrwi	CSR_HWLP_STATE, COPROC_DIRTY_STATE
2:

	csrwi	CSR_PIE_STATE, COPROC_INITIAL_STATE
	addi	a0, a0, S31_PIE_OFFSET
	.word	0x0311009f
	.word	0x0311049f
	.word	0x0311089f
	.word	0x03110c9f
	.word	0x0311109f
	.word	0x0311149f
	.word	0x0311189f
	.word	0x03111c9f
	.word	0x0011021f
	.word	0x0001021f
	.word	0x0811021f
	.word	0x0801021f
	.word	0x0011051f
	.word	0x0011031f
	lbu	a1, 5(a0)
	.word	0x9481a01b	/* esp.movx.w.fft.bit.width a1 */
	srli	a1, a1, 4
	.word	0x98c1a01b	/* esp.movx.w.sar.bytes a1 */
	lbu	a1, 6(a0)
	.word	0x90c1a01b	/* esp.movx.w.sar a1 */
	ret
	.size s31_coproc_restore, .-s31_coproc_restore
