/* SPDX-License-Identifier: BSD-2-Clause */
#include <sbi/sbi_ecall.h>
#include <sbi/sbi_error.h>
#include <sbi/sbi_console.h>
#include <sbi/riscv_asm.h>
#include <sbi/riscv_io.h>
#include <sbi/sbi_ipi.h>
#include <sbi/sbi_scratch.h>
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
#define S31_SBI_COPROC_SUSPEND		3
#define S31_COPROC_STATE_SIZE		256UL
#define S31_SBI_EXT_CLIC		0x09000003UL
#define S31_SBI_CLIC_MINTSTATUS		0
#define S31_SBI_CLIC_MINTTHRESH		1
#define S31_SBI_CLIC_MIP		2
#define S31_SBI_CLIC_MIE		3
#define S31_SBI_CLIC_WFI		4

#define S31_CSR_MINTSTATUS		0xfb1
#define S31_CSR_MINTTHRESH		0x347

/*
 * S31 does not reliably wake an M-mode WFI for an interrupt delegated to
 * S-mode.  Each timer group provides two independent comparators, so reserve
 * timer 1 in TIMERG0 for hart 0 and timer 1 in TIMERG1 for hart 1, then route
 * each source to a private M-level CLIC slot.  The interrupt is individually
 * enabled while mstatus.MIE is clear in the ecall handler: it wakes WFI
 * without being taken as a nested M-mode trap, then the handler clears it
 * before returning to Linux.
 */
#define S31_IDLE_CLIC_SLOT		48U
#define S31_IDLE_CLIC_ATTR_M_LEVEL	0xc0U
#define S31_IDLE_INTMTX_BASE		0x20585000UL
#define S31_IDLE_INTMTX_STRIDE		0x800UL
#define S31_IDLE_INTMTX_PASS_M		(2U << 8)
#define S31_IDLE_TG0_T1_SOURCE		26U
#define S31_IDLE_TIMER_SOURCE_STRIDE	3U
#define S31_IDLE_TIMERG_BASE(g)		(0x20580000UL + (g) * 0x1000UL)
#define S31_IDLE_TIMER_STRIDE		0x24UL
#define S31_IDLE_TIMER_INDEX		1U
#define S31_IDLE_TIMER_CONFIG(g)	(S31_IDLE_TIMERG_BASE(g) + \
			 S31_IDLE_TIMER_INDEX * S31_IDLE_TIMER_STRIDE + 0x00UL)
#define S31_IDLE_TIMER_ALARM_LO(g)	(S31_IDLE_TIMERG_BASE(g) + \
			 S31_IDLE_TIMER_INDEX * S31_IDLE_TIMER_STRIDE + 0x10UL)
#define S31_IDLE_TIMER_ALARM_HI(g)	(S31_IDLE_TIMERG_BASE(g) + \
			 S31_IDLE_TIMER_INDEX * S31_IDLE_TIMER_STRIDE + 0x14UL)
#define S31_IDLE_TIMER_LOAD_LO(g)	(S31_IDLE_TIMERG_BASE(g) + \
			 S31_IDLE_TIMER_INDEX * S31_IDLE_TIMER_STRIDE + 0x18UL)
#define S31_IDLE_TIMER_LOAD_HI(g)	(S31_IDLE_TIMERG_BASE(g) + \
			 S31_IDLE_TIMER_INDEX * S31_IDLE_TIMER_STRIDE + 0x1cUL)
#define S31_IDLE_TIMER_LOAD(g)		(S31_IDLE_TIMERG_BASE(g) + \
			 S31_IDLE_TIMER_INDEX * S31_IDLE_TIMER_STRIDE + 0x20UL)
#define S31_IDLE_TIMER_INT_ENA(g)	(S31_IDLE_TIMERG_BASE(g) + 0x70UL)
#define S31_IDLE_TIMER_INT_CLR(g)	(S31_IDLE_TIMERG_BASE(g) + 0x7cUL)
#define S31_IDLE_TIMER_ALARM_EN		BIT(10)
#define S31_IDLE_TIMER_DIV_RST		BIT(12)
#define S31_IDLE_TIMER_DIVIDER(v)	((v) << 13)
#define S31_IDLE_TIMER_INCREASE		BIT(30)
#define S31_IDLE_TIMER_ENABLE		BIT(31)
#define S31_IDLE_TIMER_DIVIDER_1MHZ	40U
#define S31_IDLE_TIMER_GUARD_TICKS	10000U
#define S31_IDLE_TIMER_CLK_CTRL(g)	(0x20587114UL + (g) * 0x8UL)
#define S31_IDLE_TIMER_APB_CLK_EN	BIT(0)
#define S31_IDLE_TIMER_FORCE_NORST	BIT(2)
#define S31_IDLE_TIMER_T1_CLK_EN	BIT(8)
#define S31_IDLE_TIMER_T1_CLK_SRC_MASK	(3U << 6)

