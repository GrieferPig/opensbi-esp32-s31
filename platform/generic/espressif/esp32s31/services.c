/* SPDX-License-Identifier: BSD-2-Clause */
#include <sbi/sbi_ecall.h>
#include <sbi/sbi_error.h>
#include <sbi/riscv_asm.h>
#include <sbi/sbi_string.h>
#include <sbi/sbi_system.h>

#include "platform.h"

#define S31_ROM_FLASH_WRITE		0x2f800168UL
#define S31_ROM_FLASH_ERASE		0x2f800174UL
#define S31_ROM_SOFTWARE_RESET_SYSTEM	0x2f800094UL
#define S31_SBI_EXT_FLASH		0x09000000UL
#define S31_SBI_FLASH_WRITE		0
#define S31_SBI_FLASH_ERASE		1
#define S31_SBI_EXT_COPROC		0x09000002UL
#define S31_SBI_COPROC_SWITCH		0
#define S31_SBI_COPROC_SAVE		1
#define S31_SBI_COPROC_RESTORE		2
#define S31_COPROC_STATE_SIZE		256UL

typedef void (*s31_rom_software_reset_system_t)(void);
typedef int (*s31_rom_flash_write_t)(u32 address, const u32 *buffer,
				     s32 length);
typedef int (*s31_rom_flash_erase_t)(u32 address, u32 length);

/*
 * The legacy ROM SPIWrite/SPIEraseArea implementation is not compatible with
 * flash auto-suspend. ESP-IDF deliberately excludes that combination and,
 * when caches are disabled on SMP, parks the other core in internal RAM.
 *
 * Do the equivalent here using the SoC's hart-stall control, then run the
 * synchronous ROM operation from an SRAM-resident wrapper with auto-suspend
 * temporarily disabled. No hart can fetch XIP until the operation is complete,
 * so the ROM implementation cannot return into a suspended/partially-readable
 * flash window.
 */
#define S31_SPI1_FLASH_SUS_CTRL		0x2050109cUL
#define S31_SPI1_AUTO_RESUME_EN		BIT(4)
#define S31_SPI1_AUTO_SUSPEND_EN	BIT(5)
#define S31_HP_CORESTALLED_ST		0x20586064UL
#define S31_PMU_CPU_STALL_SW		0x207041ecUL
#define S31_PMU_STALL_CODE		0x86U
#define S31_PMU_STALL_TIMEOUT_CYCLES	32000000U

static u32 s31_flash_buffer[8];

static inline u32 s31_flash_rdcycle(void)
{
	u32 value;

	__asm__ __volatile__("csrr %0, cycle" : "=r" (value));
	return value;
}

static int __attribute__((section(".data.s31_flash_text"), noinline))
s31_flash_rom_operation(u32 funcid, u32 address, const u32 *buffer, u32 length)
{
	volatile u32 *sus_ctrl = (volatile u32 *)S31_SPI1_FLASH_SUS_CTRL;
	volatile u32 *stall_ctrl = (volatile u32 *)S31_PMU_CPU_STALL_SW;
	volatile u32 *stall_status = (volatile u32 *)S31_HP_CORESTALLED_ST;
	u32 peer = current_hartid() ^ 1U;
	u32 stall_shift = peer ? 16 : 24;
	u32 stall_mask = 0xffU << stall_shift;
	u32 stall_bit = BIT(peer);
	u32 saved_stall_ctrl = *stall_ctrl;
	u32 saved_sus_ctrl = *sus_ctrl;
	u32 start;
	int ret;

	*stall_ctrl = (saved_stall_ctrl & ~stall_mask) |
		(S31_PMU_STALL_CODE << stall_shift);
	__asm__ __volatile__("fence iorw, iorw" ::: "memory");
	start = s31_flash_rdcycle();
	while (!(*stall_status & stall_bit)) {
		if ((u32)(s31_flash_rdcycle() - start) >
		    S31_PMU_STALL_TIMEOUT_CYCLES) {
			*stall_ctrl = saved_stall_ctrl;
			return 2;
		}
	}

	*sus_ctrl = saved_sus_ctrl &
		~(S31_SPI1_AUTO_RESUME_EN | S31_SPI1_AUTO_SUSPEND_EN);
	__asm__ __volatile__("fence iorw, iorw" ::: "memory");
	if (funcid == S31_SBI_FLASH_ERASE)
		ret = ((s31_rom_flash_erase_t)S31_ROM_FLASH_ERASE)(address,
								 length);
	else
		ret = ((s31_rom_flash_write_t)S31_ROM_FLASH_WRITE)(address,
								 buffer, length);
	__asm__ __volatile__("fence iorw, iorw" ::: "memory");
	*sus_ctrl = saved_sus_ctrl;
	__asm__ __volatile__("fence iorw, iorw" ::: "memory");
	*stall_ctrl = saved_stall_ctrl;
	__asm__ __volatile__("fence iorw, iorw" ::: "memory");
	while (*stall_status & stall_bit)
		;
	return ret;
}

