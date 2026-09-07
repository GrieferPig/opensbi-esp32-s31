/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Derived from Espressif's ESP32-S31 OpenSBI port:
 * https://github.com/espressif/opensbi/commit/a93bce66b102966b0612c0afb5c9872a4737fe68
 *
 * Original ESP32-S31 port by Shreyash Bubane and Espressif contributors.
 * Modified for dual-hart SMP, direct S-mode timer delivery and XIP OpenSBI.
 */
/* ESP32-S31 generic-platform integration; silicon blocks live in esp32s31/. */

#include <libfdt.h>
#include <platform_override.h>
#include <sbi/riscv_asm.h>
#include <sbi/sbi_domain.h>
#include <sbi/sbi_emulate_csr.h>
#include <sbi/sbi_hart.h>
#include <sbi/sbi_heap.h>
#include <sbi/sbi_platform.h>
#include <sbi/sbi_scratch.h>
#include <sbi/sbi_system.h>
#include <sbi_utils/fdt/fdt_helper.h>

#include "esp32s31/platform.h"

extern struct sbi_platform platform;
extern unsigned int sbi_hart_priv_version_override;

#define S31_CSR_MEXSTATUS	0x7f2
#define S31_FIXUP_FDT_SIZE	0x3000

static unsigned long s31_uboot_dtb;

static int s31_misa_extension(char ext)
{
	switch (ext) {
	case 'I': case 'M': case 'A': case 'F': case 'C':
	case 'S': case 'U':
		return 1;
	default:
		return 0;
	}
}

static int s31_misa_xlen(void)
{
	return 1;
}

static int s31_early_init(bool cold_boot)
{
	void *fbuf;

	s31_console_init();
	s31_pma_init();
	csr_write(S31_CSR_MEXSTATUS, 1);

	if (!cold_boot) {
		/*
		 * A system-suspend warmboot resumes the Linux-owned CLIC and
		 * interrupt-matrix state.  Resetting all slots here discards active
		 * S-mode routes immediately before Linux restores SIE.  Hart start
		 * and hotplug still require the normal per-hart reset path.
		 */
		if (!sbi_system_is_suspended())
			s31_clic_local_init();
		return 0;
	}

	/* The generic SUSP ecall probes its backend during extensions_init. */
	s31_system_suspend_register();

	/* Generic OpenSBI fixups use a private writable copy.  Keep the original
	 * PSRAM DTB untouched for the S-mode U-Boot handoff. */
	s31_uboot_dtb = root.next_arg1;
	fbuf = sbi_malloc(S31_FIXUP_FDT_SIZE);
	if (fbuf && !fdt_open_into((void *)s31_uboot_dtb, fbuf,
				   S31_FIXUP_FDT_SIZE))
		root.next_arg1 = (unsigned long)fbuf;

	/* Core 1 must enter OpenSBI warmboot/HSM before Linux starts it. */
	s31_release_hart1();
	sbi_scsr_write(CSR_STVEC, 0x2f052003);
	sbi_scsr_write(CSR_SIE, 0);
	csr_write(CSR_MEDELEG, 0xfdff);
	s31_apm_init();
	s31_clic_local_init();
	sbi_hart_update_extension(sbi_scratch_thishart_ptr(),
				  SBI_HART_EXT_SDTRIG, false);
	return 0;
}

static int s31_final_init(bool cold_boot)
{
	int ret;

	if (!cold_boot)
		return 0;

	/* Apply generic CPU/domain fixups only to the private FDT copy. */
	ret = generic_final_init(true);
	if (ret)
		return ret;
	ret = s31_services_register();
	if (ret)
		return ret;

	s31_tee_supervisor_priv_sel_to_m();
	/* sbi_domain_startup() subsequently reloads this from root.next_arg1, so
	 * restore both the domain and this hart's handoff pointer. */
	root.next_arg1 = s31_uboot_dtb;
	sbi_scratch_thishart_ptr()->next_arg1 = s31_uboot_dtb;
	csr_set(CSR_MSTATUS, MSTATUS_SPIE);
	return 0;
}

static int s31_extensions_init(bool cold_boot)
{
	return generic_extensions_init(cold_boot);
}

static bool s31_cold_boot_allowed(u32 hartid)
{
	return hartid == 0;
}

static bool s31_single_fw_region(void)
{
	/* XIP text and HP-SRAM RW storage are discontiguous. */
	return true;
}

static int s31_noop_init(void)
{
	return 0;
}

static int s31_platform_init(const void *fdt, int nodeoff,
			     const struct fdt_match *match)
{
	generic_platform_ops.single_fw_region = s31_single_fw_region;
	generic_platform_ops.cold_boot_allowed = s31_cold_boot_allowed;
	generic_platform_ops.nascent_init = s31_noop_init;
	generic_platform_ops.early_init = s31_early_init;
	generic_platform_ops.extensions_init = s31_extensions_init;
	generic_platform_ops.final_init = s31_final_init;
	generic_platform_ops.misa_check_extension = s31_misa_extension;
	generic_platform_ops.misa_get_xlen = s31_misa_xlen;
	generic_platform_ops.irqchip_init = s31_noop_init;
	generic_platform_ops.timer_init = s31_timer_init;
	generic_platform_ops.mpxy_init = s31_noop_init;
	generic_platform_ops.vendor_ext_provider = s31_cache_vendor_ext;
	sbi_hart_priv_version_override = SBI_HART_PRIV_VER_1_10;
	platform.hart_count = 2;
	platform.hart_stack_size = SBI_PLATFORM_DEFAULT_HART_STACK_SIZE;
	return 0;
}

static const struct fdt_match s31_match[] = {
	{ .compatible = "espressif,esp32s31" },
	{ /* sentinel */ }
};

const struct fdt_driver esp32s31 = {
	.match_table = s31_match,
	.init = s31_platform_init,
};