#define S31_LP_SYS_STORE8		0x2070004cUL
#define S31_LP_SYS_STORE2		0x20700034UL
#define S31_LP_SYS_STORE3		0x20700038UL
#define S31_LP_SYS_STORE4		0x2070003cUL
#define S31_LP_SYS_STORE5		0x20700040UL
#define S31_DEEP_SLEEP_MAGIC		0x53314453U
#define S31_LP_SLEEP_CONTROL		0x2e007c00UL
#define S31_LP_SLEEP_MAGIC		0x5331504dU
#define S31_LP_SLEEP_ABI_VERSION	2U
#define S31_LP_SLEEP_F_MEM		BIT(2)
#define S31_LP_SLEEP_F_DEEP_REBOOT	BIT(3)
#define S31_LP_SLEEP_F_GPIO_PULL_UP	BIT(4)
#define S31_LP_SLEEP_F_GPIO_PULL_DOWN	BIT(5)
#define S31_LP_WAKE_TIMER		BIT(0)
#define S31_LP_WAKE_GPIO		BIT(1)
#define S31_LP_SLEEP_ARMED		3U
#define S31_LP_SLEEP_HP_ASLEEP		4U
#define S31_LP_SLEEP_WAKING		5U
#define S31_LP_WAIT_CYCLES		3200000000U
#define S31_PMU_WAKE_CNTL0		0x20704150UL
#define S31_PMU_WAKE_CNTL1		0x20704154UL
#define S31_PMU_WAKE_CNTL2		0x20704158UL
#define S31_PMU_WAKE_CNTL3		0x2070415cUL
#define S31_PMU_WAKE_CNTL4		0x20704160UL
#define S31_PMU_WAKE_CNTL5		0x20704164UL
#define S31_PMU_WAKE_CNTL6		0x20704168UL
#define S31_PMU_WAKE_CNTL7		0x2070416cUL
#define S31_PMU_BASE			0x20704000UL
#define S31_PMU_HP_ACTIVE_REGULATOR0	0x20704034UL
#define S31_PMU_HP_SLEEP_REGULATOR0	0x207040b4UL
#define S31_PMU_HP_ACTIVE_BACKUP	0x20704024UL
#define S31_PMU_HP_MODEM_BACKUP		0x20704064UL
#define S31_PMU_HP_SLEEP_BACKUP		0x207040a4UL
#define S31_PMU_HP_SLEEP_BACKUP_CLOCK0	0x207040a8UL
#define S31_PMU_HP_SLEEP_BACKUP_CLOCK1	0x207040acUL
#define S31_PMU_HP_SLEEP_SYSCLK		0x207040b0UL
#define S31_PMU_HP_SLEEP2ACTIVE_BACKUP_EN BIT(29)
#define S31_PMU_HP_SLEEP2MODEM_BACKUP_EN BIT(29)
#define S31_PMU_HP_ACTIVE2SLEEP_BACKUP_EN BIT(31)
#define S31_PMU_POWER_PD_MEM_CNTL	0x2070413cUL
#define S31_PMU_POWER_PD_MEM_MASK	0x20704140UL
#define S31_PMU_POWER_WAIT_TIMER0	0x20704114UL
#define S31_PMU_POWER_WAIT_TIMER1	0x20704118UL
#define S31_PMU_POWER_WAIT_TIMER2	0x2070411cUL
#define S31_PMU_POWER_DOMAIN_FIRST	0x20704120UL
#define S31_PMU_POWER_DOMAIN_LAST	0x20704144UL
#define S31_PMU_POWER_VDD_SPI		0x20704148UL
#define S31_PMU_POWER_CLK_WAIT		0x2070414cUL
#define S31_PMU_HP_SLEEP_DIG_POWER	0x20704080UL
#define S31_PMU_HP_SLEEP_ICG_FIRST	0x20704084UL
#define S31_PMU_HP_SLEEP_ICG_LAST	0x20704094UL
#define S31_PMU_HP_SLEEP_SYS_CNTL	0x20704098UL
#define S31_PMU_HP_SLEEP_CLK_POWER	0x2070409cUL
#define S31_PMU_HP_SLEEP_BIAS		0x207040a0UL
#define S31_PMU_HP_SLEEP_REGULATOR1	0x207040b8UL
#define S31_PMU_HP_SLEEP_XTAL		0x207040bcUL
#define S31_PMU_LP_SLEEP_REGULATOR0	0x207040d8UL
#define S31_PMU_LP_SLEEP_REGULATOR1	0x207040dcUL
#define S31_PMU_LP_SLEEP_XTAL		0x207040e0UL
#define S31_PMU_LP_SLEEP_DIG_POWER	0x207040e4UL
#define S31_PMU_LP_SLEEP_CLK_POWER	0x207040e8UL
#define S31_PMU_LP_SLEEP_BIAS		0x207040ecUL
#define S31_PMU_LP_ACTIVE_REGULATOR0	0x207040c0UL
#define S31_PMU_LP_ACTIVE_REGULATOR1	0x207040c4UL
#define S31_PMU_LP_ACTIVE_DIG_POWER	0x207040ccUL
#define S31_PMU_LP_ACTIVE_CLK_POWER	0x207040d0UL
#define S31_PMU_IMMEDIATE_FIRST		0x207040f0UL
#define S31_PMU_IMMEDIATE_LAST		0x20704110UL
#define S31_PMU_IMM_SLEEP_SYSCLK	0x207040f8UL
#define S31_PMU_IMM_MODEM_ICG		0x20704104UL
#define S31_PMU_UPDATE_DIG_ICG_SWITCH	BIT(28)
#define S31_PMU_UPDATE_DIG_ICG_MODEM	BIT(31)
#define S31_PMU_FORCE_HP_MEM_PU_NOISO	0xff000000U
#define S31_PMU_HP_MEM012_ON_MASK	0xfffe0000U
#define S31_PMU_INT_RAW			0x2070418cUL
#define S31_PMU_INT_CLR			0x20704198UL
#define S31_PMU_LP_INT_RAW		0x2070419cUL
#define S31_PMU_LP_INT_CLR		0x207041a8UL
#define S31_PMU_LP_CPU_PWR0		0x207041acUL
#define S31_PMU_LP_CPU_PWR1		0x207041b0UL
#define S31_PMU_LP_CPU_PWR2		0x207041b4UL
#define S31_PMU_LP_CPU_WAITI_RDY	BIT(0)
#define S31_PMU_LP_CPU_SLEEP_REQ	BIT(31)
#define S31_PMU_ANA_PERI_PWR_CTRL	0x20704208UL
#define S31_PMU_XPD_PERIF_I2C		BIT(30)
#define S31_PMU_RSTB_PERIF_I2C		BIT(31)
#define S31_PMU_SLEEP_REQ		BIT(31)
#define S31_PMU_SLEEP_REJECT		BIT(30)
#define S31_PMU_REJECT_CAUSE_CLR	BIT(31)
#define S31_PMU_SOC_WAKEUP		BIT(31)
#define S31_PMU_SW_INT			BIT(29)
#define S31_PMU_LP_CORE_WAKEUP		BIT(0)
#define S31_PMU_RTC_TIMER_WAKEUP	BIT(13)
#define S31_PMU_LP_TIMER1_WAKEUP	BIT(18)
#define S31_RTC_TIMER_TAR0_LO		0x20800000UL
#define S31_RTC_TIMER_TAR0_HI		0x20800004UL
#define S31_RTC_TIMER_TAR1_LO		0x20800008UL
#define S31_RTC_TIMER_TAR1_HI		0x2080000cUL
#define S31_RTC_TIMER_UPDATE		0x20800010UL
#define S31_RTC_TIMER_COUNTER0_LO	0x20800014UL
#define S31_RTC_TIMER_COUNTER0_HI	0x20800018UL
#define S31_RTC_TIMER_INT_CLR		0x20800034UL
#define S31_RTC_TIMER_TAR_EN		BIT(31)
#define S31_RTC_TIMER_UPDATE_NOW	BIT(27)
#define S31_RTC_TIMER_SOC_WAKE_CLR	BIT(31)
#define S31_RTC_SLOW_HZ			155386ULL
#define S31_LP_CLKRST_ROOT_CLK_CONF	0x20701000UL
#define S31_LP_CLKRST_FAST_CLK_SEL	(3U << 26)
#define S31_RC_FAST_STABILIZE_CYCLES	16000U
#define S31_HP_CLKRST_SOC_CLK_SEL	0x20587000UL
#define S31_HP_CLKRST_CPU_FREQ_CTRL	0x20587004UL
#define S31_HP_CLKRST_MEM_FREQ_CTRL	0x20587008UL
#define S31_HP_CLKRST_SYS_FREQ_CTRL	0x2058700cUL
#define S31_HP_CLKRST_APB_FREQ_CTRL	0x20587010UL
#define S31_HP_CLKRST_DIV_MASK		0x3fffU
#define S31_HP_CLKRST_SOC_CLK_MASK	0x3U
#define S31_LP_PWR_APPWR_PWR_CFG		0x20804020UL
#define S31_LP_PWR_APPWR_CLK_CFG		0x20804024UL
#define S31_LP_PWR_APPWR_CTRL		0x20804030UL
#define S31_LP_PWR_APPWR_SLEEP_REQ	BIT(0)
/* Retain the four HP memory banks (mode 2), power down the CPU, TOP,
 * connection and HP-alive logic islands (mode 0), and gate every HP clock
 * class while APPWR sleep is asserted. */
