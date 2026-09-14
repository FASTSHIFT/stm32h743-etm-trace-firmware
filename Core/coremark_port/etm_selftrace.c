/*
 * etm_selftrace.c -- self-contained ETMv4 trace bring-up and deterministic
 * workload for the DAP-vs-FPGA cross-check. All previous compile-time knobs
 * (ETM_SELFTRACE_SYSTICK, TRACE_BB, TRACE_STALL) are now runtime fields of
 * struct etm_cfg (see etm_selftrace.h) so the UART CLI can reconfigure them
 * without a rebuild.
 *
 * All register addresses and bit names come from etm_regs.h (there is no
 * upstream CMSIS/ST header for the ETM-M7 or the H7 SoC trace path; see that
 * file's header for the rationale and the ARM/ST document references).
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "etm_selftrace.h"
#include "etm_regs.h"

const struct etm_cfg etm_cfg_default = {
    /* BB ON, but the ETM production rate is kept under the 450 Mbit/s TPIU
     * egress by LOWERING sysclk (bigger DIVP1) while keeping TRACECLK pin at
     * 56 MHz (DIVR1 unchanged). BB=1 gives full branch-target addresses (self-
     * locating decode); at reduced CPU clock the branch rate drops below the
     * egress budget so the ETF no longer overflows. See doc 27. */
    .bb      = 1,
    .stall   = 1,   /* lossless backstop when the ETF nears full */
    .systick = 0,   /* no SysTick exceptions => tightest deterministic loop */
    .ts      = 0,   /* no in-stream timestamps (matches historical golden) */
};

/* ------- deterministic workload ------------------------------------------ */
/* A tiny fixed call tree. It is short enough that several full iterations fit
 * inside the 4 KB ETF, so a DAP read of the ETF holds many complete, identical
 * loop passes -> the reference byte pattern is self-verifying (it must repeat).
 * No data-dependent branches, no memory traffic beyond the stack. */
static volatile uint32_t g_sink;
static volatile uint32_t g_seed = 0x1234u;

static uint32_t leaf_add(uint32_t x) { return x + 1u; }
static uint32_t leaf_xor(uint32_t x) { return x ^ 0x55u; }

static uint32_t node(uint32_t x)
{
    x = leaf_add(x);
    x = leaf_xor(x);
    return x;
}

static uint32_t det_iter(uint32_t seed)
{
    uint32_t a = seed;
    for (int i = 0; i < 8; i++) a = node(a);
    return a;
}

/* ------- SysTick control ------------------------------------------------- */

static void systick_off(void)
{
    ETM_REG(SYST_CSR) = 0;          /* disable counter + interrupt */
    ETM_REG(SYST_CVR) = 0;          /* clear current value */
    /* Clear a possibly-already-pending SysTick so a latched tick can't fire
     * one more IRQ after we disable it (seen on the golden dump: a single
     * SysTick_Handler broke one det_iter mid-leaf_add). */
    ETM_REG(SCB_ICSR) = SCB_ICSR_PENDSTCLR;
}

static void systick_on(void)
{
    /* 1 ms tick at 150 MHz sysclk (RVR=150000-1); short enough that several
     * interrupts land inside a multi-ms capture window. */
    ETM_REG(SYST_RVR) = 150000u - 1u;
    ETM_REG(SYST_CVR) = 0;
    ETM_REG(SYST_CSR) = SYST_CSR_RUN;
}

/* ------- ETM/TPIU/CSTF/ETF/GPIO setup (parallel 4-bit) ------------------- */

