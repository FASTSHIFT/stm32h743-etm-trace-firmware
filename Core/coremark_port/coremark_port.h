/*
 * coremark_port.h -- entry hooks for the CoreMark bring-up (proposal 36).
 *
 * These live outside CubeMX's managed files so regeneration never touches them.
 * From a legal CubeMX USER region in main(), call:
 *     board_clock_override();      // re-program PLL to our TRACECLK target
 *     cm_uart_init(&huart1);       // tell the port which UART to print on
 *     coremark_main();             // run CoreMark, print score, loop
 */
#ifndef COREMARK_PORT_H
#define COREMARK_PORT_H

#include "stm32h7xx_hal.h"

/* Re-program PLL1 to the trace target (default 150M sysclk / 112.5M TRACECLK).
 * Call right after SystemClock_Config() in main(). */
void board_clock_override(void);

/* Register the CubeMX-created UART handle for ee_printf output (PA9/PA10 = USART1). */
void cm_uart_init(UART_HandleTypeDef *huart);

/* One char out the registered UART (called by ee_printf's uart_send_char). */
void cm_uart_send_char(char c);

/* CoreMark entry: runs the benchmark (calls upstream main()), prints the score
 * over UART, then idles. Never returns. */
void coremark_main(void);

#endif /* COREMARK_PORT_H */
