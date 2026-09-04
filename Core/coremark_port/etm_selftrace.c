/*
 * etm_selftrace.c -- self-contained ETMv4 trace bring-up, done ENTIRELY in
 * firmware, plus a small deterministic workload. No debugger/openocd is needed
 * to configure trace: the firmware sets up GPIO trace pins, TPIU (4-bit
 * parallel), the CoreSight funnel, the ETF, and the ETM, then disables SysTick
 * and runs a fixed loop forever.
 *
 * WHY: configuring trace from openocd after the CPU is already running has been
 * unreliable on this H7 (the D1 debug domain / AP intermittently stalls when a
 * running program is poked). Doing it in firmware, right after the clock is up
 * and while the CPU stays busy in a tight loop, keeps D1 awake and needs the
 * DAP only for `reset` + reading the ETF -- exactly the golden cross-check
 * path (DAP reads the ETF over SWD with parity/retry -> cannot be corrupted by
 * the TPIU parallel port / FPGA sampling we want to characterise).
 *
 * The register sequence mirrors target/etm_enable_h743.cfg (verified on-board),
 * but BB=0, STALL=0, SysTick OFF for the cleanest, most predictable stream.
 *
 * Addresses (system-bus view, RM0433 + DDI0489F):
 *   GPIOE  0x58021000   RCC_AHB4ENR 0x580244E0
 *   DBGMCU_CR 0x5C001004  TPIU 0x5C015000  CSTF 0x5C013000  ETF 0x5C014000
 *   ETM 0xE0041000        DEMCR 0xE000EDFC  SYST_CSR 0xE000E010
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>

#define REG(a) (*(volatile uint32_t *)(a))

/* ------- deterministic workload knobs ------------------------------------ */
/* A tiny fixed call tree. It is short enough that several full iterations fit
 * inside the 4 KB ETF, so a DAP read of the ETF holds many complete, identical
 * loop passes -> the reference byte pattern is self-verifying (it must repeat).
 * No data-dependent branches, no memory traffic beyond the stack. */
static volatile uint32_t g_sink;

static uint32_t leaf_add(uint32_t x) { return x + 1u; }      /* one BL + one return */
static uint32_t leaf_xor(uint32_t x) { return x ^ 0x55u; }

static uint32_t node(uint32_t x)
{
    x = leaf_add(x);   /* call */
    x = leaf_xor(x);   /* call */
    return x;
}

/* One deterministic iteration: fixed sequence of calls/returns + a fixed-count
 * loop. Identical every pass -> identical ETM atoms every pass. */
static uint32_t det_iter(uint32_t seed)
{
    uint32_t a = seed;
    for (int i = 0; i < 8; i++)     /* fixed trip count -> fixed atom pattern */
        a = node(a);
    return a;
}

/* ------- SysTick off ------------------------------------------------------ */
static void systick_off(void)
{
    REG(0xE000E010) = 0;   /* SYST_CSR = 0: disable counter + int + clksrc */
}

#ifdef ETM_SELFTRACE_SYSTICK
/* ------- SysTick ON (test: does periodic exception disturb capture/decode?) -
 * Build with -DETM_SELFTRACE_SYSTICK to keep SysTick firing. This exercises the
 * ETMv4 EXCEPTION / EXCEPTION_RET path (entry+return address) in the decoder --
 * historically the mortrall false-recursion hot spot. SysTick_Handler already
 * exists in stm32h7xx_it.c (HAL_IncTick). We set a short reload so several
 * interrupts land inside the ETF window.
 *
 * Gated behind ETM_SELFTRACE_SYSTICK so -Werror doesn't fire on the unused
 * function in the default (SysTick-off) build. */
static void systick_on(void)
{
    /* 1 ms tick at the post-override core clock (150 MHz sysclk -> RVR=150000-1).
     * Matches the firmware's normal 1 kHz HAL tick, so several SysTick
     * interrupts land in a multi-ms capture window. SYST_RVR is 24-bit; 149999
     * fits. */
    REG(0xE000E014) = 150000u - 1u; /* SYST_RVR: 1 ms @150 MHz */
    REG(0xE000E018) = 0; /* SYST_CVR: clear current */
    /* SYST_CSR: ENABLE(0) | TICKINT(1) | CLKSOURCE(2, processor clock) */
    REG(0xE000E010) = 0x00000007u;
}
#endif

