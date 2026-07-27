/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * ESP32-S31 minimal platform overrides for OpenSBI generic platform.
 *
 * CPU: RV32IMAFBCNSUX — S-mode, MMU (Sv32), FPU, CLIC interrupt controller.
 * M-mode OpenSBI leaves external CLIC IRQs disabled; timer is handled by
 * the generic DTS ACLINT/CLINT driver.
 *
 * This module:
 *   1. Provides a minimal UART console
 *   2. Overrides fw_platform_init to install ESP32-S31 platform hooks
 *   3. Overrides misa detection to match real hardware extensions
 */

#include <platform_override.h>
#include <sbi/riscv_io.h>
#include <sbi/sbi_console.h>
#include <sbi/sbi_ecall.h>
#include <sbi/sbi_ecall_interface.h>
#include <sbi/sbi_platform.h>
#include <sbi/sbi_string.h>
#include <sbi_utils/fdt/fdt_helper.h>
#include <sbi/sbi_hart.h>
#include <sbi/sbi_scratch.h>
#include <sbi/sbi_emulate_csr.h>
#include <sbi/sbi_hart_pmp.h>
#include <sbi/sbi_math.h>
#include <sbi/sbi_timer.h>
#include <sbi/sbi_trap.h>
#include <sbi/sbi_unpriv.h>

extern struct sbi_platform platform;
extern unsigned int sbi_hart_priv_version_override;

/* --- UART console (UART0 at 0x2038A000 per ESP-IDF reg_base.h) --- */
#define UART_BASE         0x2038A000UL
#define UART_FIFO         (UART_BASE + 0x00)
#define UART_STATUS       (UART_BASE + 0x1c)
#define UART_TXFIFO_CNT   0x00FF0000UL
#define UART_CLKDIV       (UART_BASE + 0x14)
#define UART_CONF0        (UART_BASE + 0x20)
#define UART_FIFO_LEN     128

/* Core-local timer window observed on ESP32-S31. */
#define S31_CLINT_BASE          0x10000000UL
#define S31_MTIMECMP_LO         (S31_CLINT_BASE + 0x4000)
#define S31_MTIMECMP_HI         (S31_CLINT_BASE + 0x4004)
#define S31_MTIMECTL            (S31_CLINT_BASE + 0x4010)
#define S31_MTIME_LO            (S31_CLINT_BASE + 0xbff8)
#define S31_MTIME_HI            (S31_CLINT_BASE + 0xbffc)
#define S31_TIMEBASE_HZ         320000000UL

/* Auto-suspend is configured by the ESP-IDF bootloader before OpenSBI runs. */
#define S31_ROM_FLASH_WRITE      0x2f800168UL
#define S31_ROM_FLASH_ERASE      0x2f800174UL
#define S31_ROM_FLASH_UNLOCK     0x2f800170UL
#define S31_FLASH_SIZE           0x01000000UL
#define S31_PSRAM_LINUX_START    0x50000000UL
#define S31_PSRAM_LINUX_END      0x50ef0000UL
/* Not owned by Linux or the bootloader app after the firmware handoff. */
#define S31_DRAM_FLASH_BUFFER    0x2f07ff00UL

#define S31_SBI_EXT_FLASH        0x09000000UL
#define S31_SBI_FLASH_WRITE      0
#define S31_SBI_FLASH_ERASE      1

#define S31_SBI_EXT_HOSTED       0x09000001UL
#define S31_SBI_HOSTED_TX        0
#define S31_SBI_HOSTED_RX_ACK    1
#define S31_SBI_HOSTED_H1_READY  2
#define S31_HOSTED_BASE          0x2f062f80UL
#define S31_HOSTED_H0_RING       (S31_HOSTED_BASE + 64)
#define S31_HOSTED_H1_RING       (S31_HOSTED_BASE + 256)
#define S31_HOSTED_H1_SLOTS      (S31_HOSTED_BASE + 0x8800)
#define S31_HOSTED_SLOT_SIZE     1920UL
#define S31_HOSTED_DATA_SIZE     1912UL
#define S31_HOSTED_SLOT_COUNT    16UL
#define S31_HOSTED_DB_H0         0x2058701cUL
#define S31_ROM_CACHE_WRITEBACK  0x2f8005f0UL
#define S31_ROM_CACHE_WB_INV_ALL 0x2f800604UL
#define S31_CACHE_MAP_L1_DCACHE  (1U << 4)

