/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Derived from Espressif's ESP32-S31 OpenSBI port:
 * https://github.com/espressif/opensbi/commit/a93bce66b102966b0612c0afb5c9872a4737fe68
 *
 * Original ESP32-S31 port by Shreyash Bubane and Espressif contributors.
 * Modified for dual-hart SMP, direct S-mode timer delivery and XIP OpenSBI.
 */
#include <sbi/riscv_io.h>

#include "platform.h"

#define S31_TEE_BASE			0x20504000UL
#define S31_HP_APM_BASE			0x20504400UL
#define S31_HP_MEM_APM_BASE		0x20504800UL
#define S31_CPU_APM_BASE		0x20504c00UL
#define S31_LP_APM_BASE			0x20706c00UL
#define S31_APM_FILTER_EN_OFF		0x00UL
#define S31_APM_REGION0_ATTR_OFF	0x0cUL
#define S31_APM_REGION_STRIDE		0x0cUL
#define S31_APM_FUNC_CTRL_OFF		0xc4UL
#define S31_LP_APM_FUNC_CTRL_OFF	0xbcUL
#define S31_TEE_SUPERVISOR_PRIV_SEL	(S31_TEE_BASE + 0x200UL)

struct s31_apm_desc {
	unsigned long base;
	u32 regions;
	u32 paths;
	unsigned long func_ctrl_off;
};

static const struct s31_apm_desc s31_apm_descs[] = {
	{ S31_CPU_APM_BASE,    8,  0x0f, S31_APM_FUNC_CTRL_OFF },
	{ S31_HP_APM_BASE,     16, 0x7f, S31_APM_FUNC_CTRL_OFF },
	{ S31_HP_MEM_APM_BASE, 8,  0x3f, S31_APM_FUNC_CTRL_OFF },
	{ S31_LP_APM_BASE,     8,  0x0f, S31_LP_APM_FUNC_CTRL_OFF },
};

void s31_apm_init(void)
{
	u32 i, region;

	for (i = 0; i < array_size(s31_apm_descs); i++) {
		const struct s31_apm_desc *desc = &s31_apm_descs[i];

		for (region = 0; region < desc->regions; region++) {
			unsigned long attr = desc->base +
				S31_APM_REGION0_ATTR_OFF +
				S31_APM_REGION_STRIDE * region;

			writel(0, (void *)(attr - 8));
			writel(0xffffffff, (void *)(attr - 4));
			writel(0x7777, (void *)attr);
		}
		writel((1U << desc->regions) - 1U,
		       (void *)(desc->base + S31_APM_FILTER_EN_OFF));
		writel(desc->paths,
		       (void *)(desc->base + desc->func_ctrl_off));
	}
}

void s31_tee_supervisor_priv_sel_to_m(void)
{
	writel(BIT(0) | BIT(1), (void *)S31_TEE_SUPERVISOR_PRIV_SEL);
}