#define S31_LP_PWR_APPWR_RET_PWR_CFG	0x0000aa00U
#define S31_LP_PWR_APPWR_RET_CLK_CFG	0x00000000U
#define S31_OPENSBI_RETENTION_BUFFER	0x2e002000UL
#define S31_OPENSBI_RETENTION_SIZE	0x5000U

extern void s31_retention_warmboot(void);

struct s31_pmu_regval {
	u16 offset;
	u32 value;
};

/* Register image produced by ESP-IDF 6.2 immediately before a verified S31
 * timer deep sleep.  The sleep-specific half begins at 0x80; the active and
 * modem halves are equally important because the PMU consumes them while
 * sequencing the wake transition. */
static const struct s31_pmu_regval s31_pmu_deep_profile[] = {
	{ 0x000, 0x00000000 }, { 0x004, 0xffffffff },
	{ 0x008, 0xffffffff }, { 0x00c, 0xffffffff },
	{ 0x010, 0xffffffff }, { 0x014, 0x80000000 },
	{ 0x018, 0x00800000 }, { 0x01c, 0x5dc00000 },
	{ 0x020, 0x02000000 }, { 0x024, 0x010000a0 },
	{ 0x028, 0xffffffff }, { 0x02c, 0xffffffff },
	{ 0x030, 0x08000000 }, { 0x034, 0xd0047180 },
	{ 0x038, 0x00000000 }, { 0x03c, 0x80000000 },
	{ 0x040, 0x00000000 }, { 0x044, 0x00000000 },
	{ 0x048, 0x00000000 }, { 0x04c, 0x00008100 },
	{ 0x050, 0x00000000 }, { 0x054, 0x40000000 },
	{ 0x058, 0x36800000 }, { 0x05c, 0x19f00000 },
	{ 0x060, 0x00000000 }, { 0x064, 0x00100010 },
	{ 0x068, 0xffffffff }, { 0x06c, 0xffffffff },
	{ 0x070, 0x38000000 }, { 0x074, 0xd0040000 },
	{ 0x078, 0x00000000 }, { 0x07c, 0x80000000 },
	{ 0x080, 0xf8300000 }, { 0x084, 0x00000000 },
	{ 0x088, 0x00000000 }, { 0x08c, 0x00000000 },
	{ 0x090, 0x00000000 }, { 0x094, 0x00000000 },
	{ 0x098, 0x37000000 }, { 0x09c, 0x00300000 },
	{ 0x0a0, 0xc0000000 }, { 0x0a4, 0x21100200 },
	{ 0x0a8, 0xffffffff }, { 0x0ac, 0xffffffff },
	{ 0x0b0, 0x30000000 }, { 0x0b4, 0x00000000 },
	{ 0x0b8, 0x00000000 }, { 0x0bc, 0x00000000 },
	{ 0x0c0, 0xd0400000 }, { 0x0c4, 0x00000000 },
	{ 0x0cc, 0x00000000 }, { 0x0d0, 0x40000000 },
	{ 0x0d8, 0xb8400000 }, { 0x0dc, 0x00000000 },
	{ 0x0e0, 0x00000000 }, { 0x0e4, 0x80000000 },
	{ 0x0e8, 0x00000000 }, { 0x0ec, 0xf0000000 },
	{ 0x114, 0x11089fe0 }, { 0x118, 0x11227e00 },
	{ 0x11c, 0x11111111 }, { 0x120, 0x00000000 },
	{ 0x124, 0x00000000 }, { 0x128, 0x00000000 },
	{ 0x12c, 0x00000000 }, { 0x130, 0x00000000 },
	{ 0x134, 0x00000000 }, { 0x138, 0x00000000 },
	{ 0x13c, 0x00000000 }, { 0x140, 0x00000000 },
	{ 0x144, 0x00000000 }, { 0x148, 0x63fc0000 },
	{ 0x14c, 0x036510fb },
	{ 0x15c, 0x00024747 },
	{ 0x164, 0x18000982 }, { 0x168, 0x00000080 },
	{ 0x16c, 0x0a760000 }, { 0x178, 0x00000032 },
	{ 0x17c, 0x00000a0a }, { 0x184, 0x00000000 },
	{ 0x188, 0x80000000 }, { 0x194, 0x00000000 },
	{ 0x1a4, 0x00000000 },
	{ 0x1bc, 0x00000000 }, { 0x1dc, 0x028804b0 },
	{ 0x1e0, 0x00000008 }, { 0x1e4, 0x00000028 },
	{ 0x1e8, 0x00000000 },
	{ 0x200, 0x000003ff }, { 0x208, 0xc0000000 },
};

static void s31_pmu_apply_deep_profile(void)
{
	unsigned int i;

	for (i = 0; i < sizeof(s31_pmu_deep_profile) /
			 sizeof(s31_pmu_deep_profile[0]); i++)
		writel_relaxed(s31_pmu_deep_profile[i].value,
			       (void *)(S31_PMU_BASE +
					s31_pmu_deep_profile[i].offset));
}

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
	/*
	 * A hart which is already in WFI does not observe the PMU software-stall
	 * request until it wakes.  Flash writes from JFFS2 commonly arrive while
	 * the other CPU is idle, so kick its M-mode IPI source after arming the
	 * stall.  The pending stall then catches the peer before any XIP mapping is
	 * changed by the ROM operation.
	 */
	sbi_ipi_raw_send(sbi_hartid_to_hartindex(peer), false);
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
		/* SAVE is a snapshot operation used by fork and signal setup. */
		s31_coproc_restore((const void *)regs->a0);
		return SBI_SUCCESS;
	case S31_SBI_COPROC_RESTORE:
		s31_coproc_restore((const void *)regs->a0);
		return SBI_SUCCESS;
	case S31_SBI_COPROC_SUSPEND:
		s31_coproc_save((void *)regs->a0);
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