typedef int (*s31_rom_flash_write_t)(u32 address, const u32 *buffer,
                                     s32 length);
typedef int (*s31_rom_flash_erase_t)(u32 address, u32 length);
typedef int (*s31_rom_flash_unlock_t)(void);
typedef int (*s31_rom_cache_writeback_t)(u32 map, u32 address, u32 size);
typedef int (*s31_rom_cache_all_t)(u32 map);

#define S31_MCLICCFG            0x10800000UL
#define S31_CLICCFG_NMBITS_MASK (3U << 5)
#define S31_CLICCFG_NMBITS_1    (1U << 5)
#define S31_CLIC_CTRL_BASE      0x10801000UL
#define S31_CLIC_WORD(id)       (S31_CLIC_CTRL_BASE + (4UL * (id)))
#define S31_CLIC_ATTR_S_EDGE    0x42
#define S31_CLIC_ATTR_M_EDGE    0xc2
#define S31_CLIC_SINGLE_LEVEL   0x3f
#define S31_CSR_MINTTHRESH      0x347
static void raw_putc(char ch)
{
        while ((readl_relaxed((void *)UART_STATUS) & UART_TXFIFO_CNT) >=
               (UART_FIFO_LEN << 16))
                ;
        writel_relaxed(ch, (void *)UART_FIFO);
}

static struct sbi_console_device esp32s31_console = {
        .name         = "esp32s31_uart",
        .console_putc = raw_putc,
};

static u64 esp32s31_timer_value(void)
{
        u32 lo, hi, tmp;

        do {
                hi = readl_relaxed((void *)S31_MTIME_HI);
                lo = readl_relaxed((void *)S31_MTIME_LO);
                tmp = readl_relaxed((void *)S31_MTIME_HI);
        } while (hi != tmp);

        return ((u64)hi << 32) | lo;
}

static void esp32s31_timer_event_stop(void)
{
        writel_relaxed(0xffffffff, (void *)S31_MTIMECMP_HI);
        writel_relaxed(0xffffffff, (void *)S31_MTIMECMP_LO);
        /* Clear pending edge-triggered ID7 after stopping timer */
        volatile uint8_t *clic_tmr = (uint8_t *)S31_CLIC_WORD(7);
        clic_tmr[0] = 0;
}

static void esp32s31_timer_event_start(u64 next_event)
{
        /* Deliver the hardware compare directly to S-mode as local ID7. */
        volatile uint8_t *clic_tmr = (uint8_t *)S31_CLIC_WORD(7);

        /*
         * Re-assert the machine-side gate while servicing every timer SBI
         * call.  S31 can restore mintthresh to 0x0f on the later privilege
         * return, so this is only M-mode defence in depth; Linux's sanitized
         * non-nested scause token is the persistent SIL=0xff protection.
         */
        csr_write(S31_CSR_MINTTHRESH, S31_CLIC_SINGLE_LEVEL);

        clic_tmr[0] = 0;                       /* IP: clear edge latch */
        clic_tmr[1] = 1;                       /* IE: ensure enabled */
        clic_tmr[2] = S31_CLIC_ATTR_S_EDGE;    /* ATTR: S-mode, edge */
        clic_tmr[3] = S31_CLIC_SINGLE_LEVEL;   /* Same level as S peripherals */

        writel_relaxed(0xffffffff, (void *)S31_MTIMECMP_HI);
        writel_relaxed((u32)next_event, (void *)S31_MTIMECMP_LO);
        writel_relaxed((u32)(next_event >> 32), (void *)S31_MTIMECMP_HI);
}

