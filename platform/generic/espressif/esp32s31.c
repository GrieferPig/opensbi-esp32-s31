/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * ESP32-S31 minimal platform overrides for OpenSBI generic platform.
 *
 * CPU: RV32IMAFBCNSUX — S-mode, MMU (Sv32), FPU, CLIC interrupt controller.
 * M-mode OpenSBI leaves external CLIC IRQs disabled; timer is handled by
 * the generic DTS ACLINT/CLINT driver.
 *
 * This module overrides fw_platform_init to install ESP32-S31 platform hooks
 * and overrides misa detection to match real hardware extensions.
 */

#include <platform_override.h>
#include <sbi/riscv_io.h>
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
#include <sbi/sbi_system.h>
#include <sbi/sbi_timer.h>
#include <sbi/sbi_console.h>
#include <sbi/sbi_trap.h>

extern struct sbi_platform platform;
extern unsigned int sbi_hart_priv_version_override;

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
/* Exclusive end of the complete 16-MiB Linux PSRAM memory node. */
#define S31_PSRAM_LINUX_END      0x51000000UL
#define S31_SBI_EXT_FLASH        0x09000000UL
#define S31_SBI_FLASH_WRITE      0
#define S31_SBI_FLASH_ERASE      1

#define S31_SBI_EXT_COPROC       0x09000002UL
#define S31_SBI_COPROC_SWITCH    0
#define S31_SBI_COPROC_SAVE      1
#define S31_SBI_COPROC_RESTORE   2
#define S31_COPROC_STATE_SIZE    256UL
#define S31_ROM_SOFTWARE_RESET_SYSTEM 0x2f800094UL

typedef int (*s31_rom_flash_write_t)(u32 address, const u32 *buffer,
                                     s32 length);
typedef int (*s31_rom_flash_erase_t)(u32 address, u32 length);
typedef int (*s31_rom_flash_unlock_t)(void);
typedef void (*s31_rom_software_reset_system_t)(void);

/*
 * ROM Wi-Fi/PP globals occupy 0x2f07fc3c..0x2f07ffa8, including xphyQueue,
 * pp_task_hdl, s_wifi_queue, and the registered netstack callbacks.  A former
 * fixed staging address at 0x2f07ff00 overwrote those globals on every MTD
 * write.  Keep the ROM flash input in OpenSBI's own internal HP-SRAM .bss;
 * the SBI flash extension is serialized by Linux and runs on the sole hart.
 */
static u32 s31_flash_buffer[8];

static void __noreturn esp32s31_reboot(void)
{
	s31_rom_software_reset_system_t reset_system =
		(s31_rom_software_reset_system_t)S31_ROM_SOFTWARE_RESET_SYSTEM;

	/* This mask-ROM entry is the esp_rom_software_reset_system() backend
	 * exported by ESP-IDF for ESP32-S31.  It does not depend on hart0,
	 * FreeRTOS, or the retired ESP-Hosted SRAM mailbox. */
	__asm__ __volatile__("fence rw, rw" ::: "memory");
	reset_system();
	for (;;)
		__asm__ __volatile__("wfi");
}

static int esp32s31_system_reset_check(u32 type, u32 reason)
{
	(void)reason;

	switch (type) {
	case SBI_SRST_RESET_TYPE_COLD_REBOOT:
	case SBI_SRST_RESET_TYPE_WARM_REBOOT:
		return 1;
	default:
		return 0;
	}
}

static void esp32s31_system_reset(u32 type, u32 reason)
{
	(void)reason;

	esp32s31_reboot();
}

static struct sbi_system_reset_device esp32s31_reset = {
	.name = "esp32s31-reset",
	.system_reset_check = esp32s31_system_reset_check,
	.system_reset = esp32s31_system_reset,
};
/* Simple polling console on the loader-configured UART0.  The DT stdout
 * path uses the UHCI UART which OpenSBI has no driver for, but the plain
 * UART FIFO at 0x2038a000 is already pinned and clocked by the bootloader.
 */
#define S31_UART_BASE            0x2038a000UL
#define S31_UART_STATUS_REG      (S31_UART_BASE + 0x1c)
#define S31_UART_TXFIFO_CNT      GENMASK(23, 16)

static void esp32s31_console_putc(char ch)
{
	while ((readl((void *)S31_UART_STATUS_REG) & S31_UART_TXFIFO_CNT) >= 127)
		;
	writel((u32)(u8)ch, (void *)S31_UART_BASE);
}

static struct sbi_console_device esp32s31_console = {
.name = "esp32s31-uart0",
.console_putc = esp32s31_console_putc,
};