static void s31_idle_timer_arm(unsigned int group)
{
	volatile u8 *clic = (u8 *)S31_CLIC_WORD(S31_IDLE_CLIC_SLOT);
	unsigned long hartid = current_hartid();
	unsigned long matrix;
	u32 source, value;

	/* Keep the reserved timer on the 40 MHz XTAL source at a fixed 1 MHz. */
	value = readl((void *)S31_IDLE_TIMER_CLK_CTRL(group));
	value &= ~S31_IDLE_TIMER_T1_CLK_SRC_MASK;
	value |= S31_IDLE_TIMER_APB_CLK_EN | S31_IDLE_TIMER_FORCE_NORST |
		 S31_IDLE_TIMER_T1_CLK_EN;
	writel(value, (void *)S31_IDLE_TIMER_CLK_CTRL(group));

	/* Linux resets the hart0 INTMTX routes during probe, so restore this
	 * private M-mode route on every entry rather than relying on boot state. */
	source = S31_IDLE_TG0_T1_SOURCE + group * S31_IDLE_TIMER_SOURCE_STRIDE;
	matrix = S31_IDLE_INTMTX_BASE + hartid * S31_IDLE_INTMTX_STRIDE;
	writel(S31_IDLE_CLIC_SLOT | S31_IDLE_INTMTX_PASS_M,
	       (void *)(matrix + source * sizeof(u32)));

	clic[1] = 0;
	clic[0] = 0;
	clic[2] = S31_IDLE_CLIC_ATTR_M_LEVEL;
	clic[3] = S31_CLIC_SINGLE_LEVEL;
	clic[1] = 1;

	writel(0, (void *)S31_IDLE_TIMER_CONFIG(group));
	writel(BIT(S31_IDLE_TIMER_INDEX),
	       (void *)S31_IDLE_TIMER_INT_CLR(group));
	writel(0, (void *)S31_IDLE_TIMER_LOAD_LO(group));
	writel(0, (void *)S31_IDLE_TIMER_LOAD_HI(group));
	writel(1, (void *)S31_IDLE_TIMER_LOAD(group));
	writel(S31_IDLE_TIMER_GUARD_TICKS,
	       (void *)S31_IDLE_TIMER_ALARM_LO(group));
	writel(0, (void *)S31_IDLE_TIMER_ALARM_HI(group));
	writel(readl((void *)S31_IDLE_TIMER_INT_ENA(group)) |
	       BIT(S31_IDLE_TIMER_INDEX),
	       (void *)S31_IDLE_TIMER_INT_ENA(group));
	writel(S31_IDLE_TIMER_ALARM_EN | S31_IDLE_TIMER_DIV_RST |
	       S31_IDLE_TIMER_DIVIDER(S31_IDLE_TIMER_DIVIDER_1MHZ) |
	       S31_IDLE_TIMER_INCREASE | S31_IDLE_TIMER_ENABLE,
	       (void *)S31_IDLE_TIMER_CONFIG(group));
	RISCV_FENCE(iorw, iorw);
}

static void s31_idle_timer_disarm(unsigned int group)
{
	volatile u8 *clic = (u8 *)S31_CLIC_WORD(S31_IDLE_CLIC_SLOT);

	writel(0, (void *)S31_IDLE_TIMER_CONFIG(group));
	writel(BIT(S31_IDLE_TIMER_INDEX),
	       (void *)S31_IDLE_TIMER_INT_CLR(group));
	clic[0] = 0;
	RISCV_FENCE(iorw, iorw);
}

/*
 * CLIC status CSRs are implemented in M-mode on ESP32-S31, but S-mode CSR
 * accesses trap or hang on current silicon.  Keep the privileged reads in
 * OpenSBI so Linux can diagnose each hart without weakening delegation.
 */
static int s31_clic_ecall(unsigned long extid, unsigned long funcid,
			  struct sbi_trap_regs *regs,
			  struct sbi_ecall_return *out)
{
	switch (funcid) {
	case S31_SBI_CLIC_MINTSTATUS:
		out->value = csr_read(S31_CSR_MINTSTATUS);
		break;
	case S31_SBI_CLIC_MINTTHRESH:
		out->value = csr_read(S31_CSR_MINTTHRESH);
		break;
	case S31_SBI_CLIC_MIP:
		out->value = csr_read(CSR_MIP);
		break;
	case S31_SBI_CLIC_MIE:
		out->value = csr_read(CSR_MIE);
		break;
	case S31_SBI_CLIC_WFI:
		/*
		 * A private M-level timer-group comparator bounds each hart's WFI. The
		 * ecall trap cleared mstatus.MIE, so the comparator wakes WFI without
		 * nesting an unknown external interrupt in OpenSBI.  Pending S-mode
		 * work is taken normally after mret.
		 */
		if (current_hartid() > 1)
			return SBI_ERR_INVALID_PARAM;
		s31_idle_timer_arm(current_hartid());
		__asm__ __volatile__("wfi" ::: "memory");
		s31_idle_timer_disarm(current_hartid());
		out->value = csr_read(S31_CSR_MINTSTATUS);
		break;
	default:
		return SBI_ERR_NOT_SUPPORTED;
	}

	return SBI_SUCCESS;
}

static struct sbi_ecall_extension s31_clic_ecall_ext = {
	.name = "s31clic",
	.extid_start = S31_SBI_EXT_CLIC,
	.extid_end = S31_SBI_EXT_CLIC,
	.handle = s31_clic_ecall,
};

/* The generic OpenSBI SUSP core stops secondary harts and saves the
 * non-retentive warmboot state before entering this callback. */
static int s31_system_suspend_check(u32 sleep_type)
{
	return sleep_type == SBI_SUSP_SLEEP_TYPE_SUSPEND ?
		SBI_SUCCESS : SBI_ERR_NOT_SUPPORTED;
}