static struct sbi_timer_device esp32s31_timer = {
        .name = "esp32s31-mtimer",
        .timer_freq = S31_TIMEBASE_HZ,
        .timer_value = esp32s31_timer_value,
        .timer_event_start = esp32s31_timer_event_start,
        .timer_event_stop = esp32s31_timer_event_stop,
};

/* --- misa override: RV32IMAFBCNSUX --- */
static int esp32s31_misa_extension(char ext)
{
        switch (ext) {
        case 'I': case 'M': case 'A': case 'F': case 'C':
        case 'S': case 'U':
                return 1;
        default:
                return 0;
        }
}

static int esp32s31_misa_xlen(void)
{
        return 1;
}

/* --- Platform init --- */
static int esp32s31_early_init(bool cold_boot)
{
        if (!cold_boot)
                return 0;
        /* UART already initialized by fw_platform_init; just setup console */
        sbi_console_set_device(&esp32s31_console);

        // struct sbi_scratch *scratch = sbi_scratch_thishart_ptr();

        /* On S31 only the standard S-mode interrupt CSRs need emulation.
         * Keep the placeholder STVEC in the reserved cached PSRAM window. */
        sbi_scsr_write(CSR_STVEC,    0x50f00003); /* CLIC MODE=3 */
        sbi_scsr_write(CSR_SIE,      0);
        /*
         * Request delegation of U-mode ECALL (bit 8) to Linux while keeping
         * S-mode ECALL (bit 9) in M-mode for SBI.  S31 WARL may reject bit 8;
         * OpenSBI then redirects it and rebuilds scause without M metadata.
         */
        csr_write(CSR_MEDELEG, 0xfdff);

        /* Configure CPU_APM to allow S-mode access to peripherals (including CLIC) */
        #define DR_REG_CPU_APM_BASE 0x20504C00
        #define CPU_APM_REGION0_ADDR_START_REG (DR_REG_CPU_APM_BASE + 0x4)
        #define CPU_APM_REGION0_ADDR_END_REG (DR_REG_CPU_APM_BASE + 0x8)
        #define CPU_APM_REGION0_ATTR_REG (DR_REG_CPU_APM_BASE + 0xc)
        #define CPU_APM_REGION_FILTER_EN_REG (DR_REG_CPU_APM_BASE + 0x0)
        #define CPU_APM_FUNC_CTRL_REG (DR_REG_CPU_APM_BASE + 0xc4)
        
        writel(0, (void *)CPU_APM_REGION0_ADDR_START_REG);
        writel(0xFFFFFFFF, (void *)CPU_APM_REGION0_ADDR_END_REG);
        writel(0x7777, (void *)CPU_APM_REGION0_ATTR_REG);
        writel(1, (void *)CPU_APM_REGION_FILTER_EN_REG);
        writel(0x0F, (void *)CPU_APM_FUNC_CTRL_REG);

        /*
         * S31 has machine-only CLIC inputs whose MODE and IE bits are
         * hardwired (ID21 is observable as MODE=M, IE=1).  A machine
         * interrupt taken while Linux runs in S-mode creates the hardware
         * SIL=0xff sentinel, even when every programmable source uses the
         * same ctl value.  Keep the machine threshold at that single level:
         * all M interrupts are masked, so none can nest across S-mode.
         *
         * ESP-IDF defines INTTHRESH_STANDARD=1 for S31, so the machine
         * threshold is CSR 0x347 and contains the left-aligned threshold
         * byte directly.  The S-mode threshold remains zero so the direct S
         * interrupts at ctl=0x3f are accepted when no S handler is active;
         * their active level then naturally blocks same-level nesting.
         */
        writel((readl((void *)S31_MCLICCFG) & ~S31_CLICCFG_NMBITS_MASK) |
               S31_CLICCFG_NMBITS_1, (void *)S31_MCLICCFG);
        csr_write(S31_CSR_MINTTHRESH, S31_CLIC_SINGLE_LEVEL);
        csr_write(0x147, 0); /* sintthresh */

        /* Configure CLIC M-mode interrupts for timer and software IPI.
         * CLIC ID 3 = machine software interrupt (IPI across harts).
         * CLIC ID 7 = machine timer interrupt (via SYSTIMER COMPx routing).
         * MODE must be written explicitly. */
        volatile uint8_t *clic_swi_attr = (uint8_t *)0x1080100E;
        volatile uint8_t *clic_swi_ctl  = (uint8_t *)0x1080100F;
        volatile uint8_t *clic_swi_ie   = (uint8_t *)0x1080100D;
        volatile uint8_t *clic_tmr_ip   = (uint8_t *)0x1080101C;
        volatile uint8_t *clic_tmr_attr = (uint8_t *)0x1080101E;
        volatile uint8_t *clic_tmr_ctl  = (uint8_t *)0x1080101F;
        volatile uint8_t *clic_tmr_ie   = (uint8_t *)0x1080101D;
        /*
         * Use one effective CLIC level (ctl=0x3f) for every interrupt.
         * Privilege still determines M/S delivery, but the controller no
         * longer exposes priority-based nesting within either mode.
         */
        *clic_swi_attr = S31_CLIC_ATTR_M_EDGE;
        *clic_swi_ctl  = S31_CLIC_SINGLE_LEVEL;
        /* This platform currently exposes one hart and has no IPI device. */
        *clic_swi_ie   = 0;
        /* Timer interrupt (ID 7): direct S-mode edge interrupt. */
        *clic_tmr_ip   = 0;
        *clic_tmr_attr = S31_CLIC_ATTR_S_EDGE;
        *clic_tmr_ctl  = S31_CLIC_SINGLE_LEVEL;
        *clic_tmr_ie   = 1;

        /* CLIC mode: CSR_TSELECT is readable but non-functional; force-disable SDTRIG */
        sbi_hart_update_extension(sbi_scratch_thishart_ptr(), SBI_HART_EXT_SDTRIG, false);
        return 0;
}

