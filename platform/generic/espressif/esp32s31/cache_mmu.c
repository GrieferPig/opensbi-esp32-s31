/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Derived from Espressif's ESP32-S31 OpenSBI port:
 * https://github.com/espressif/opensbi/commit/a93bce66b102966b0612c0afb5c9872a4737fe68
 *
 * Original ESP32-S31 port by Shreyash Bubane and Espressif contributors.
 * Modified for dual-hart SMP, direct S-mode timer delivery and XIP OpenSBI.
 */
#include <sbi/sbi_error.h>

#include "platform.h"

typedef int (*s31_rom_cache_range_t)(u32 map, u32 address, u32 size);
typedef int (*s31_rom_cache_all_t)(u32 map);

#define S31_ROM_CACHE_INVALIDATE_ADDR	0x2f8005e8UL
#define S31_ROM_CACHE_WRITEBACK_ADDR	0x2f8005f0UL
#define S31_ROM_CACHE_INVALIDATE_ALL	0x2f8005f8UL
#define S31_ROM_CACHE_WRITEBACK_ALL	0x2f800600UL
#define S31_CACHE_MAP_ICACHE		(BIT(0) | BIT(1))
#define S31_CACHE_MAP_DCACHE		BIT(4)

#define S31_SBI_CACHE_WBACK		0
#define S31_SBI_CACHE_INVAL		1
#define S31_SBI_CACHE_WBACK_INVAL	2
#define S31_SBI_ICACHE_SYNC		3
#define S31_SBI_ICACHE_SYNC_RANGE	4
#define S31_SBI_DCACHE_WBACK_ALL	5

static bool s31_cache_range_valid(u32 address, u32 size)
{
	if (!size)
		return false;
	if (address >= S31_FLASH_XIP_START && size <= S31_FLASH_SIZE &&
	    address - S31_FLASH_XIP_START <= S31_FLASH_SIZE - size)
		return true;
	if (address >= S31_PSRAM_LINUX_START &&
	    size <= S31_PSRAM_LINUX_END - S31_PSRAM_LINUX_START &&
	    address - S31_PSRAM_LINUX_START <=
		S31_PSRAM_LINUX_END - S31_PSRAM_LINUX_START - size)
		return true;
	return false;
}

static void s31_cache_range(unsigned long rom_address, u32 map,
			    u32 address, u32 size)
{
	((s31_rom_cache_range_t)rom_address)(map, address, size);
}

int s31_cache_vendor_ext(long funcid, struct sbi_trap_regs *regs,
			 struct sbi_ecall_return *out)
{
	u32 address = (u32)regs->a0;
	u32 size = (u32)regs->a1;

	switch (funcid) {
	case S31_SBI_CACHE_WBACK:
	case S31_SBI_CACHE_INVAL:
	case S31_SBI_CACHE_WBACK_INVAL:
	case S31_SBI_ICACHE_SYNC_RANGE:
		if (!s31_cache_range_valid(address, size))
			return SBI_ERR_INVALID_PARAM;
		break;
	}

	switch (funcid) {
	case S31_SBI_CACHE_WBACK:
		s31_cache_range(S31_ROM_CACHE_WRITEBACK_ADDR,
				S31_CACHE_MAP_DCACHE, address, size);
		break;
	case S31_SBI_CACHE_INVAL:
		s31_cache_range(S31_ROM_CACHE_INVALIDATE_ADDR,
				S31_CACHE_MAP_DCACHE, address, size);
		break;
	case S31_SBI_CACHE_WBACK_INVAL:
		s31_cache_range(S31_ROM_CACHE_WRITEBACK_ADDR,
				S31_CACHE_MAP_DCACHE, address, size);
		s31_cache_range(S31_ROM_CACHE_INVALIDATE_ADDR,
				S31_CACHE_MAP_DCACHE, address, size);
		break;
	case S31_SBI_ICACHE_SYNC:
		((s31_rom_cache_all_t)S31_ROM_CACHE_WRITEBACK_ALL)(
			S31_CACHE_MAP_DCACHE);
		((s31_rom_cache_all_t)S31_ROM_CACHE_INVALIDATE_ALL)(
			S31_CACHE_MAP_ICACHE);
		break;
	case S31_SBI_ICACHE_SYNC_RANGE:
		if (address >= S31_PSRAM_LINUX_START)
			s31_cache_range(S31_ROM_CACHE_WRITEBACK_ADDR,
					S31_CACHE_MAP_DCACHE, address, size);
		s31_cache_range(S31_ROM_CACHE_INVALIDATE_ADDR,
				S31_CACHE_MAP_ICACHE, address, size);
		break;
	case S31_SBI_DCACHE_WBACK_ALL:
		((s31_rom_cache_all_t)S31_ROM_CACHE_WRITEBACK_ALL)(
			S31_CACHE_MAP_DCACHE);
		break;
	default:
		return SBI_ENOTSUPP;
	}

	out->value = 0;
	return 0;
}
