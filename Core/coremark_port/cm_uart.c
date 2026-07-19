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

void coremark_main(void)
{
    /* Boot banner: proves the UART path works and the baud is right, printed
     * BEFORE CoreMark so a late-attached terminal always catches it. If you
     * see this but no CoreMark report, the issue is CoreMark, not the UART. */
    cm_uart_puts("\r\n=== H743 CoreMark (proposal 36 stage 0) ===\r\n");
    cm_uart_puts("UART OK, starting CoreMark...\r\n");

    cm_benchmark_main();

    cm_uart_puts("=== CoreMark done ===\r\n");
    /* Heartbeat so a terminal attached at any time confirms we're alive.
     * Use a dumb busy-loop delay (NOT HAL_Delay) so it does not depend on
     * SysTick, which board_clock_override() may leave misconfigured. */
    for (;;)
    {
        volatile unsigned i;
        cm_uart_puts("hb\r\n");
        for (i = 0; i < 5000000u; i++) { }
    }
}