static int esp32s31_flash_ecall(unsigned long extid, unsigned long funcid,
                                struct sbi_trap_regs *regs,
                                struct sbi_ecall_return *out)
{
        u32 address = regs->a0;
        u32 length = regs->a2;
        int ret;

        if (!length || address >= S31_FLASH_SIZE ||
            length > S31_FLASH_SIZE - address)
                return SBI_ERR_INVALID_PARAM;

	ret = ((s31_rom_flash_unlock_t)S31_ROM_FLASH_UNLOCK)();
	if (ret) {
		out->value = ret;
                return SBI_ERR_FAILED;
        }

        switch (funcid) {
        case S31_SBI_FLASH_WRITE:
		if ((address | regs->a1 | length) & 3 || length > 32 ||
		    regs->a1 < S31_PSRAM_LINUX_START ||
		    regs->a1 >= S31_PSRAM_LINUX_END ||
		    length > S31_PSRAM_LINUX_END - regs->a1)
			return SBI_ERR_INVALID_PARAM;
                sbi_memcpy((void *)S31_DRAM_FLASH_BUFFER,
                           (const void *)regs->a1, length);
                ret = ((s31_rom_flash_write_t)S31_ROM_FLASH_WRITE)(
                        address, (const u32 *)S31_DRAM_FLASH_BUFFER, length);
                break;
        case S31_SBI_FLASH_ERASE:
                if ((address | length) & 0xfff)
                        return SBI_ERR_INVALID_PARAM;
                ret = ((s31_rom_flash_erase_t)S31_ROM_FLASH_ERASE)(address,
                                                                    length);
                break;
        default:
                return SBI_ERR_NOT_SUPPORTED;
        }

	out->value = ret;
        return ret ? SBI_ERR_FAILED : SBI_SUCCESS;
}

static struct sbi_ecall_extension esp32s31_flash_ecall_ext = {
        .name           = "s31flash",
        .extid_start    = S31_SBI_EXT_FLASH,
        .extid_end      = S31_SBI_EXT_FLASH,
        .handle         = esp32s31_flash_ecall,
};

static int esp32s31_hosted_ecall(unsigned long extid, unsigned long funcid,
				 struct sbi_trap_regs *regs,
				 struct sbi_ecall_return *out)
{
	volatile u32 *ring;
	struct sbi_trap_info trap = { 0 };
	u8 frame[S31_HOSTED_DATA_SIZE];
	u32 producer, consumer, index;
	volatile u8 *slot;
	u32 count;