/* Drain the OpenSBI UART0 FIFO and drop the console device before
 * handing off to Linux.  Linux uses the same UART for earlycon/console;
 * leaving the OpenSBI console installed after mret lets later SBI prints
 * race with Linux UART output and has been observed to break the Linux
 * console entirely.
 */
void sbi_platform_console_release(void)
{
	u32 i;

	for (i = 0; i < 1000000; i++) {
		if (!(readl((void *)S31_UART_STATUS_REG) & S31_UART_TXFIFO_CNT))
			break;
	}

	sbi_console_release_device();
}

/* Write back the OpenSBI 64 KiB PSRAM page so the shared secondary hart
 * can observe cold-boot heap/scratch state even if its PMA load path does
 * not hit the shared D-cache.  Matches the bootloader/head.S sequence.
 */
#define S31_CACHE_BASE            0x2c000000UL
#define S31_CACHE_MAP_DCACHE      0x10
#define S31_CACHE_WRITEBACK_ENA   0x4

void sbi_platform_warmboot_sync(void)
{
	volatile void *cache = (volatile void *)S31_CACHE_BASE;

	writel(S31_CACHE_MAP_DCACHE, cache + 0xa0);
	writel(0x50000000, cache + 0xa4);
	writel(0x01000000, cache + 0xa8);
	RISCV_FENCE(ow, ow);
	writel(S31_CACHE_WRITEBACK_ENA, cache + 0x9c);
	while (readl(cache + 0x9c) & 0x10)
		;
	writel(S31_CACHE_WRITEBACK_ENA, cache + 0x9c);
	while (readl(cache + 0x9c) & 0x10)
		;
	RISCV_FENCE(iorw, iorw);
}

void sbi_platform_hsm_start_sync(u32 hartid)
{
	sbi_platform_warmboot_sync();
}

#define S31_MCLICCFG            0x10800000UL
#define S31_CLICCFG_NMBITS_MASK (3U << 5)
#define S31_CLICCFG_NMBITS_1    (1U << 5)
#define S31_CLIC_CTRL_BASE      0x10801000UL
#define S31_CLIC_WORD(id)       (S31_CLIC_CTRL_BASE + (4UL * (id)))
#define S31_CLIC_NUM_SLOTS      128
#define S31_INTMATRIX_BASE      0x20585000UL
#define S31_INTMATRIX_STRIDE    0x800UL
/* IDF interrupt_core[01]_reg.h: the last source mapping is at 0x2a0. */
#define S31_INTMATRIX_LAST_MAP  0x2a0UL
#define S31_CLIC_ATTR_S_EDGE    0x42
#define S31_CLIC_ATTR_M_EDGE    0xc2
#define S31_CLIC_SINGLE_LEVEL   0x3f
#define S31_CSR_MINTTHRESH      0x347
#define S31_CSR_MEXSTATUS       0x7f2
#define S31_CSR_PMACFG0         0xbc0
#define S31_CSR_PMAADDR0        0xbd0
#define S31_CSR_PMACFG7         0xbc7
#define S31_CSR_PMAADDR7        0xbd7

/* PMA state is hart-local on S31.  A software reset can leave the secondary
 * hart without the cached-PSRAM aperture even though the loader configured it
 * on the previous boot; the first M-mode trap then faults while fetching the
 * XIP trap vector.  Re-assert the ESP-IDF PMA7 NAPOT entry on every hart boot. */
static void esp32s31_pma_init(void)
{
        csr_write_num(S31_CSR_PMACFG7, 0);
        csr_write_num(S31_CSR_PMAADDR7, 0);
        csr_write_num(S31_CSR_PMAADDR7, 0x147fffff);
        csr_write_num(S31_CSR_PMACFG7, 0xe000001d);
        RISCV_FENCE(iorw, iorw);
}

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
        clic_tmr[0] = 0;                       /* IP: clear edge latch */
        clic_tmr[1] = 1;                       /* IE: ensure enabled */
        clic_tmr[2] = S31_CLIC_ATTR_S_EDGE;    /* ATTR: S-mode, edge */
        clic_tmr[3] = S31_CLIC_SINGLE_LEVEL;

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

/*
 * Keep a persistent, per-hart record of every M-mode trap.  S31 enters
 * OpenSBI not only for M-mode CLIC interrupts but also for undelegatable
 * U/S ecalls.  Recording both is required to identify which privilege
 * crossing first leaves SINTSTATUS.SIL at the 0xff sentinel.
 * OpenOCD can locate s31_mtrap_debug in fw_jump.elf.  Halt both harts and
 * write back the shared D-cache before reading its physical PSRAM address.
 * Recording happens before OpenSBI dispatch, so an unhandled raw ID such as
 * ID21 and the immediately preceding synchronous crossings are preserved
 * even if the firmware subsequently parks in trap error.
 */
