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
    cm_benchmark_main();
    for (;;) { /* idle after the run; score already printed */ }
}
