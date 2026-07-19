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

/* Flash wait states scale with HCLK at the active VOS. We keep it conservative
 * (LATENCY_2 covers HCLK<=~185MHz at VOS3/VOS1); adjust if HCLK is raised. */
#ifndef BOARD_FLASH_LATENCY
#define BOARD_FLASH_LATENCY FLASH_LATENCY_2
#endif

void board_clock_override(void)
{
    RCC_OscInitTypeDef osc = {0};
    RCC_ClkInitTypeDef clk = {0};

    /* Re-program PLL1 with our dividers (HSE source, wide VCO). */
    osc.OscillatorType      = RCC_OSCILLATORTYPE_HSE;
    osc.HSEState            = RCC_HSE_ON;
    osc.PLL.PLLState        = RCC_PLL_ON;
    osc.PLL.PLLSource       = RCC_PLLSOURCE_HSE;
    osc.PLL.PLLM            = PLL_M_OVR;
    osc.PLL.PLLN            = PLL_N_OVR;
    osc.PLL.PLLP            = PLL_P_OVR;
    osc.PLL.PLLQ            = PLL_Q_OVR;
    osc.PLL.PLLR            = PLL_R_OVR;
    osc.PLL.PLLRGE          = RCC_PLL1VCIRANGE_3;   /* ref 8..16 MHz */
    osc.PLL.PLLVCOSEL       = RCC_PLL1VCOWIDE;      /* 192..836 MHz */
    osc.PLL.PLLFRACN        = 0;
    if (HAL_RCC_OscConfig(&osc) != HAL_OK)
    {
        /* leave CubeMX's clock in place if our config is rejected */
        return;
    }

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
