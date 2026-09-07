/*
 * board_clock.c -- one-time PLL override at cold init. Now a thin wrapper
 * around pll_ctrl_apply() so both boot AND the runtime UART CLI take the same
 * code path (the 2026-09-07 sweep confirmed that any other path leaves
 * SystemCoreClock inconsistent and the whole system "runs at the old clock").
 *
 * Default dividers are the doc-16 breakthrough operating point:
 *   HSE = 25 MHz, M = 2  -> ref = 12.5 MHz  (VCIRANGE_3 = 8..16 MHz)
 *   N   = 36            -> VCO = 450 MHz    (WIDE = 192..836)
 *   P   = 3             -> sysclk = 150 MHz
 *   R   = 2             -> pll1_r_ck = TRACECLK = 225 MHz
 *
 * Wait, doc-16 says TRACECLK = 112.5 MHz. This is a persistent muddle:
 * board_clock.c originally documented "pll1_r_ck / 2 = 112.5 MHz TRACECLK",
 * but 2026-09-07 LA readback proved TRACECLK == pll1_r_ck directly, not
 * pll1_r_ck / 2. To keep the historical operating point (~112.5 MHz on the
 * physical TRACECK pin), we now emit pll1_r_ck = 225 MHz and the on-die TPIU
 * DDRs the 225 MHz down to a 112.5 MHz visible-edge rate (data changes on
 * both edges). The "TRACECLK" the LA / cortrace sees is 112.5 MHz.
 *
 * Overridable at build time via -DPLL_*_OVR (still supported for CI / bring-up
 * clones); at RUNTIME via the `pll` CLI command in cli.c.
 */
#include "stm32h7xx_hal.h"
#include "pll_ctrl.h"

/* PLL dividers -- overridable at build time. */
#ifndef PLL_M_OVR
#define PLL_M_OVR 2
#endif
#ifndef PLL_N_OVR
#define PLL_N_OVR 36
#endif
#ifndef PLL_P_OVR
#define PLL_P_OVR 3
#endif
#ifndef PLL_Q_OVR
#define PLL_Q_OVR 4
#endif
#ifndef PLL_R_OVR
/* R=4 -> pll1_r_ck = 450/4 = 112.5MHz -> TRACECLK pin = 56.25MHz (/2).
 * Chosen as the cold-init default because 56MHz is where the eye is open
 * (scope Q~7.0) vs the marginal SI at 112MHz pin (R=2, Q~3.5): see
 * stage4-datapath/23-center-aligned-capture-retro.md. Raise/lower at runtime
 * with the `pll --r N --apply` CLI command. */
#define PLL_R_OVR 4
#endif
#ifndef PLL_VCIRANGE_OVR
#define PLL_VCIRANGE_OVR RCC_PLL1VCIRANGE_3
#endif

/* Cached "last committed" state so `pll --show` doesn't have to re-read RCC
 * every time (still cheap to do, but a struct is nicer to hand to callers). */
static struct pll_state g_pll;

void board_clock_override(void)
{
    struct pll_state c = {
        .m        = PLL_M_OVR,
        .n        = PLL_N_OVR,
        .p        = PLL_P_OVR,
        .q        = PLL_Q_OVR,
        .r        = PLL_R_OVR,
        .vcirange = PLL_VCIRANGE_OVR,
        .hse_hz   = 25000000u,
    };
    if (pll_ctrl_apply(&c) == 0) {
        g_pll = c;
    } else {
        /* fall back to whatever the reset PLL config is; read it so
         * pll_ctrl_get_current() returns something sensible. */
        pll_ctrl_read(&g_pll);
    }
}

const struct pll_state *pll_ctrl_get_current(void)
{
    return &g_pll;
}

void pll_ctrl_set_current(const struct pll_state *s)
{
    g_pll = *s;
}
