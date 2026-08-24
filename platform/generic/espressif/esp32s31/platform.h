/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef __ESP32S31_PLATFORM_H__
#define __ESP32S31_PLATFORM_H__

#include <sbi/sbi_bitops.h>
#include <sbi/sbi_ecall.h>
#include <sbi/sbi_trap.h>
#include <sbi/sbi_types.h>

#define S31_FLASH_SIZE		0x01000000UL
#define S31_FLASH_XIP_START	0x40000000UL
#define S31_PSRAM_LINUX_START	0x50000000UL
#define S31_PSRAM_LINUX_END	0x51000000UL

#define S31_MCLICCFG		0x10800000UL
#define S31_CLICCFG_NMBITS_MASK	(3U << 5)
#define S31_CLICCFG_NMBITS_1	(1U << 5)
#define S31_CLIC_CTRL_BASE	0x10801000UL
#define S31_CLIC_WORD(id)	(S31_CLIC_CTRL_BASE + (4UL * (id)))
#define S31_CLIC_ATTR_S_EDGE	0x42
#define S31_CLIC_SINGLE_LEVEL	0xe0

void s31_apm_init(void);
void s31_tee_supervisor_priv_sel_to_m(void);
int s31_cache_vendor_ext(long funcid, struct sbi_trap_regs *regs,
			 struct sbi_ecall_return *out);
void s31_clic_local_init(void);
void s31_clic_delegate_runtime_sources(void);
void s31_console_init(void);
void s31_pma_init(void);
void s31_release_hart1(void);
int s31_services_register(void);
int s31_timer_init(void);

#endif
