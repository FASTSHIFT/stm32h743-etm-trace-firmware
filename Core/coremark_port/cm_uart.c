/*
 * cm_uart.c -- UART output shim for CoreMark ee_printf (proposal 36).
 * Lives outside CubeMX's managed files. main() registers the CubeMX-created
 * UART handle (USART1, PA9/PA10, 115200 8N1) via cm_uart_init(); ee_printf's
 * uart_send_char() -> cm_uart_send_char() -> HAL_UART_Transmit.
 */
#include "coremark_port.h"

static UART_HandleTypeDef *s_huart = 0;

void cm_uart_init(UART_HandleTypeDef *huart)
{
    s_huart = huart;
    /* MX_USART1_UART_Init() ran at CubeMX's default clock, BEFORE
     * board_clock_override() changed sysclk/APB. Re-init here so the baud
     * divisor is recomputed for the new (post-override) UART kernel clock;
     * otherwise the baud rate is wrong and nothing legible comes out. */
    if (huart)
        HAL_UART_Init(huart);
}

/* Blocking string helper (no timing constraints; used for boot banner). */
static void cm_uart_puts(const char *s)
{
    while (s && *s)
        cm_uart_send_char(*s++);
}

void cm_uart_send_char(char c)
{
    if (!s_huart)
        return;
    /* blocking single-byte TX; CoreMark only prints at start/end, not timed. */
    HAL_UART_Transmit(s_huart, (uint8_t *)&c, 1, 100);
}

/*
 * coremark_main(): the upstream CoreMark main() is renamed to cm_benchmark_main
 * at compile time (-Dmain=cm_benchmark_main on core_main.c), so we can call it
 * from our firmware main() without symbol clash. It prints the CoreMark report
 * over UART via ee_printf, then we idle.
 */
extern int cm_benchmark_main(void);

/* Runtime cache control moved to cli.c (`cache on|off`); CoreMark itself no
 * longer touches SCB caches. Callers wanting cache during a run should
 * `cache on` from the CLI first, or let the boot default (both off) stand. */

void coremark_main_one(void)
{
    cm_benchmark_main();
    cm_uart_puts("--- CoreMark run complete ---\r\n");
}

void coremark_main(void)
{
    cm_uart_puts("\r\n=== H743 CoreMark (proposal 36) ===\r\n");
    cm_uart_puts("UART OK, starting CoreMark (looping)...\r\n");

    /* Run CoreMark FOREVER (re-run back-to-back). A single run finishes in a
     * few seconds and then the CPU would idle with no branch traffic, so the
     * trace probe could never catch the benchmark phase. Looping keeps the
     * ETM stream continuously busy so a capture at any time lands inside a
     * real CoreMark run (proposal 36 needs to trace the running benchmark). */
    for (;;)
    {
        cm_benchmark_main();
        cm_uart_puts("--- CoreMark run complete, restarting ---\r\n");
    }
}
