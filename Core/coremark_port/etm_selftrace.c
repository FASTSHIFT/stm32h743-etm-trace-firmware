/*
 * etm_selftrace.c -- self-contained ETMv4 trace bring-up and deterministic
 * workload for the DAP-vs-FPGA cross-check. All previous compile-time knobs
 * (ETM_SELFTRACE_SYSTICK, TRACE_BB, TRACE_STALL) are now runtime fields of
 * struct etm_cfg (see etm_selftrace.h) so the UART CLI can reconfigure them
 * without a rebuild.
 *
 * Addresses (system-bus view, RM0433 + DDI0489F):
 *   GPIOE  0x58021000   RCC_AHB4ENR 0x580244E0
 *   DBGMCU_CR 0x5C001004  TPIU 0x5C015000  CSTF 0x5C013000  ETF 0x5C014000
 *   ETM 0xE0041000        DEMCR 0xE000EDFC  SYST_CSR 0xE000E010
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "etm_selftrace.h"

#define REG(a) (*(volatile uint32_t *)(a))

const struct etm_cfg etm_cfg_default = {
    .bb      = 1,   /* dense stream for the DAP/FPGA cross-check */
    .stall   = 1,   /* lossless (CPU stalls when ETF fills) */
    .systick = 0,   /* no SysTick exceptions => tightest deterministic loop */
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

static void systick_off(void) { REG(0xE000E010) = 0; }

static void systick_on(void)
{
    /* 1 ms tick at 150 MHz sysclk (RVR=150000-1); short enough that several
     * interrupts land inside a multi-ms capture window. */
    REG(0xE000E014) = 150000u - 1u;
    REG(0xE000E018) = 0;
    REG(0xE000E010) = 0x00000007u;   /* ENABLE | TICKINT | CLKSOURCE=core */
}

/* ------- ETM/TPIU/CSTF/ETF/GPIO setup (parallel 4-bit) ------------------- */

void etm_selftrace_setup(const struct etm_cfg *cfg)
{
    if (!cfg) cfg = &etm_cfg_default;

    /* 0) GPIOE PE2..PE6 -> AF0, very-high speed, push-pull. */
    REG(0x580244E0) |= 0x00000010u;
    uint32_t moder = REG(0x58021000);
    moder &= ~(0x3FFFu << 4);
    moder |= (0x2u << 4) | (0x2u << 6) | (0x2u << 8) | (0x2u << 10) | (0x2u << 12);
    REG(0x58021000) = moder;
    REG(0x58021008) |= (0x3u << 4) | (0x3u << 6) | (0x3u << 8) | (0x3u << 10) | (0x3u << 12);
    REG(0x58021004) &= ~0x7Cu;
    REG(0x58021020) &= ~(0xFFFFFu << 8);

    /* 1) core trace power + 2) DBGMCU trace clocks. */
    REG(0xE000EDFC) = 0x01000000u;
    REG(0x5C001004) |= 0x00700000u;

    /* 3) TPIU: 4-bit parallel + continuous formatter, no test pattern. */
    REG(0x5C015FB0) = 0xC5ACCE55u;
    REG(0x5C015004) = 0x00000008u;
    REG(0x5C0150F0) = 0x00000000u;
    REG(0x5C015304) = 0x00000102u;
    REG(0x5C015204) = 0;

    /* 3b) CSTF funnel: enable ETM slave port S0. */
    REG(0x5C013FB0) = 0xC5ACCE55u;
    REG(0x5C013000) |= 0x00000001u;

    /* 3c) ETF: hardware-FIFO mode -> TPIU. */
    REG(0x5C014020) = 0x00000000u;
    REG(0x5C014FB0) = 0xC5ACCE55u;
    REG(0x5C014028) = 0x00000002u;
    REG(0x5C014304) = 0x00000001u;
    REG(0x5C014020) = 0x00000001u;

    /* 4) ETM (ETMv4): apply the requested BB / STALL from cfg. */
    REG(0xE0041FB0) = 0xC5ACCE55u;
    REG(0xE0041310) = 0x00000008u;   /* TRCPDCR.PU power-up */
    REG(0xE0041004) = 0x00000000u;   /* disable to program */
    for (volatile int i = 0; i < 1000; i++)
        if (REG(0xE004100C) & 0x1u) break;

    REG(0xE0041010) = cfg->bb ? 0x00000008u : 0x00000000u;  /* TRCCONFIGR.BB */
    REG(0xE0041040) = 0x00000002u;                          /* TRCTRACEIDR = 2 */
    REG(0xE004102C) = cfg->stall ? 0x0000010Cu : 0x00000000u; /* TRCSTALLCTLR */
    REG(0xE0041080) = 0x00000201u;                          /* TRCVICTLR trace-all */
    REG(0xE0041034) = 0x0000000Cu;                          /* TRCSYNCPR 2^12 */
    REG(0xE0041004) = 0x00000001u;                          /* TRCPRGCTLR.EN */

    if (cfg->systick) systick_on();
    else              systick_off();
}

void etm_selftrace_iterate(void)
{
    g_seed = det_iter(g_seed);
    g_sink = g_seed;
}
