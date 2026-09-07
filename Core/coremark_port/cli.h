/*
 * cli.h -- UART command line and workload scheduler for the H743 selftrace
 * bring-up. Owns:
 *   - argparse-based command dispatcher over USART1 (DAPLink VCP)
 *   - `run` command that selects the workload (selftrace / coremark / idle)
 *   - `pll` command that reprograms PLL1 at runtime
 *   - `trace` command that toggles ETMv4 BB / STALL knobs
 *   - `cache` command that flips I/D cache on/off (runtime, not compile flag)
 *
 * Nothing here is behind a #ifdef -- feature flags moved from compile to
 * runtime so ONE binary can drive every experiment (2026-09-07 sweep bug root
 * cause was firmware/host divergence on assumed clock; a single reprogrammable
 * binary sidesteps that).
 */
#ifndef CLI_H
#define CLI_H

#include "stm32h7xx_hal.h"

/* Register the shared UART; must be called after cm_uart_init(). */
void cli_init(UART_HandleTypeDef *huart);

/* Non-blocking poll (drain RX, execute completed lines). Safe from any
 * context; ETM stream not perturbed while no line is pending. */
void cli_poll(void);

/* Workload scheduler entry (called from main()). Never returns. Runs the
 * selected workload in a tight loop, polling the CLI between iterations. */
void workload_run(void);

/* Bytes-received counter for tests / triage. */
unsigned int cli_rx_count(void);

#endif /* CLI_H */
