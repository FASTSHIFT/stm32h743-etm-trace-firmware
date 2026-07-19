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
    /* Boot banner: proves the UART path works and the baud is right. */
    cm_uart_puts("\r\n=== H743 CoreMark (proposal 36) ===\r\n");

    /* proposal 37 r30: RUNTIME cache toggle for a TRUE single-variable test.
     * The cache decision is read from Backup SRAM (D3 domain, 0x38800000),
     * which survives reset and is writable by openocd BEFORE resume. This way
     * BOTH runs flash the SAME .bin and decode with the SAME ELF -- the only
     * difference is one word openocd pokes, so mem.bin is byte-identical
     * (r30 killed the old -DBOARD_ENABLE_CACHE approach: it produced two
     * different binaries, 2615 bytes apart, breaking single-variable purity).
     *   openocd: mww 0x38800000 0x0000CACE  -> enable cache
     *            mww 0x38800000 0x00000000  -> keep cache disabled
     * Compile-time -DBOARD_ENABLE_CACHE still forces enable if the magic is
     * unset (back-compat / default). */
    {
        /* Flag word in RAM_D1 (0x24000000). CubeMX puts .data/.bss in DTCMRAM,
         * so the C startup NEVER touches RAM_D1 -- our flag survives from the
         * openocd poke (done at reset-halt, before main) through startup into
         * here. D1 domain SRAM is clocked by default; openocd can read/write it.
         *   openocd: mww 0x24000000 0x0000CACE -> enable ; 0x0 -> disable */
        volatile uint32_t *cache_flag = (volatile uint32_t *)0x24000000u;
        int enable = (*cache_flag == 0x0000CACEu);
#ifdef BOARD_ENABLE_CACHE
        if (*cache_flag != 0x00000000u)   /* magic 0 explicitly disables */
            enable = 1;
#endif
        if (enable) {
            SCB_EnableICache();
            SCB_EnableDCache();
            cm_uart_puts("I/D cache: ENABLED (runtime)\r\n");
        } else {
            cm_uart_puts("I/D cache: disabled (runtime)\r\n");
        }
    }
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