/* ------- trace bring-up (mirrors etm_enable_h743.cfg, BB=0/STALL=0) ------- */
static void trace_setup_4bit(void)
{
    /* 0) GPIOE PE2..PE6 -> AF0, very-high speed, push-pull */
    REG(0x580244E0) |= 0x00000010u;            /* RCC_AHB4ENR GPIOEEN */
    uint32_t moder = REG(0x58021000);
    moder &= ~(0x3FFFu << 4);
    moder |= (0x2u << 4) | (0x2u << 6) | (0x2u << 8) | (0x2u << 10) | (0x2u << 12);
    REG(0x58021000) = moder;
    REG(0x58021008) |= (0x3u << 4) | (0x3u << 6) | (0x3u << 8) | (0x3u << 10) | (0x3u << 12);
    REG(0x58021004) &= ~0x7Cu;                 /* push-pull */
    REG(0x58021020) &= ~(0xFFFFFu << 8);       /* AFRL PE2..6 = AF0 */

    /* 1) core trace power */
    REG(0xE000EDFC) = 0x01000000u;             /* DEMCR.TRCENA */
    /* 2) DBGMCU: TRACECLKEN + D1/D3 debug clocks (keeps D1 debug alive) */
    REG(0x5C001004) |= 0x00700000u;

    /* 3) TPIU: 4-bit parallel, continuous formatter */
    REG(0x5C015FB0) = 0xC5ACCE55u;             /* unlock */
    REG(0x5C015004) = 0x00000008u;             /* CURPSIZE = 4-bit */
    REG(0x5C0150F0) = 0x00000000u;             /* SPPR = parallel */
    REG(0x5C015304) = 0x00000102u;             /* FFCR: EnFCont */
    REG(0x5C015204) = 0;                        /* CURTPM test pattern OFF */

    /* 3b) CSTF funnel: enable ETM slave port S0 */
    REG(0x5C013FB0) = 0xC5ACCE55u;
    REG(0x5C013000) |= 0x00000001u;

    /* 3c) ETF: hardware-FIFO mode (drains to TPIU for the FPGA capture) */
    REG(0x5C014020) = 0x00000000u;             /* CTL off to program */
    REG(0x5C014FB0) = 0xC5ACCE55u;
    REG(0x5C014028) = 0x00000002u;             /* MODE = HW FIFO */
    REG(0x5C014304) = 0x00000001u;             /* FFCR: EnFt */
    REG(0x5C014020) = 0x00000001u;             /* CTL: TraceCaptEn */

    /* 4) ETM (ETMv4), BB=0, STALL=0 */
    REG(0xE0041FB0) = 0xC5ACCE55u;             /* unlock */
    REG(0xE0041310) = 0x00000008u;             /* TRCPDCR.PU power-up */
    REG(0xE0041004) = 0x00000000u;             /* disable to program */
    for (volatile int i = 0; i < 1000; i++)    /* wait TRCSTATR.IDLE */
        if (REG(0xE004100C) & 0x1u) break;
    /* SATURATION MODE (proposal: dense stream to kill halfsync idle).
     * BB=1 emits an Address packet for EVERY taken branch -> byte rate jumps
     * ~20-30x, filling the TPIU port so it stops emitting halfsync filler.
     * A dense back-to-back-frame stream lets the deframer lock without the
     * nibble-grouping drift that produced 0xD5 artifacts on the sparse stream,
     * so the FPGA capture deframes to a clean ETM byte sequence that can be
     * diffed byte-for-byte against the DAP-read ETF golden.
     * STALL=1 (ISTALL + LEVEL=max, 0x10C) throttles the CPU if the ETF fills,
     * so trace is LOSSLESS (no Overflow packets to complicate the diff). */
    REG(0xE0041010) = 0x00000008u;             /* TRCCONFIGR: BB on (bit3) */
    REG(0xE0041040) = 0x00000002u;             /* TRCTRACEIDR = 2 */
    REG(0xE004102C) = 0x0000010Cu;             /* TRCSTALLCTLR: ISTALL + LEVEL max */
    REG(0xE0041080) = 0x00000201u;             /* TRCVICTLR: trace-all + started */
    REG(0xE0041034) = 0x0000000Cu;             /* TRCSYNCPR: 2^12 sync period */
    REG(0xE0041004) = 0x00000001u;             /* TRCPRGCTLR.EN = 1 */
}

/* Public entry: call from main() after the clock is configured. Never returns.
 * Trace is fully set up in firmware; SysTick is off; the CPU spins a fixed
 * deterministic loop so the ETM byte stream is periodic and hand/DAP-verifiable. */
void etm_selftrace_run(void)
{
#ifdef ETM_SELFTRACE_SYSTICK
    systick_on();    /* test: periodic SysTick exception during the loop */
#else
    systick_off();
#endif
    trace_setup_4bit();

    uint32_t a = 0x1234u;
    for (;;) {
        a = det_iter(a);
        g_sink = a;   /* keep live */
    }
}