static int s31_system_suspend(u32 sleep_type,
			      unsigned long mmode_resume_addr)
{
	volatile u32 *control = (volatile u32 *)S31_LP_SLEEP_CONTROL;
	struct sbi_scratch *scratch = sbi_scratch_thishart_ptr();
	void *runtime_rw = (void *)(scratch->fw_start + scratch->fw_rw_offset);
	u32 saved_appwr_pwr;
	u32 saved_appwr_clk;
	u32 start;

	if (control[0] != S31_LP_SLEEP_MAGIC ||
	    control[1] != S31_LP_SLEEP_ABI_VERSION ||
	    control[2] < 28U * sizeof(u32) ||
	    !(control[4] & S31_LP_SLEEP_F_MEM) ||
	    (control[4] & ~(S31_LP_SLEEP_F_MEM |
			   S31_LP_SLEEP_F_GPIO_PULL_UP |
			   S31_LP_SLEEP_F_GPIO_PULL_DOWN)) ||
	    ((control[4] & S31_LP_SLEEP_F_GPIO_PULL_UP) &&
	     (control[4] & S31_LP_SLEEP_F_GPIO_PULL_DOWN)) ||
	    ((control[4] & (S31_LP_SLEEP_F_GPIO_PULL_UP |
			    S31_LP_SLEEP_F_GPIO_PULL_DOWN)) &&
	     !(control[5] & S31_LP_WAKE_GPIO)) ||
	    !(control[5] & S31_LP_WAKE_TIMER) ||
	    (control[5] & ~(S31_LP_WAKE_TIMER | S31_LP_WAKE_GPIO)) ||
	    ((control[5] & S31_LP_WAKE_GPIO) &&
	     (!control[8] || (control[8] & ~0xffU) || control[9] ||
	      control[11] || (control[10] & ~control[8]))) ||
	    control[12] != ~0U || control[13] != ~0U ||
	    control[14] != ~0U || control[19] != S31_LP_SLEEP_ARMED) {
		sbi_printf("S31 SUSP: invalid LP descriptor state=%x result=%x reason=%x raw=%x deadline=%08x:%08x sleep=%08x:%08x wake=%08x:%08x\n",
			   control[19], control[20], control[21], control[22],
			   control[7], control[6],
			   control[24], control[23],
			   control[26], control[25]);
		return SBI_ERR_INVALID_PARAM;
	}
	/* LP_SYS_STORE8 encodes the light-sleep wake stub in bits 31:2 and
	 * bit0 selects deep sleep.  Keep bit0 clear so ROM jumps directly to
	 * OpenSBI's retained warmboot entry instead of reloading firmware. */
	if ((unsigned long)s31_retention_warmboot & 3U) {
		sbi_printf("S31 SUSP: unaligned warmboot address %lx\n",
			   (unsigned long)s31_retention_warmboot);
		return SBI_ERR_INVALID_ADDRESS;
	}
	writel_relaxed((unsigned long)s31_retention_warmboot,
		       (void *)S31_LP_SYS_STORE8);

	/* The LP core may have asserted its HP communication trigger while
	 * servicing the prepare/arm RPCs.  Clear that sticky software wake
	 * status before requesting sleep so the timer ISR can create a fresh
	 * LP-to-HP wake edge.  ESP-IDF performs the same clear immediately
	 * before pmu_ll_hp_set_sleep_enable(). */
	writel_relaxed(S31_PMU_SOC_WAKEUP | S31_PMU_SLEEP_REJECT |
		       S31_PMU_SW_INT,
		       (void *)S31_PMU_INT_CLR);
	writel_relaxed(S31_PMU_SLEEP_REJECT, (void *)S31_PMU_WAKE_CNTL4);
	/* Keep OpenSBI's HP-SRAM RW/scratch region alive.  The IDF default sleep
	 * profile powers the memory and logic retention rails down; start from
	 * the proven active regulator settings, then tune retention bias only
	 * after the full warm-resume path is stable. */
	writel_relaxed(readl_relaxed((void *)S31_PMU_HP_ACTIVE_REGULATOR0),
		       (void *)S31_PMU_HP_SLEEP_REGULATOR0);
	writel_relaxed(readl_relaxed((void *)S31_PMU_POWER_PD_MEM_CNTL) |
		       S31_PMU_FORCE_HP_MEM_PU_NOISO,
		       (void *)S31_PMU_POWER_PD_MEM_CNTL);
	writel_relaxed(S31_PMU_HP_MEM012_ON_MASK,
		       (void *)S31_PMU_POWER_PD_MEM_MASK);
	/* Match ESP-IDF's light-sleep transition setup.  Without these backup
	 * enables the PMU reaches HP_SLEEP and receives the RTC wake event, but
	 * does not restore the active clock/control image before releasing the
	 * stalled HP CPUs. */
	writel_relaxed(readl_relaxed((void *)S31_PMU_HP_ACTIVE_BACKUP) |
		       S31_PMU_HP_SLEEP2ACTIVE_BACKUP_EN,
		       (void *)S31_PMU_HP_ACTIVE_BACKUP);
	writel_relaxed(readl_relaxed((void *)S31_PMU_HP_MODEM_BACKUP) |
		       S31_PMU_HP_SLEEP2MODEM_BACKUP_EN,
		       (void *)S31_PMU_HP_MODEM_BACKUP);
	writel_relaxed(readl_relaxed((void *)S31_PMU_HP_SLEEP_BACKUP) |
		       S31_PMU_HP_ACTIVE2SLEEP_BACKUP_EN,
		       (void *)S31_PMU_HP_SLEEP_BACKUP);
	writel_relaxed(0, (void *)S31_PMU_WAKE_CNTL1);
	/*
	 * The LP firmware owns duration conversion and arms RTC target 1 for its
	 * ISR.  Mirror that absolute deadline to target 0, which is the PMU's
	 * always-on system timer wake source used by the IDF sleep path.  This
	 * remains effective even if entering HP sleep also stalls the LP core.
	 */
	writel_relaxed(S31_RTC_TIMER_SOC_WAKE_CLR,
		       (void *)S31_RTC_TIMER_INT_CLR);
	writel_relaxed(readl_relaxed((void *)S31_RTC_TIMER_TAR1_LO),
		       (void *)S31_RTC_TIMER_TAR0_LO);
	writel_relaxed((readl_relaxed((void *)S31_RTC_TIMER_TAR1_HI) & 0xffffU) |
		       S31_RTC_TIMER_TAR_EN, (void *)S31_RTC_TIMER_TAR0_HI);
	writel_relaxed(S31_PMU_RTC_TIMER_WAKEUP,
		       (void *)S31_PMU_WAKE_CNTL2);
	RISCV_FENCE(iorw, iorw);
	/* HSM state, system_suspended and scratch resume metadata were updated by
	 * the generic SBI layer immediately before this callback.  S31's D-cache
	 * is not snooped by the HP-SRAM backing store, and the cache contents are
	 * lost across the PMU CPU reset, so commit all of it before sleep. */
	s31_dcache_writeback_all();
	RISCV_FENCE(rw, rw);
	sbi_memcpy((void *)S31_OPENSBI_RETENTION_BUFFER, runtime_rw,
		   S31_OPENSBI_RETENTION_SIZE);
	RISCV_FENCE(rw, rw);
	/* Enter the LP-owned retention path independently of the PMU regulator
	 * transition.  Hardware validation established that APPWR mode 0 gates all
	 * HP clocks and powers down the CPU, TOP, connection and HP-alive logic
	 * islands, while mode 2 retains the four HP memory banks.  Save the boot
	 * profile so non-suspend users continue to observe the firmware defaults.
	 * The LP timer ISR pairs this request with APPWR_WAKEUP_REQ. */
	saved_appwr_pwr = readl_relaxed((void *)S31_LP_PWR_APPWR_PWR_CFG);
	saved_appwr_clk = readl_relaxed((void *)S31_LP_PWR_APPWR_CLK_CFG);
	writel_relaxed(S31_LP_PWR_APPWR_RET_PWR_CFG,
		       (void *)S31_LP_PWR_APPWR_PWR_CFG);
	writel_relaxed(S31_LP_PWR_APPWR_RET_CLK_CFG,
		       (void *)S31_LP_PWR_APPWR_CLK_CFG);
	/* LP GPIO polling becomes a real wake source only after this point. */
	control[19] = S31_LP_SLEEP_HP_ASLEEP;
	RISCV_FENCE(rw, rw);
	writel_relaxed(S31_LP_PWR_APPWR_SLEEP_REQ,
		       (void *)S31_LP_PWR_APPWR_CTRL);
	RISCV_FENCE(iorw, iorw);

	start = csr_read(CSR_CYCLE);
	while (!(readl_relaxed((void *)S31_PMU_INT_RAW) &
		 (S31_PMU_SOC_WAKEUP | S31_PMU_SLEEP_REJECT)) &&
	       control[19] != S31_LP_SLEEP_WAKING) {
		if ((u32)(csr_read(CSR_CYCLE) - start) > S31_LP_WAIT_CYCLES) {
			sbi_printf("S31 SUSP: PMU timeout state=%x raw=%x\n",
				   control[19],
				   readl_relaxed((void *)S31_PMU_INT_RAW));
			return SBI_ERR_TIMEOUT;
		}
	}
	writel_relaxed(saved_appwr_pwr, (void *)S31_LP_PWR_APPWR_PWR_CFG);
	writel_relaxed(saved_appwr_clk, (void *)S31_LP_PWR_APPWR_CLK_CFG);
	RISCV_FENCE(iorw, iorw);
	if (readl_relaxed((void *)S31_PMU_INT_RAW) & S31_PMU_SLEEP_REJECT) {
		sbi_printf("S31 SUSP: PMU rejected sleep raw=%x\n",
			   readl_relaxed((void *)S31_PMU_INT_RAW));
		return SBI_ERR_FAILED;
	}
	writel_relaxed(S31_PMU_SOC_WAKEUP | S31_PMU_SLEEP_REJECT,
		       (void *)S31_PMU_INT_CLR);

	(void)sleep_type;
	(void)mmode_resume_addr;
	/*
	 * A real non-retentive wake resets supervisor translation state before
	 * entering the physical warmboot vector.  This validation backend returns
	 * without resetting the hart, so emulate that architectural reset state.
	 */
	csr_write(CSR_SATP, 0);
	__asm__ __volatile__("sfence.vma" ::: "memory");
	return SBI_SUCCESS;
}