#define S31_MTRAP_DEBUG_MAGIC   0x4d545233U /* "MTR3" */
#define S31_MTRAP_DEBUG_HARTS   2
#define S31_MTRAP_DEBUG_IDS     48
#define S31_MTRAP_DEBUG_EVENTS  16

struct s31_mtrap_event {
        u32 sequence;
        u32 raw_mcause;
        u32 mepc;
        u32 mstatus;
        u32 clic_word;
};

struct s31_mtrap_record {
        u32 magic;
        u32 total;
        u32 head;
        u32 last_id;
        u32 counts[S31_MTRAP_DEBUG_IDS];
        struct s31_mtrap_event events[S31_MTRAP_DEBUG_EVENTS];
};

volatile struct s31_mtrap_record
s31_mtrap_debug[S31_MTRAP_DEBUG_HARTS];

void esp32s31_record_m_interrupt(ulong raw_mcause,
                                 const struct sbi_trap_regs *regs)
{
        u32 hartid = current_hartid();
        u32 id = raw_mcause & 0xfff;
        volatile struct s31_mtrap_record *record;
        volatile struct s31_mtrap_event *event;
        u32 sequence;

        if (hartid >= S31_MTRAP_DEBUG_HARTS)
                return;

        record = &s31_mtrap_debug[hartid];
        sequence = record->total + 1;
        event = &record->events[record->head &
                                (S31_MTRAP_DEBUG_EVENTS - 1)];

        event->raw_mcause = raw_mcause;
        event->mepc = regs->mepc;
        event->mstatus = regs->mstatus;
        event->clic_word = (raw_mcause & MCAUSE_IRQ_MASK) && id < 128 ?
                readl((void *)S31_CLIC_WORD(id)) : 0;
        event->sequence = sequence;

        if (id < S31_MTRAP_DEBUG_IDS)
                record->counts[id]++;
        record->last_id = id;
        record->head++;
        record->magic = S31_MTRAP_DEBUG_MAGIC;
        RISCV_FENCE(w, w);
        record->total = sequence;
}

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

static void esp32s31_clic_local_reset(void)
{
        ulong hartid = current_hartid();
        ulong matrix = S31_INTMATRIX_BASE + hartid * S31_INTMATRIX_STRIDE;
        int i;

        /*
         * The ESP-IDF loader and radio setup can leave interrupt-matrix
         * sources routed to M-mode CLIC slots on the boot hart.  In
         * particular ID21 has hardwired MODE=M and IE=1, so clearing its IP
         * latch alone is insufficient: an asserted level source immediately
         * sets it again.  Disconnect every IDF-defined source first, then
         * clear the complete address-virtualised local CLIC bank.  Linux and
         * the platform code explicitly rebuild only the routes they own.
         */
        for (i = 0; i <= S31_INTMATRIX_LAST_MAP; i += sizeof(u32))
                writel(0, (void *)(matrix + i));

        for (i = 0; i < S31_CLIC_NUM_SLOTS; i++) {
                volatile u8 *slot = (u8 *)S31_CLIC_WORD(i);

                slot[0] = 0; /* IP */
                slot[1] = 0; /* IE */
                slot[2] = 0; /* ATTR */
                slot[3] = 0; /* CTL */
        }
        RISCV_FENCE(iorw, iorw);
}