	switch (funcid) {
	case S31_SBI_HOSTED_TX:
		if (!regs->a1 || regs->a1 > sizeof(frame))
			return SBI_ERR_INVALID_PARAM;

		sbi_load_loop(frame, regs->a0, regs->a1, &trap);
		if (trap.cause)
			return SBI_ERR_INVALID_ADDRESS;

		ring = (volatile u32 *)S31_HOSTED_H1_RING;
		producer = ring[0];
		consumer = ring[16];
		if (producer - consumer >= S31_HOSTED_SLOT_COUNT)
			return SBI_ERR_NO_SHMEM;

		index = producer & (S31_HOSTED_SLOT_COUNT - 1);
		slot = (volatile u8 *)(S31_HOSTED_H1_SLOTS +
				      index * S31_HOSTED_SLOT_SIZE);
		sbi_memcpy((void *)(slot + 8), frame, regs->a1);
		*(volatile u16 *)(slot + 4) = regs->a1;
		slot[6] = 0;
		*(volatile u32 *)slot = producer + 1;
		__asm__ __volatile__("fence rw, rw" ::: "memory");
		ring[0] = producer + 1;
		__asm__ __volatile__("fence rw, rw" ::: "memory");
		__asm__ __volatile__("fence rw, rw" ::: "memory");
		((s31_rom_cache_writeback_t)S31_ROM_CACHE_WRITEBACK)(
			S31_CACHE_MAP_L1_DCACHE,
			(u32)(unsigned long)slot,
			S31_HOSTED_SLOT_SIZE);
		((s31_rom_cache_writeback_t)S31_ROM_CACHE_WRITEBACK)(
			S31_CACHE_MAP_L1_DCACHE,
			(u32)(unsigned long)ring,
			64);
		((s31_rom_cache_all_t)S31_ROM_CACHE_WB_INV_ALL)(
			S31_CACHE_MAP_L1_DCACHE);
		__asm__ __volatile__("fence rw, rw" ::: "memory");
		writel(1, (void *)S31_HOSTED_DB_H0);
		out->value = producer + 1;
		return SBI_SUCCESS;

	case S31_SBI_HOSTED_RX_ACK:
		ring = (volatile u32 *)S31_HOSTED_H0_RING;
		producer = ring[0];
		consumer = ring[16];
		count = regs->a0;
		if (!count || count > producer - consumer)
			return SBI_ERR_INVALID_PARAM;
		ring[16] = consumer + count;
		__asm__ __volatile__("fence rw, rw" ::: "memory");
		return SBI_SUCCESS;

	case S31_SBI_HOSTED_H1_READY:
		*(volatile u32 *)(S31_HOSTED_BASE + 12) |= 2;
		__asm__ __volatile__("fence rw, rw" ::: "memory");
		return SBI_SUCCESS;
	default:
		return SBI_ERR_NOT_SUPPORTED;
	}
}

static struct sbi_ecall_extension esp32s31_hosted_ecall_ext = {
	.name		= "s31host",
	.extid_start	= S31_SBI_EXT_HOSTED,
	.extid_end	= S31_SBI_EXT_HOSTED,
	.handle		= esp32s31_hosted_ecall,
};

static int esp32s31_extensions_init(bool cold_boot)
{
	return generic_extensions_init(cold_boot);
}

static bool esp32s31_single_fw_region(void)
{
        /* XIP: Flash text + cached PSRAM data are physically separate,
         * fw_rw_offset is not a power of 2.  Report single region
         * so sbi_domain_init skips the power-of-2 alignment check. */
        return true;
}

static int esp32s31_noop_init(void)
{
        return 0;
}