static void s31_system_resume(void)
{
	writel_relaxed(0, (void *)S31_LP_SYS_STORE8);
}

static struct sbi_system_suspend_device s31_suspend = {
	.name = "esp32s31-retention",
	.system_suspend_check = s31_system_suspend_check,
	.system_suspend = s31_system_suspend,
	.system_resume = s31_system_resume,
};

void s31_system_suspend_register(void)
{
	sbi_system_suspend_set_device(&s31_suspend);
}

static void __attribute__((section(".data.s31_flash_text"), noinline, noreturn))
s31_reboot(void)
{
	s31_rom_software_reset_system_t reset_system =
		(s31_rom_software_reset_system_t)S31_ROM_SOFTWARE_RESET_SYSTEM;

	__asm__ __volatile__("fence rw, rw" ::: "memory");
	reset_system();
	for (;;)
		__asm__ __volatile__("wfi");
}

/* The PMU may gate the flash/cache path as soon as sleep_req is sampled.
 * Keep every instruction after that write in HP SRAM, just like IDF's
 * IRAM_ATTR pmu_sleep_start(). */
static void __attribute__((section(".data.s31_flash_text"), noinline, noreturn))
s31_shutdown_failed(bool timed)
{
	if (timed)
		s31_reboot();
	/* A failed power-off must not turn into an unsolicited reboot. */
	csr_clear(CSR_MSTATUS, MSTATUS_MIE);
	for (;;)
		__asm__ __volatile__("wfi");
}

static void __attribute__((section(".data.s31_flash_text"), noinline, noreturn))
s31_deep_enter_sram(volatile u32 *control, bool timed)
{
	u32 start;

	writel_relaxed(S31_PMU_SOC_WAKEUP | S31_PMU_SLEEP_REJECT,
		       (void *)S31_PMU_INT_CLR);
	writel_relaxed(S31_PMU_REJECT_CAUSE_CLR,
		       (void *)S31_PMU_WAKE_CNTL4);
	RISCV_FENCE(iorw, iorw);
	writel_relaxed(readl_relaxed((void *)S31_PMU_WAKE_CNTL0) |
		       S31_PMU_SLEEP_REQ, (void *)S31_PMU_WAKE_CNTL0);
	RISCV_FENCE(iorw, iorw);

	start = csr_read(CSR_CYCLE);
	while ((u32)(csr_read(CSR_CYCLE) - start) < S31_LP_WAIT_CYCLES) {
		if (timed && control[19] == S31_LP_SLEEP_WAKING)
			s31_reboot();
		__asm__ __volatile__("nop");
	}
	s31_shutdown_failed(timed);
}

static bool s31_deep_sleep_armed(void)
{
	volatile u32 *control = (volatile u32 *)S31_LP_SLEEP_CONTROL;

	if (readl_relaxed((void *)S31_LP_SYS_STORE4) ==
	    S31_DEEP_SLEEP_MAGIC)
		return true;

	return control[0] == S31_LP_SLEEP_MAGIC &&
	       control[1] == S31_LP_SLEEP_ABI_VERSION &&
	       control[2] >= 28U * sizeof(u32) &&
	       control[4] & S31_LP_SLEEP_F_DEEP_REBOOT &&
	       !(control[4] & S31_LP_SLEEP_F_MEM) &&
	       control[5] & S31_LP_WAKE_TIMER &&
	       !(control[5] & ~(S31_LP_WAKE_TIMER | S31_LP_WAKE_GPIO)) &&
	       control[19] == S31_LP_SLEEP_ARMED;
}

