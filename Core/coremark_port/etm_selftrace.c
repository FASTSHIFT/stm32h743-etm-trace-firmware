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
#include <stdio.h>
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
    .cc      = 0,   /* no cycle counting (matches historical golden) */
};

/* ------- deterministic workload ------------------------------------------ */
/* A tiny fixed call tree. It is short enough that several full iterations fit
 * inside the 4 KB ETF, so a DAP read of the ETF holds many complete, identical
 * loop passes -> the reference byte pattern is self-verifying (it must repeat).
 * No data-dependent branches, no memory traffic beyond the stack. */
static volatile uint32_t g_sink;
static volatile uint32_t g_seed = 0x1234u;

/* P0 DWT ground-truth probe target. A plain global (not const/register) so DWT
 * comparator 0 can watch writes to it. etm_dwt_probe_tick() writes an
 * increasing value each time; the decoder should recover exactly that sequence
 * from the DWT data-value packets. Stands in for &g_running_tasks until NuttX. */
static volatile uint32_t g_dwt_probe;

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

    /* 3b) CSTF funnel: S0=ETM (always). When DWT data trace is requested, also
     * enable S1=ITM (RM0433 §60.5.4 p.3135: S0=ETM, S1=ITM -- documented) so
     * the ITM/DWT data-value packets merge into the same ATB stream feeding
     * ETF/TPIU (the parallel port). Then raise S1(ITM) priority ABOVE S0(ETM)
     * via READ-MODIFY-WRITE (preserve the reserved bits -- overwriting the whole
     * PRIORITY word corrupted the funnel in an earlier attempt). */
    ETM_REG(CSTF_LAR) = CS_LAR_UNLOCK;
    ETM_REG(CSTF_CTRL) |= CSTF_CTRL_ENS0 | (cfg->dwt ? CSTF_CTRL_ENS1 : 0u);
    if (cfg->dwt) {
        uint32_t prio = ETM_REG(CSTF_PRIORITY);
        prio &= ~(CSTF_PRIPORT0_MASK | CSTF_PRIPORT1_MASK);
        prio |= (1u << 0)     /* PRIPORT0 = 1 (ETM, lower) */
             |  (0u << 3);    /* PRIPORT1 = 0 (ITM, highest) */
        ETM_REG(CSTF_PRIORITY) = prio;
    }

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
                            | (cfg->ts ? ETM_TRCCONFIGR_TS : 0u)
                            | (cfg->cc ? ETM_TRCCONFIGR_CCI : 0u);
    ETM_REG(ETM_TRCTRACEIDR) = ETM_TRCTRACEIDR_ID2;
    /* Cycle-count threshold: only meaningful when CCI is on. Emit a Cycle Count
     * element once >= THRESHOLD cycles have accumulated (>= TRCIDR3.CCITMIN). */
    if (cfg->cc)
        ETM_REG(ETM_TRCCCCTLR) = ETM_TRCCCCTLR_THRESHOLD;
    /* TRCSTALLCTLR: lossless backstop. NB (DDI0494D §3.4.7): LEVEL!=0 may
     * SUPPRESS in-stream timestamps. When ts is requested, drop the stall LEVEL
     * to 0 (keep ISTALL so overflow is still bounded) so timestamp packets are
     * not starved; without ts keep the historical max-invasion setting. */
    ETM_REG(ETM_TRCSTALLCTLR) = cfg->stall
        ? (cfg->ts ? ETM_TRCSTALLCTLR_ISTALL : ETM_TRCSTALLCTLR_LOSSLESS)
        : 0u;
    ETM_REG(ETM_TRCVICTLR) = ETM_TRCVICTLR_TRACE_ALL;
    /* NB: board-measured 2026 — lowering TRCSYNCPR (tried 2^8) does NOT raise
     * the TIMESTAMP packet rate on this M7 (TS stayed ~1 per 105 us either
     * way): TS insertion here is driven by an internal period, not the sync
     * period. So keep the 4K sync period; time-base resolution between TS
     * anchors comes from host-side interpolation. But the rate CAN be raised
     * via TRCTSCTLR + the counter (below), which is the real knob. */
    ETM_REG(ETM_TRCSYNCPR) = ETM_TRCSYNCPR_4K;

    /* Timestamp rate: leave TRCTSCTLR at 0 (implicit TS points only, ~1 per
     * 105 us board-measured). The ETMv4 way to make TS denser is a counter in
     * self-reload mode driving the timestamp event -- but on THIS M7
     * (instruction-only configuration) TRCCNTCTLR0/TRCCNTVR0 are RAZ/WI
     * (DDI0494D Table 3-1 note a: counter control/value exist only in the
     * instruction+data configuration). Board-confirmed: TRCCNTRLDVR0 accepts a
     * value but TRCCNTCTLR0 stays 0, so the counter never counts; pointing
     * TRCTSCTLR at counter-0-at-zero then leaves that resource permanently
     * active and CORRUPTS the trace (measured ~2.17M dropped calls vs 0). So
     * the counter path is a dead end here -- so instead we enable cycle
     * counting (cfg->cc, TRCCONFIGR.CCI above) and interpolate between the
     * sparse implicit TS anchors with real CPU cycle counts on the host. */
    ETM_REG(ETM_TRCTSCTLR) = 0;

    ETM_REG(ETM_TRCPRGCTLR) = ETM_TRCPRGCTLR_EN;

    /* 5) DWT data-value trace (nxtrace P0). Independent of the ETM instruction
     * stream: the DWT unit watches WRITES to dwt_watch_addr and the ITM forwards
     * the resulting data-value packets onto the ATB (ID 1), which the funnel
     * merges with the ETM stream (ID 2) into the one parallel-TPIU output. The
     * host then demuxes both streams from the same capture, sharing the ETM
     * global timestamp. DEMCR.TRCENA (set in step 1) already gates DWT+ITM. */
    if (cfg->dwt) {
        uint32_t addr = cfg->dwt_watch_addr;
        if (!addr)
            addr = (uint32_t)(uintptr_t)&g_dwt_probe;  /* P0 default target */

        /* ITM: unlock, then enable ITM + forward DWT packets to the ATB with a
         * non-zero TraceBusID so the funnel/formatter tags them as stream 1. */
        ETM_REG(ITM_LAR) = CS_LAR_UNLOCK;
        ETM_REG(ITM_TCR) = ITM_TCR_DWT_ATB;

        /* DWT comparator 0: exact-match (MASK=0) data-value packet on WRITE. */
        ETM_REG(DWT_FUNCTION0) = 0;                 /* disable while programming */
        ETM_REG(DWT_COMP0)     = addr;
        ETM_REG(DWT_MASK0)     = 0;                 /* match the exact address */
        ETM_REG(DWT_FUNCTION0) = DWT_FUNCTION_DATAVWRITE;

        /* Self-readback over UART -- bypasses openocd entirely (a debugger
         * connect CLEARS DWT_FUNCTION, so an SWD read always shows 0 and can't
         * tell us if the firmware armed it). Printing from the CPU is the only
         * way to see the value the hardware actually latched. DWT_CTRL.NOTRCPKT
         * (bit24) must be 0 for data-value packets to be emittable at all. */
        printf("[dwt] CTRL=0x%08lx COMP0=0x%08lx MASK0=0x%08lx FUNC0=0x%08lx "
               "ITM_TCR=0x%08lx (watch=0x%08lx)\r\n",
               (unsigned long)ETM_REG(DWT_CTRL_REG),
               (unsigned long)ETM_REG(DWT_COMP0),
               (unsigned long)ETM_REG(DWT_MASK0),
               (unsigned long)ETM_REG(DWT_FUNCTION0),
               (unsigned long)ETM_REG(ITM_TCR),
               (unsigned long)addr);
    }

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

/* ------- P0 DWT ground-truth probe --------------------------------------- */

uint32_t etm_dwt_probe_addr(void)
{
    return (uint32_t)(uintptr_t)&g_dwt_probe;
}

void etm_dwt_probe_tick(void)
{
    /* Each write to a DWT-watched address emits one data-value packet carrying
     * the written value. Use an incrementing counter so the host can verify the
     * recovered payload sequence is exactly 1,2,3,... (no drops, no dups) and
     * that each packet's timestamp falls within the concurrent ETM timeline. */
    static uint32_t n;
    g_dwt_probe = ++n;
}
