/*
 * board_clock.c -- PLL/clock override for the trace bring-up (proposal 36).
 *
 * WHY A SEPARATE FILE: CubeMX "Generate Code" rewrites SystemClock_Config() in
 * main.c every time, clobbering any hand-set PLL dividers. This file lives
 * OUTSIDE CubeMX's management, so our clock config survives regeneration. Call
 * board_clock_override() from a legal CubeMX USER region (e.g. USER CODE BEGIN 2
 * in main(), right after SystemClock_Config()); it re-programs PLL1 to our
 * target and re-selects it as sysclk.
 *
 * Target (overridable at build time via -DPLL_*_OVR, HSE = 25 MHz on this board):
 *   default N=36 P=3 R=2 -> VCO=450M, sysclk=150M, HCLK=75M, pll1_r_ck=225M,
 *   TRACECLK (=pll1_r_ck/2) = 112.5MHz  (the doc-16 breakthrough operating point)
 *
 * To sweep frequency later (proposal 36 stage 4) just rebuild with different
 * -DPLL_N_OVR / -DPLL_P_OVR / -DPLL_R_OVR; no CubeMX involved.
 */
#include "stm32h7xx_hal.h"

/* PLL dividers -- overridable at build time. HSE is 25 MHz. */
#ifndef PLL_M_OVR
#define PLL_M_OVR 2      /* ref = 25/2 = 12.5 MHz (RANGE_3 8..16) */
#endif
#ifndef PLL_N_OVR
#define PLL_N_OVR 36     /* VCO = 12.5 * 36 = 450 MHz */
#endif
#ifndef PLL_P_OVR
#define PLL_P_OVR 3      /* sysclk = 450/3 = 150 MHz */
#endif
#ifndef PLL_Q_OVR
#define PLL_Q_OVR 4
#endif
#ifndef PLL_R_OVR
#define PLL_R_OVR 2      /* pll1_r_ck = 450/2 = 225 MHz -> TRACECLK 112.5 MHz */
#endif

/* PLL1 input clock range (ref = HSE/M). RANGE_3 = 8..16MHz (default, M=2 ref=12.5);
 * for 480M we use M=5 ref=5MHz which needs RANGE_1 (4..8MHz). Override via
 * -DPLL_VCIRANGE_OVR=RCC_PLL1VCIRANGE_1. */
#ifndef PLL_VCIRANGE_OVR
#define PLL_VCIRANGE_OVR RCC_PLL1VCIRANGE_3
#endif

/* Flash wait states scale with HCLK at the active VOS. We keep it conservative
 * (LATENCY_2 covers HCLK<=~185MHz at VOS3/VOS1); adjust if HCLK is raised. */
#ifndef BOARD_FLASH_LATENCY
#define BOARD_FLASH_LATENCY FLASH_LATENCY_2
#endif

void board_clock_override(void)
{
    RCC_OscInitTypeDef osc = {0};
    RCC_ClkInitTypeDef clk = {0};

    /* H7 requires PLL1 to be OFF before its dividers can be re-programmed.
     * CubeMX already runs sysclk off PLL1, so first switch sysclk to HSI and
     * disable PLL1, THEN reconfigure. Otherwise HAL_RCC_OscConfig rejects the
     * change (PLL busy) and we silently keep CubeMX's default clock. */
    clk.ClockType      = RCC_CLOCKTYPE_SYSCLK;
    clk.SYSCLKSource   = RCC_SYSCLKSOURCE_HSI;
    (void)HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_1);  /* HSI 64M, safe WS */

    /* High sysclk (>~300M) needs VOS0 + SYSCFG overdrive. main() set VOS3;
     * raise to VOS0 here. Guarded by -DBOARD_VOS0 so low-freq builds are
     * unaffected. (RM0433: set SCALE1 then SYSCFG ODEN -> ODRDY = VOS0.) */
#ifdef BOARD_VOS0
    __HAL_RCC_SYSCFG_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);
    while (!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}
    SYSCFG->PWRCR |= SYSCFG_PWRCR_ODEN;                /* overdrive -> VOS0 */
    while (!(SYSCFG->PWRCR & SYSCFG_PWRCR_ODEN)) {}
    (void)SYSCFG->PWRCR;
#endif

    osc.OscillatorType      = RCC_OSCILLATORTYPE_HSE;
    osc.HSEState            = RCC_HSE_ON;
    osc.PLL.PLLState        = RCC_PLL_OFF;             /* turn PLL1 off first */
    (void)HAL_RCC_OscConfig(&osc);

    /* Now program PLL1 with our dividers (HSE source, wide VCO). */
    osc.OscillatorType      = RCC_OSCILLATORTYPE_HSE;
    osc.HSEState            = RCC_HSE_ON;
    osc.PLL.PLLState        = RCC_PLL_ON;
    osc.PLL.PLLSource       = RCC_PLLSOURCE_HSE;
    osc.PLL.PLLM            = PLL_M_OVR;
    osc.PLL.PLLN            = PLL_N_OVR;
    osc.PLL.PLLP            = PLL_P_OVR;
    osc.PLL.PLLQ            = PLL_Q_OVR;
    osc.PLL.PLLR            = PLL_R_OVR;
    osc.PLL.PLLRGE          = PLL_VCIRANGE_OVR;     /* ref range, default 8..16 */
    osc.PLL.PLLVCOSEL       = RCC_PLL1VCOWIDE;      /* 192..836 MHz */
    osc.PLL.PLLFRACN        = 0;
    if (HAL_RCC_OscConfig(&osc) != HAL_OK)
    {
        return;                                    /* give up -> HSI stays */
    }

    /* NB: HAL_RCC_OscConfig() already enables DIVP1EN/DIVQ1EN/DIVR1EN for us
     * (verified on-board: RCC_PLLCFGR = 0x01ff010d, all three set), so
     * pll1_r_ck -- which feeds TRACECLK -- needs no extra poke here.
     * Watch the register OFFSETS when checking this over SWD: PLLCKSELR is at
     * RCC+0x28 (0x58024428) and PLLCFGR at RCC+0x2C (0x5802442C). Reading 0x28
     * by mistake shows 0x01020022, whose bit18 is clear, which looks exactly
     * like "pll1_r_ck disabled, no TRACECLK" and is purely a misread. */

    clk.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                       | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2
                       | RCC_CLOCKTYPE_D3PCLK1 | RCC_CLOCKTYPE_D1PCLK1;
    clk.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    clk.SYSCLKDivider  = RCC_SYSCLK_DIV1;
    clk.AHBCLKDivider  = RCC_HCLK_DIV2;
    clk.APB3CLKDivider = RCC_APB3_DIV1;
    clk.APB1CLKDivider = RCC_APB1_DIV1;
    clk.APB2CLKDivider = RCC_APB2_DIV1;
    clk.APB4CLKDivider = RCC_APB4_DIV1;
    HAL_RCC_ClockConfig(&clk, BOARD_FLASH_LATENCY);

    /* SystemCoreClock now reflects the new sysclk (used by CoreMark timing). */
    SystemCoreClockUpdate();
}