static void __noreturn s31_deep_sleep(bool timed)
{
	volatile u32 *control = (volatile u32 *)S31_LP_SLEEP_CONTROL;
	u32 duration_ms = readl_relaxed((void *)S31_LP_SYS_STORE5);
	u32 retained_wake_stub = readl_relaxed((void *)S31_LP_SYS_STORE8);
	u32 now_lo, now_hi, check_hi;
	u64 deadline;
	u32 start;
	uintptr_t reg;

	/* Bit 0 selects ROM deep-sleep cold boot.  A zero wake-stub address asks
	 * ROM to run the normal verified boot path instead of retained HP code. */
	writel_relaxed(0, (void *)S31_LP_SYS_STORE4);
	writel_relaxed(0, (void *)S31_LP_SYS_STORE5);
	writel_relaxed(S31_DEEP_SLEEP_MAGIC, (void *)S31_LP_SYS_STORE2);
	writel_relaxed(timed ? S31_LP_WAKE_TIMER : 0, (void *)S31_LP_SYS_STORE3);
	/* ROM samples LP_SYS_STORE8[0] to distinguish a deep-sleep cold boot
	 * from a light-sleep resume.  Clear every retained wake-stub address bit:
	 * Linux uses a different HP-retention warmboot vector for s2idle, and
	 * carrying that address into a non-retentive deep sleep makes ROM jump to
	 * powered-down memory instead of taking the verified cold-boot path. */
	writel_relaxed(BIT(0), (void *)S31_LP_SYS_STORE8);
	writel_relaxed(S31_PMU_SOC_WAKEUP | S31_PMU_SLEEP_REJECT |
		       S31_PMU_SW_INT, (void *)S31_PMU_INT_CLR);
	/* Target 1 belongs to the LP firmware, which Linux shutdown may restart.
	 * Arm target 0 from a duration latched in LP_SYS instead of copying that
	 * volatile target.  RTC_SLOW was measured by the resident LP firmware at
	 * 155386 Hz on this clock profile; frequency error only shifts the delay. */
	if (duration_ms < 1000U || duration_ms > 600000U)
		duration_ms = 10000U;
	writel_relaxed(readl_relaxed((void *)S31_RTC_TIMER_UPDATE) |
		       S31_RTC_TIMER_UPDATE_NOW,
		       (void *)S31_RTC_TIMER_UPDATE);
	do {
		now_hi = readl_relaxed((void *)S31_RTC_TIMER_COUNTER0_HI) & 0xffffU;
		now_lo = readl_relaxed((void *)S31_RTC_TIMER_COUNTER0_LO);
		check_hi = readl_relaxed((void *)S31_RTC_TIMER_COUNTER0_HI) & 0xffffU;
	} while (now_hi != check_hi);
	deadline = ((u64)now_hi << 32) | now_lo;
	deadline += ((u64)duration_ms * S31_RTC_SLOW_HZ) / 1000U;
	writel_relaxed(S31_RTC_TIMER_SOC_WAKE_CLR,
		       (void *)S31_RTC_TIMER_INT_CLR);
	/* Match rtc_timer_ll_set_wakeup_time() exactly: program HI without the
	 * enable strobe, program LO, then arm the target with an HI RMW. */
	writel_relaxed((u32)(deadline >> 32) & 0xffffU,
		       (void *)S31_RTC_TIMER_TAR0_HI);
	writel_relaxed((u32)deadline, (void *)S31_RTC_TIMER_TAR0_LO);
	writel_relaxed(((u32)(deadline >> 32) & 0xffffU) |
		       (timed ? S31_RTC_TIMER_TAR_EN : 0),
		       (void *)S31_RTC_TIMER_TAR0_HI);
	/* Cancel an LP firmware deadline left by a previous suspend. */
	writel_relaxed(0, (void *)S31_RTC_TIMER_TAR1_HI);
	writel_relaxed(0, (void *)S31_PMU_WAKE_CNTL1);
	writel_relaxed(timed ? S31_PMU_RTC_TIMER_WAKEUP : 0,
		       (void *)S31_PMU_WAKE_CNTL2);
	/* Remoteproc's stop callback pulses LP_CPU_SLEEP_REQ, but a pending
	 * mailbox/RTC source can release the LP hart immediately.  Disable its
	 * wake sources and clear sticky PMU interrupts before asking it to enter
	 * WAITI; RTC target 0 independently owns the cold-boot wake deadline. */
	writel_relaxed(0, (void *)S31_PMU_LP_CPU_PWR2);
	writel_relaxed(readl_relaxed((void *)S31_PMU_LP_INT_RAW),
		       (void *)S31_PMU_LP_INT_CLR);
	writel_relaxed(S31_PMU_LP_CPU_SLEEP_REQ,
		       (void *)S31_PMU_LP_CPU_PWR1);
	start = csr_read(CSR_CYCLE);
	while (!(readl_relaxed((void *)S31_PMU_LP_CPU_PWR0) &
		 S31_PMU_LP_CPU_WAITI_RDY)) {
		if ((u32)(csr_read(CSR_CYCLE) - start) >
		    S31_PMU_STALL_TIMEOUT_CYCLES) {
			sbi_printf("S31 deep: LP hart did not enter WAITI pwr0=%08x raw=%08x wake=%08x\n",
				   readl_relaxed((void *)S31_PMU_LP_CPU_PWR0),
				   readl_relaxed((void *)S31_PMU_LP_INT_RAW),
				   readl_relaxed((void *)S31_PMU_LP_CPU_PWR2));
			s31_shutdown_failed(timed);
		}
	}
	/* Remoteproc runs the LP core with reset/stall-on-sleep enabled.  Those
	 * controls are useful for a restartable coprocessor but differ from the
	 * ROM/IDF deep-sleep terminal state and hold the PMU transition open.
	 * Once WAITI is acknowledged, leave only the documented 0xff-cycle stall
	 * wait plus the stall-ready flag selection used by IDF. */
	writel_relaxed(0x1ff00000U, (void *)S31_PMU_LP_CPU_PWR0);
	RISCV_FENCE(iorw, iorw);
	/* pmu_init() begins by resetting, powering and releasing the analog I2C
	 * peripheral used by the PMU regulator/PLL sequencer.  This register lies
	 * beyond the mode-table block and returns to reset across a deep boot. */
	writel_relaxed(readl_relaxed((void *)S31_PMU_ANA_PERI_PWR_CTRL) &
		       ~S31_PMU_RSTB_PERIF_I2C,
		       (void *)S31_PMU_ANA_PERI_PWR_CTRL);
	start = csr_read(CSR_CYCLE);
	while ((u32)(csr_read(CSR_CYCLE) - start) < 320U)
		__asm__ __volatile__("nop");
	writel_relaxed(readl_relaxed((void *)S31_PMU_ANA_PERI_PWR_CTRL) |
		       S31_PMU_XPD_PERIF_I2C | S31_PMU_RSTB_PERIF_I2C,
		       (void *)S31_PMU_ANA_PERI_PWR_CTRL);
	/* Deep sleep transitions through HP_SLEEP and wakes through HP_ACTIVE.
	 * Install the captured IDF state tables, but do not fire the PMU immediate
	 * command ports here: those are boot-time commits rather than part of
	 * IDF's per-sleep sequence. */
	s31_pmu_apply_deep_profile();
	/* Program the same deep-sleep PMU state selected by ESP-IDF's S31
	 * PMU_SLEEP_*_DSLP_CONFIG_DEFAULT.  U-Boot initializes the active and
	 * modem state-machine tables at boot.  Do not replay their live register
	 * image here: IDF's per-sleep path updates only the sleep-mode and timing
	 * fields, and writing command/status registers from a dump can corrupt the
	 * sequencer's internal latches. */
	writel_relaxed(0xf8300000U, (void *)S31_PMU_HP_SLEEP_DIG_POWER);
	for (reg = S31_PMU_HP_SLEEP_ICG_FIRST;
	     reg <= S31_PMU_HP_SLEEP_ICG_LAST; reg += sizeof(u32))
		writel_relaxed(0, (void *)reg);
	writel_relaxed(0x37000000U, (void *)S31_PMU_HP_SLEEP_SYS_CNTL);
	writel_relaxed(0x00300000U, (void *)S31_PMU_HP_SLEEP_CLK_POWER);
	writel_relaxed(0xc0000000U, (void *)S31_PMU_HP_SLEEP_BIAS);
	writel_relaxed(0x21100200U, (void *)S31_PMU_HP_SLEEP_BACKUP);
	writel_relaxed(0xffffffffU, (void *)S31_PMU_HP_SLEEP_BACKUP_CLOCK0);
	writel_relaxed(0xffffffffU, (void *)S31_PMU_HP_SLEEP_BACKUP_CLOCK1);
	writel_relaxed(0x30000000U, (void *)S31_PMU_HP_SLEEP_SYSCLK);
	writel_relaxed(0, (void *)S31_PMU_HP_SLEEP_REGULATOR0);
	writel_relaxed(0, (void *)S31_PMU_HP_SLEEP_REGULATOR1);
	writel_relaxed(0, (void *)S31_PMU_HP_SLEEP_XTAL);
	writel_relaxed(0xd0400000U, (void *)S31_PMU_LP_ACTIVE_REGULATOR0);
	writel_relaxed(0, (void *)S31_PMU_LP_ACTIVE_REGULATOR1);
	writel_relaxed(0, (void *)S31_PMU_LP_ACTIVE_DIG_POWER);
	writel_relaxed(0x40000000U, (void *)S31_PMU_LP_ACTIVE_CLK_POWER);
	writel_relaxed(0xb8400000U, (void *)S31_PMU_LP_SLEEP_REGULATOR0);
	writel_relaxed(0, (void *)S31_PMU_LP_SLEEP_REGULATOR1);
	writel_relaxed(0, (void *)S31_PMU_LP_SLEEP_XTAL);
	/* Match the IDF profile captured on this board.  LP peripherals and
	 * RC_FAST remain powered while the always-on RTC timer owns wakeup. */
	writel_relaxed(0x80000000U, (void *)S31_PMU_LP_SLEEP_DIG_POWER);
	writel_relaxed(0, (void *)S31_PMU_LP_SLEEP_CLK_POWER);
	writel_relaxed(0xf0000000U, (void *)S31_PMU_LP_SLEEP_BIAS);
	/* Complete the power-transition and wakeup timing profile.  These values
	 * are the ESP-IDF S31 defaults for the board's 40 MHz crystal and the ROM
	 * selected RTC slow clock.  Active-mode timing lets HP power down, but is
	 * insufficient to sequence the RTC wake transition back to HP active. */
	writel_relaxed(0x11089fe0U, (void *)S31_PMU_POWER_WAIT_TIMER0);
	writel_relaxed(0x11227e00U, (void *)S31_PMU_POWER_WAIT_TIMER1);
	writel_relaxed(0x11111111U, (void *)S31_PMU_POWER_WAIT_TIMER2);
	for (reg = S31_PMU_POWER_DOMAIN_FIRST;
	     reg <= S31_PMU_POWER_DOMAIN_LAST; reg += sizeof(u32))
		writel_relaxed(0, (void *)reg);
	writel_relaxed(0x63fc0000U, (void *)S31_PMU_POWER_VDD_SPI);
	writel_relaxed(0x036510fbU, (void *)S31_PMU_POWER_CLK_WAIT);
	writel_relaxed(0x00024747U, (void *)S31_PMU_WAKE_CNTL3);
	writel_relaxed(0x18000982U, (void *)S31_PMU_WAKE_CNTL5);
	writel_relaxed(0x00000080U, (void *)S31_PMU_WAKE_CNTL6);
	writel_relaxed(0x0a760000U, (void *)S31_PMU_WAKE_CNTL7);
	/* Wake enable/reject registers are live request state, not part of the
	 * reusable PMU table.  Program them after the table image so the sleep
	 * request starts from the same state as IDF's pmu_sleep_start(). */
	writel_relaxed(0, (void *)S31_PMU_WAKE_CNTL0);
	writel_relaxed(0, (void *)S31_PMU_WAKE_CNTL1);
	writel_relaxed(timed ? S31_PMU_RTC_TIMER_WAKEUP : 0,
		       (void *)S31_PMU_WAKE_CNTL2);
	/* RTC_FAST normally follows XTAL while Linux is running.  Deep sleep
	 * powers XTAL down, so switch the PMU transition clock to the always-on
	 * RC_FAST source before asserting sleep_req.  ROM/SPL restores the normal
	 * runtime clock tree after the deep-sleep cold boot. */
	writel_relaxed(readl_relaxed((void *)S31_LP_CLKRST_ROOT_CLK_CONF) &
		       ~S31_LP_CLKRST_FAST_CLK_SEL,
		       (void *)S31_LP_CLKRST_ROOT_CLK_CONF);
	/* PMU_HP_SLEEP_LP_CK_POWER.xpd_fosc_clk was asserted by the profile.
	 * Give RC_FAST the same 50 us startup allowance as rtc_clk_8m_enable()
	 * before making it the sleep-transition source. */
	start = csr_read(CSR_CYCLE);
	while ((u32)(csr_read(CSR_CYCLE) - start) <
	       S31_RC_FAST_STABILIZE_CYCLES)
		__asm__ __volatile__("nop");
	sbi_printf("S31 deep: ms=%u now=%04x:%08x target=%04x:%08x update=%08x wake2=%08x raw=%08x old_stub=%08x\n",
		   duration_ms, now_hi, now_lo,
		   (u32)(deadline >> 32) & 0xffffU, (u32)deadline,
		   readl_relaxed((void *)S31_RTC_TIMER_UPDATE),
		   readl_relaxed((void *)S31_PMU_WAKE_CNTL2),
		   readl_relaxed((void *)S31_RTC_TIMER_TAR0_HI),
		   retained_wake_stub);
	control[19] = S31_LP_SLEEP_HP_ASLEEP;
	RISCV_FENCE(rw, rw);
	/* Match rtc_clk_cpu_freq_set_xtal_for_sleep(): the PMU deep-sleep
	 * sequencer must be entered with CPU, memory, system and APB clocks all at
	 * the 40 MHz crystal rate.  SOC_CLK_UPDATE needs both HP harts, so Linux
	 * performs this transition from its reboot notifier before stopping CPU1.
	 * Never retry the handshake here after the secondary hart is offline. */
	if ((readl_relaxed((void *)S31_HP_CLKRST_SOC_CLK_SEL) &
	     S31_HP_CLKRST_SOC_CLK_MASK) ||
	    (readl_relaxed((void *)S31_HP_CLKRST_CPU_FREQ_CTRL) &
	     S31_HP_CLKRST_DIV_MASK) ||
	    (readl_relaxed((void *)S31_HP_CLKRST_MEM_FREQ_CTRL) & BIT(0)) ||
	    (readl_relaxed((void *)S31_HP_CLKRST_SYS_FREQ_CTRL) &
	     S31_HP_CLKRST_DIV_MASK) ||
	    (readl_relaxed((void *)S31_HP_CLKRST_APB_FREQ_CTRL) &
	     S31_HP_CLKRST_DIV_MASK)) {
		sbi_printf("S31 shutdown: Linux did not hand off clocks to XTAL\n");
		s31_shutdown_failed(timed);
	}
	/* Enter the PMU state machine from SRAM.  This call never returns: either
	 * the PMU completes deep sleep, or the bounded fail-closed path resets. */
	s31_deep_enter_sram(control, timed);
}

static int s31_system_reset_check(u32 type, u32 reason)
{
	return type == SBI_SRST_RESET_TYPE_SHUTDOWN ||
	       type == SBI_SRST_RESET_TYPE_COLD_REBOOT ||
	       type == SBI_SRST_RESET_TYPE_WARM_REBOOT;
}

static void s31_system_reset(u32 type, u32 reason)
{
	if (type == SBI_SRST_RESET_TYPE_SHUTDOWN)
		s31_deep_sleep(s31_deep_sleep_armed());
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
	ret = sbi_ecall_register_extension(&s31_clic_ecall_ext);
	if (ret)
		return ret;
	sbi_system_reset_add_device(&s31_reset);
	return 0;
}