static int s31_flash_ecall(unsigned long extid, unsigned long funcid,
			   struct sbi_trap_regs *regs,
			   struct sbi_ecall_return *out)
{
	u32 address = regs->a0;
	u32 length = regs->a2;
	int ret;

	if (!length || address >= S31_FLASH_SIZE ||
	    length > S31_FLASH_SIZE - address)
		return SBI_ERR_INVALID_PARAM;
	switch (funcid) {
	case S31_SBI_FLASH_WRITE:
		if ((address | regs->a1 | length) & 3 || length > 32 ||
		    regs->a1 < S31_PSRAM_LINUX_START ||
		    regs->a1 >= S31_PSRAM_LINUX_END ||
		    length > S31_PSRAM_LINUX_END - regs->a1)
			return SBI_ERR_INVALID_PARAM;
		sbi_memcpy(s31_flash_buffer, (const void *)regs->a1, length);
		break;
	case S31_SBI_FLASH_ERASE:
		if ((address | length) & 0xfff)
			return SBI_ERR_INVALID_PARAM;
		break;
	default:
		return SBI_ERR_NOT_SUPPORTED;
	}
	ret = s31_flash_rom_operation(funcid, address,
			funcid == S31_SBI_FLASH_WRITE ? s31_flash_buffer : NULL,
			length);
	out->value = ret;
	return ret ? SBI_ERR_FAILED : SBI_SUCCESS;
}

static struct sbi_ecall_extension s31_flash_ecall_ext = {
	.name = "s31flash",
	.extid_start = S31_SBI_EXT_FLASH,
	.extid_end = S31_SBI_EXT_FLASH,
	.handle = s31_flash_ecall,
};

extern void s31_coproc_save(void *state);
extern void s31_coproc_restore(const void *state);

static bool s31_coproc_state_valid(unsigned long address)
{
	return !(address & 0xf) && address >= S31_PSRAM_LINUX_START &&
	       address <= S31_PSRAM_LINUX_END - S31_COPROC_STATE_SIZE;
}

static int s31_coproc_ecall(unsigned long extid, unsigned long funcid,
			    struct sbi_trap_regs *regs,
			    struct sbi_ecall_return *out)
{
	if (!s31_coproc_state_valid(regs->a0))
		return SBI_ERR_INVALID_ADDRESS;
	switch (funcid) {
	case S31_SBI_COPROC_SWITCH:
		if (!s31_coproc_state_valid(regs->a1))
			return SBI_ERR_INVALID_ADDRESS;
		s31_coproc_save((void *)regs->a0);
		s31_coproc_restore((const void *)regs->a1);
		return SBI_SUCCESS;
	case S31_SBI_COPROC_SAVE:
		s31_coproc_save((void *)regs->a0);
		return SBI_SUCCESS;
	case S31_SBI_COPROC_RESTORE:
		s31_coproc_restore((const void *)regs->a0);
		return SBI_SUCCESS;
	default:
		return SBI_ERR_NOT_SUPPORTED;
	}
}

static struct sbi_ecall_extension s31_coproc_ecall_ext = {
	.name = "s31cprc",
	.extid_start = S31_SBI_EXT_COPROC,
	.extid_end = S31_SBI_EXT_COPROC,
	.handle = s31_coproc_ecall,
};

static void __noreturn s31_reboot(void)
{
	s31_rom_software_reset_system_t reset_system =
		(s31_rom_software_reset_system_t)S31_ROM_SOFTWARE_RESET_SYSTEM;

	__asm__ __volatile__("fence rw, rw" ::: "memory");
	reset_system();
	for (;;)
		__asm__ __volatile__("wfi");
}

static int s31_system_reset_check(u32 type, u32 reason)
{
	return type == SBI_SRST_RESET_TYPE_COLD_REBOOT ||
	       type == SBI_SRST_RESET_TYPE_WARM_REBOOT;
}

static void s31_system_reset(u32 type, u32 reason)
{
	s31_reboot();
}

static struct sbi_system_reset_device s31_reset = {
	.name = "esp32s31-reset",
	.system_reset_check = s31_system_reset_check,
	.system_reset = s31_system_reset,
};

int s31_services_register(void)
{
	int ret;

	ret = sbi_ecall_register_extension(&s31_flash_ecall_ext);
	if (ret)
		return ret;
	ret = sbi_ecall_register_extension(&s31_coproc_ecall_ext);
	if (ret)
		return ret;
	sbi_system_reset_add_device(&s31_reset);
	return 0;
}