void etm_selftrace_setup(const struct etm_cfg *cfg)
{
    if (!cfg) cfg = &etm_cfg_default;

    /* 0a) SYSCFG I/O compensation cell. On H7, VERY_HIGH-speed GPIO only
     * reaches its rated slew when the compensation cell is enabled; without it
     * the pads fall back to a slower default drive (measured: PE5 rise 5.5 ns
     * = 62% of the 8.8 ns half-UI @56 MHz). Clock SYSCFG (RCC_APB4ENR.SYSCFGEN,
     * bit1) then set SYSCFG_CCCSR.EN (bit0) and wait for READY (bit8). */
    ETM_REG(RCC_APB4ENR) |= RCC_APB4ENR_SYSCFGEN;
    (void)ETM_REG(RCC_APB4ENR);
    ETM_REG(SYSCFG_CCCSR) |= SYSCFG_CCCSR_EN;
    for (volatile int i = 0; i < 10000; i++)
        if (ETM_REG(SYSCFG_CCCSR) & SYSCFG_CCCSR_READY) break;

    /* 0) GPIOE PE2..PE6 -> AF0, very-high speed, push-pull. */
    ETM_REG(RCC_AHB4ENR) |= RCC_AHB4ENR_GPIOEEN;
    uint32_t moder = ETM_REG(GPIOE_MODER);
    moder &= ~(0x3FFFu << 4);
    moder |= (0x2u << 4) | (0x2u << 6) | (0x2u << 8) | (0x2u << 10) | (0x2u << 12);
    ETM_REG(GPIOE_MODER) = moder;
    ETM_REG(GPIOE_OSPEEDR) |= (0x3u << 4) | (0x3u << 6) | (0x3u << 8) | (0x3u << 10) | (0x3u << 12);
    ETM_REG(GPIOE_OTYPER) &= ~0x7Cu;
    ETM_REG(GPIOE_AFRL) &= ~(0xFFFFFu << 8);

    /* 1) core trace power + 2) DBGMCU trace clocks. */
    ETM_REG(DEMCR) = DEMCR_TRCENA;
    ETM_REG(DBGMCU_CR) |= DBGMCU_CR_TRACE_ALL;

    /* 2b) CoreSight timestamp generator. The TSGEN is the SoC-wide counter the
     * ETM samples into TIMESTAMP packets; it resets DISABLED, so unless we set
     * CNTCR.EN every ETM timestamp is 0 (confirmed on-board). Enable it only
     * when timestamps are requested. Unlike the F429 (which has no TSGEN at
     * all), the H743 counter increments freely once enabled. */
    if (cfg->ts)
        ETM_REG(TSGEN_CNTCR) |= TSGEN_CNTCR_EN;

    /* 3) TPIU: 4-bit parallel + continuous formatter, no test pattern. */
    ETM_REG(TPIU_LAR) = CS_LAR_UNLOCK;
    ETM_REG(TPIU_CURPSIZE) = TPIU_CURPSIZE_4BIT;
    ETM_REG(TPIU_SPPR) = TPIU_SPPR_PARALLEL;
    ETM_REG(TPIU_FFCR) = TPIU_FFCR_CONT;
    ETM_REG(TPIU_BASE + 0x204u) = 0;            /* ITCTRL: leave integration mode off */

    /* 3b) CSTF funnel: enable ETM slave port S0. */
    ETM_REG(CSTF_LAR) = CS_LAR_UNLOCK;
    ETM_REG(CSTF_CTRL) |= CSTF_CTRL_ENS0;

    /* 3c) ETF: hardware-FIFO mode -> TPIU. */
    ETM_REG(ETF_CTL) = 0;                        /* disable to program */
    ETM_REG(ETF_LAR) = CS_LAR_UNLOCK;
    ETM_REG(ETF_MODE) = ETF_MODE_HW_FIFO;
    ETM_REG(ETF_FFCR) = ETF_FFCR_ENFT;
    ETM_REG(ETF_CTL) = ETF_CTL_TRACECAPTEN;

    /* 4) ETM (ETMv4): apply the requested BB / STALL / TS from cfg. */
    ETM_REG(ETM_LAR) = CS_LAR_UNLOCK;
    ETM_REG(ETM_TRCPDCR) = ETM_TRCPDCR_PU;      /* power-up request */
    ETM_REG(ETM_TRCPRGCTLR) = 0;                /* disable to program */
    for (volatile int i = 0; i < 1000; i++)
        if (ETM_REG(ETM_TRCSTATR) & ETM_TRCSTATR_IDLE) break;

    /* TRCCONFIGR: BB | TS. TS makes the ETM insert 48-bit global-timestamp
     * packets (from the SoC timestamp generator) at sync points / around
     * exceptions, giving an EXECUTION-time base for the decoder instead of the
     * FPGA ETF-egress time. See DDI0494D §3.3.4. */
    ETM_REG(ETM_TRCCONFIGR) = (cfg->bb ? ETM_TRCCONFIGR_BB : 0u)
                            | (cfg->ts ? ETM_TRCCONFIGR_TS : 0u);
    ETM_REG(ETM_TRCTRACEIDR) = ETM_TRCTRACEIDR_ID2;
    /* TRCSTALLCTLR: lossless backstop. NB (DDI0494D §3.4.7): LEVEL!=0 may
     * SUPPRESS in-stream timestamps. When ts is requested, drop the stall LEVEL
     * to 0 (keep ISTALL so overflow is still bounded) so timestamp packets are
     * not starved; without ts keep the historical max-invasion setting. */
    ETM_REG(ETM_TRCSTALLCTLR) = cfg->stall
        ? (cfg->ts ? ETM_TRCSTALLCTLR_ISTALL : ETM_TRCSTALLCTLR_LOSSLESS)
        : 0u;
    ETM_REG(ETM_TRCVICTLR) = ETM_TRCVICTLR_TRACE_ALL;
    ETM_REG(ETM_TRCSYNCPR) = ETM_TRCSYNCPR_4K;
    ETM_REG(ETM_TRCPRGCTLR) = ETM_TRCPRGCTLR_EN;

    if (cfg->systick) {
        systick_on();
        __asm volatile ("cpsie i" ::: "memory");   /* need IRQs for SysTick test */
    } else {
        systick_off();
        /* PURE deterministic mode: mask ALL interrupts (PRIMASK=1) so no
         * SysTick / EXTI / stray IRQ can preempt det_iter mid-node and break
         * the structural invariant. The UART CLI still works because cli_poll
         * peeks the RX FIFO by polling (no IRQ needed). Re-enabled only if the
         * user switches to a workload that needs interrupts. */
        __asm volatile ("cpsid i" ::: "memory");
    }
}

void etm_selftrace_iterate(void)
{
    g_seed = det_iter(g_seed);
    g_sink = g_seed;
}