static void esp32s31_clic_local_init(void)
{
        esp32s31_clic_local_reset();

        /* Linux aggregates timer and IPI into one S-mode slot per hart;
         * keep the controller in the non-nested configuration. */
        writel((readl((void *)S31_MCLICCFG) & ~S31_CLICCFG_NMBITS_MASK) |
               S31_CLICCFG_NMBITS_1, (void *)S31_MCLICCFG);
        csr_write(S31_CSR_MINTTHRESH, S31_CLIC_SINGLE_LEVEL);
        csr_write(0x147, 0); /* sintthresh */

        /* Configure CLIC interrupts for timer and software IPI.
         * CLIC ID 1 = S-mode software interrupt (Linux native IPI/fallback).
         * CLIC ID 3 = machine software interrupt (OpenSBI cross-hart IPI).
         * CLIC ID 7 = machine timer interrupt (via SYSTIMER COMPx routing).
         * MODE must be written explicitly. */
        volatile uint8_t *clic_sipi_ip   = (uint8_t *)S31_CLIC_WORD(1);
        volatile uint8_t *clic_swi = (uint8_t *)S31_CLIC_WORD(3);
        volatile uint8_t *clic_tmr_ip   = (uint8_t *)0x1080101C;
        volatile uint8_t *clic_tmr_attr = (uint8_t *)0x1080101E;
        volatile uint8_t *clic_tmr_ctl  = (uint8_t *)0x1080101F;
        volatile uint8_t *clic_tmr_ie   = (uint8_t *)0x1080101D;

        /* S-mode software interrupt for Linux IPI: edge, S-mode,
         * same single level as all other S interrupts.  Do not leave it
         * as reset default (ATTR=0 means U-mode level) or Linux never
         * receives an SBI IPI and remote wakeups are lost.
         */
        clic_sipi_ip[0] = 0;
        clic_sipi_ip[2] = S31_CLIC_ATTR_S_EDGE;
        /* Linux replaces this with a native S-mode cross-hart IPI.  Keep
         * the boot-time slot at the same non-nested level as every other
         * S interrupt so no path can reintroduce CLIC priority nesting. */
        clic_sipi_ip[3] = S31_CLIC_SINGLE_LEVEL;
        clic_sipi_ip[1] = 1;

        /* Linux owns runtime IPIs through native S-mode doorbells ID40/41.
         * HSM startup uses a PSRAM state poll, so accepting ID3 in M-mode
         * only creates an avoidable cross-privilege CLIC transition. */
        clic_swi[1] = 0;                       /* IE: permanently disabled */
        clic_swi[0] = 0;                       /* IP: discard stale request */
        clic_swi[2] = S31_CLIC_ATTR_M_EDGE;
        clic_swi[3] = S31_CLIC_SINGLE_LEVEL;

        *clic_tmr_ip   = 0;
        *clic_tmr_attr = S31_CLIC_ATTR_S_EDGE;
        *clic_tmr_ctl  = S31_CLIC_SINGLE_LEVEL;
        *clic_tmr_ie   = 1;

}

/* --- Platform init --- */
static int esp32s31_early_init(bool cold_boot)
{
	sbi_console_set_device(&esp32s31_console);
	esp32s31_pma_init();

	/* ESP-IDF radio blobs use the S31 PIE extension in S-mode. */
	csr_write(S31_CSR_MEXSTATUS, 1);

        if (!cold_boot) {
                esp32s31_clic_local_init();
                return 0;
        }
        // struct sbi_scratch *scratch = sbi_scratch_thishart_ptr();

        /* On S31 only the standard S-mode interrupt CSRs need emulation.
         * Keep the placeholder STVEC in OpenSBI's reserved HP-SRAM window. */
        sbi_scsr_write(CSR_STVEC,    0x2f052003); /* CLIC MODE=3 */
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
        csr_write(S31_CSR_MINTTHRESH, 0);
        csr_write(0x147, 0); /* sintthresh */

        esp32s31_clic_local_init();

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
                sbi_memcpy(s31_flash_buffer, (const void *)regs->a1, length);
                ret = ((s31_rom_flash_write_t)S31_ROM_FLASH_WRITE)(
                        address, s31_flash_buffer, length);
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

extern void s31_coproc_save(void *state);
extern void s31_coproc_restore(const void *state);

static bool s31_coproc_state_valid(unsigned long address)
{
	return !(address & 0xf) &&
	       address >= S31_PSRAM_LINUX_START &&
	       address <= S31_PSRAM_LINUX_END - S31_COPROC_STATE_SIZE;
}

static int esp32s31_coproc_ecall(unsigned long extid, unsigned long funcid,
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

static struct sbi_ecall_extension esp32s31_coproc_ecall_ext = {
	.name		= "s31cprc",
	.extid_start	= S31_SBI_EXT_COPROC,
	.extid_end	= S31_SBI_EXT_COPROC,
	.handle		= esp32s31_coproc_ecall,
};

static int esp32s31_extensions_init(bool cold_boot)
{
	return generic_extensions_init(cold_boot);
}

/* The user-visible Linux boot hart must be hart 0; hart 1 is a secondary
 * hart that Linux starts through SBI HSM. */
static bool esp32s31_cold_boot_allowed(u32 hartid)
{
	return hartid == 0;
}

static bool esp32s31_single_fw_region(void)
{
        /* XIP: Flash text + internal HP-SRAM data are physically separate,
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

                writel((readl((void *)S31_MCLICCFG) &
                       ~S31_CLICCFG_NMBITS_MASK) |
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
		/* Register after generic platform setup, before SBI dispatch starts. */
		ret = sbi_ecall_register_extension(&esp32s31_flash_ecall_ext);
		if (ret)
			return ret;
		ret = sbi_ecall_register_extension(&esp32s31_coproc_ecall_ext);
		if (ret)
			return ret;
		sbi_system_reset_add_device(&esp32s31_reset);

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
        generic_platform_ops.cold_boot_allowed = esp32s31_cold_boot_allowed;
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
        platform.hart_count = 2;
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
