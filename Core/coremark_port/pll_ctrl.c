/*
 * pll_ctrl.c -- runtime PLL1 reprogramming shared by cold init and the UART
 * CLI. Same code path both places so SystemCoreClock, flash wait states, APB
 * dividers and UART baud stay consistent with the real hardware clocks.
 *
 * H7 UART kernel clock is selected per group via RCC_D2CCIP2R.USART16SEL.
 * The default CubeMX MSP wires USART1 to PLL3, so a PLL1-only reprogram
 * ought to leave UART alone -- in practice it still glitches during the
 * sysclk park/HSI dance. Explicitly pointing UART1 at HSI here decouples
 * baud from every PLL, so the CLI stays responsive across `pll --apply`.
 */
#include "stm32h7xx_hal.h"
#include "pll_ctrl.h"

/* Board oscillator. Change here if the crystal is swapped. */
#define BOARD_HSE_HZ 25000000u

void pll_ctrl_read(struct pll_state *s)
{
    uint32_t sel = RCC->PLLCKSELR;
    uint32_t cfg = RCC->PLLCFGR;
    uint32_t div = RCC->PLL1DIVR;

    /* DIVN/P/Q/R are stored "field = value - 1" in RCC_PLL1DIVR (RM0433 §8.7.9). */
    s->m        = (sel >> 4)  & 0x3F;
    s->n        = ((div >>  0) & 0x1FF) + 1;
    s->p        = ((div >>  9) & 0x7F)  + 1;
    s->q        = ((div >> 16) & 0x7F)  + 1;
    s->r        = ((div >> 24) & 0x7F)  + 1;
    s->vcirange = (cfg >>  2) & 0x3;
    s->hse_hz   = BOARD_HSE_HZ;
}

/* sysclk = pll1_p_ck = HSE * N / (M * P). */
uint32_t pll_ctrl_sysclk_hz(const struct pll_state *s)
{
    if (!s->m || !s->p) return 0;
    return (uint32_t)(((uint64_t)s->hse_hz * s->n) / s->m / s->p);
}

uint32_t pll_ctrl_pll1r_hz(const struct pll_state *s)
{
    if (!s->m || !s->r) return 0;
    return (uint32_t)(((uint64_t)s->hse_hz * s->n) / s->m / s->r);
}

/* TRACECLK on the pin. RM0433: TRACECLK is derived from pll1_r_ck, and the
 * TPIU parallel port drives DDR data so the pin clock runs at HALF the
 * internal bit-clock (pll1_r_ck). Board-measured on the scope: R=2 ->
 * pll1_r_ck 225MHz -> TRACECLK pin 112MHz; R=8 -> 56.25MHz -> 28MHz. So the
 * visible TRACECLK edge rate = pll1_r_ck / 2. */
uint32_t pll_ctrl_traceclk_hz(const struct pll_state *s)
{
    return pll_ctrl_pll1r_hz(s) / 2u;
}

void pll_ctrl_uart1_to_hsi(UART_HandleTypeDef *huart)
{
    RCC_PeriphCLKInitTypeDef p = { 0 };
    p.PeriphClockSelection    = RCC_PERIPHCLK_USART1;
    p.Usart16ClockSelection   = RCC_USART16CLKSOURCE_HSI;
    (void)HAL_RCCEx_PeriphCLKConfig(&p);
    if (huart) HAL_UART_Init(huart);   /* recompute baud on 64 MHz HSI */
}

int pll_ctrl_apply(const struct pll_state *cfg)
{
    RCC_OscInitTypeDef osc = { 0 };
    RCC_ClkInitTypeDef clk = { 0 };

    /* Park sysclk on HSI so PLL1 can be disabled. RM0433 §8.5.5. */
    clk.ClockType    = RCC_CLOCKTYPE_SYSCLK;
    clk.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
    HAL_StatusTypeDef hs = HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_1);
    if (hs != HAL_OK) return -10 - hs;
    if ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_HSI) return -4;

    /* Disable PLL1, reprogram, re-enable. */
    osc.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    osc.HSEState       = RCC_HSE_ON;
    osc.PLL.PLLState   = RCC_PLL_OFF;
    if (HAL_RCC_OscConfig(&osc) != HAL_OK) return -1;

    osc.PLL.PLLState   = RCC_PLL_ON;
    osc.PLL.PLLSource  = RCC_PLLSOURCE_HSE;
    osc.PLL.PLLM       = cfg->m;
    osc.PLL.PLLN       = cfg->n;
    osc.PLL.PLLP       = cfg->p;
    osc.PLL.PLLQ       = cfg->q;
    osc.PLL.PLLR       = cfg->r;
    osc.PLL.PLLRGE     = cfg->vcirange;
    osc.PLL.PLLVCOSEL  = RCC_PLL1VCOWIDE;
    osc.PLL.PLLFRACN   = 0;
    if (HAL_RCC_OscConfig(&osc) != HAL_OK) return -2;

    /* Repoint sysclk at PLL1 and set the APB dividers CubeMX generated. */
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
    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_2) != HAL_OK) return -3;

    SystemCoreClockUpdate();
    return 0;
}