static int esp32s31_timer_init(void)
{
        /* Re-assert direct S-mode delivery for the hardware timer. */
        {
                volatile uint8_t *clic_tmr = (uint8_t *)S31_CLIC_WORD(7);

                writel((readl((void *)S31_MCLICCFG) & ~S31_CLICCFG_NMBITS_MASK) |
                       S31_CLICCFG_NMBITS_1, (void *)S31_MCLICCFG);
                clic_tmr[0] = 0;                        /* IP: clear pending */
                clic_tmr[2] = S31_CLIC_ATTR_S_EDGE;    /* ATTR: S-mode, edge */
                clic_tmr[3] = S31_CLIC_SINGLE_LEVEL;
                clic_tmr[1] = 1;                       /* IE: enable */
        }

        /*
         * Pre-configure external CLIC slots (16-47) for S-mode delivery.
         * Linux's CLIC driver writes through the S-mode sclicbase window
         * (0x10A00000), which cannot change MODE for slots still in M-mode.
         * Set MODE=S here from M-mode so Linux can later enable/disable IE.
         */
        {
                const u8 s_level_attr = 0x40; /* MODE=S, TRIG=level, SHV=0 */
                int i;

                for (i = 16; i <= 47; i++) {
                        volatile uint8_t *ext = (uint8_t *)S31_CLIC_WORD(i);

                        ext[0] = 0;                     /* IP: clear */
                        ext[2] = s_level_attr;          /* ATTR: S-mode, level */
                        ext[3] = S31_CLIC_SINGLE_LEVEL; /* CTL: level 1 */
                        ext[1] = 0;                     /* IE: off (Linux enables) */
                }
        }

        writel_relaxed(1, (void *)S31_MTIMECTL);
        esp32s31_timer_event_stop();
        sbi_timer_set_device(&esp32s31_timer);

        return 0;
}

static int esp32s31_final_init(bool cold_boot)
{
	int ret;

	if (cold_boot) {
		/* Best-effort M-side gate in the last platform hook before Linux. */
		csr_write(S31_CSR_MINTTHRESH, S31_CLIC_SINGLE_LEVEL);

		/* Register after generic platform setup, before SBI dispatch starts. */
		ret = sbi_ecall_register_extension(&esp32s31_flash_ecall_ext);
		if (ret)
			return ret;
		ret = sbi_ecall_register_extension(&esp32s31_hosted_ecall_ext);
		if (ret)
			return ret;

                /*
                 * Ensure S-mode interrupts are enabled after mret.
                 * sbi_hart_switch_mode preserves MSTATUS_SPIE, so
                 * setting it here guarantees SIE ← 1 on mret to S-mode.
                 * Without this, Linux enters S-mode with SIE=0 and
                 * timer interrupts (CLIC ID 5) can never be serviced.
                 */
                csr_set(CSR_MSTATUS, MSTATUS_SPIE);
        }

        return 0;
}

static int esp32s31_platform_init(const void *fdt, int nodeoff,
                                  const struct fdt_match *match)
{
        generic_platform_ops.single_fw_region = esp32s31_single_fw_region;
        generic_platform_ops.nascent_init    = esp32s31_noop_init;
        generic_platform_ops.early_init      = esp32s31_early_init;
        generic_platform_ops.extensions_init = esp32s31_extensions_init;
        generic_platform_ops.final_init      = esp32s31_final_init;
        generic_platform_ops.misa_check_extension = esp32s31_misa_extension;
        generic_platform_ops.misa_get_xlen   = esp32s31_misa_xlen;
        generic_platform_ops.irqchip_init    = esp32s31_noop_init;
        generic_platform_ops.timer_init      = esp32s31_timer_init;
        generic_platform_ops.mpxy_init       = esp32s31_noop_init;
        /* Skip trap-based CSR probing on CLIC-only platforms */
        sbi_hart_priv_version_override = SBI_HART_PRIV_VER_1_10;
        platform.hart_count = 1;
        platform.hart_stack_size = SBI_PLATFORM_DEFAULT_HART_STACK_SIZE;
        return 0;
}

static const struct fdt_match esp32s31_match[] = {
        { .compatible = "espressif,esp32s31" },
        { /* sentinel */ }
};

const struct fdt_driver esp32s31 = {
        .match_table = esp32s31_match,
        .init        = esp32s31_platform_init,
};
