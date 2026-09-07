/*
 * pll_ctrl.h -- PLL1 dividers as RUNTIME state, callable from both cold init
 * and the UART CLI. Splits out the PLL dance from board_clock.c so we can
 * re-run it after boot with new dividers, without a reset.
 */
#ifndef PLL_CTRL_H
#define PLL_CTRL_H

#include <stdint.h>

/* Current PLL1 dividers as last committed by pll_ctrl_apply(). Written on
 * cold init so the CLI's `pll --show` reflects reality even before any user
 * override. */
struct pll_state {
    uint32_t m, n, p, q, r;   /* PLLM/N/P/Q/R (raw field-visible values) */
    uint32_t vcirange;        /* RCC_PLL1VCIRANGE_* */
    uint32_t hse_hz;          /* board HSE oscillator; 25 MHz on this board */
};

/* Read the LIVE PLL1 dividers from RCC and populate the struct. */
void pll_ctrl_read(struct pll_state *out);

/* Apply new dividers. Same dance as the boot-time override:
 *   sysclk -> HSI, PLL1 OFF, program dividers, PLL1 ON, sysclk -> PLL1.
 * Called with the CPU running, so the selftrace loop is briefly interrupted
 * but resumes at the new frequency. Returns 0 on OK, negative on HAL error.
 * The rest of the peripherals (UART baud, HAL SysTick, USB VCP) are kept
 * consistent via HAL_RCC_ClockConfig + SystemCoreClockUpdate + HAL_UART_Init
 * of the print UART (caller's responsibility to notify the shim if any). */
int pll_ctrl_apply(const struct pll_state *cfg);

/* Convenience derived-clock helpers -- pure math, use the passed struct. */
uint32_t pll_ctrl_sysclk_hz(const struct pll_state *s);   /* pll1_p_ck */
uint32_t pll_ctrl_pll1r_hz (const struct pll_state *s);   /* pll1_r_ck */
uint32_t pll_ctrl_traceclk_hz(const struct pll_state *s); /* alias of pll1_r_ck */

/* Cached last-applied dividers (populated by board_clock_override at cold
 * init and by every successful pll_ctrl_apply thereafter). CLI reads this
 * for `pll --show`; DAP-side tools shouldn't need to poke RCC directly. */
const struct pll_state *pll_ctrl_get_current(void);
void                    pll_ctrl_set_current(const struct pll_state *s);

/* Reroute USART1's kernel clock to HSI (64 MHz, always on) so its baud
 * stays stable across arbitrary PLL1/PLL3 reprogramming. Call once from
 * cli_init() after cm_uart_init(). */
void pll_ctrl_uart1_to_hsi(UART_HandleTypeDef *huart);

#endif /* PLL_CTRL_H */
