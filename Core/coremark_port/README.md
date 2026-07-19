# CoreMark port for STM32H743 (proposal 36)

CubeMX-safe: everything here lives OUTSIDE CubeMX's managed files, so
"Generate Code" never touches it. CoreMark upstream is the `Core/coremark`
git submodule (untouched, version-pinned).

## Files
- `board_clock.c` — `board_clock_override()`: re-programs PLL1 to our target
  (default N=36 P=3 R=2, HSE=25M -> sysclk 150M / **TRACECLK 112.5M**). Survives
  CubeMX regen (which clobbers `SystemClock_Config` in main.c). Sweep frequency
  via `-DPLL_N_OVR/-DPLL_P_OVR/-DPLL_R_OVR`.
- `cm_uart.c` — UART shim: `cm_uart_init(&huart1)` registers the CubeMX UART;
  `cm_uart_send_char()` is what CoreMark's ee_printf calls. Also `coremark_main()`
  which calls the (renamed) upstream CoreMark main.
- `core_portme.c/.h` — CoreMark port: DWT CYCCNT timing, M7 data types,
  EE_TICKS_PER_SEC = SystemCoreClock (correct at any freq).
- `ee_printf.c` — copy of CoreMark's ee_printf with `uart_send_char` forwarding
  to `cm_uart_send_char`.

## After every CubeMX "Generate Code", add these to main.c USER regions:

`/* USER CODE BEGIN Includes */`
```c
#include "coremark_port.h"
```

`/* USER CODE BEGIN 2 */`  (runs after SystemClock_Config + MX_USART1_UART_Init)
```c
board_clock_override();     /* PLL -> 150M sysclk / 112.5M TRACECLK */
cm_uart_init(&huart1);      /* USART1 PA9/PA10, 115200 8N1 */
coremark_main();            /* run CoreMark, print score over UART, idle */
```
(Remove the old `main_loop();` call if present — coremark_main() never returns.)

## Makefile additions
```
C_SOURCES += \
Core/coremark/core_list_join.c Core/coremark/core_main.c \
Core/coremark/core_matrix.c Core/coremark/core_state.c \
Core/coremark/core_util.c \
Core/coremark_port/core_portme.c Core/coremark_port/ee_printf.c \
Core/coremark_port/cm_uart.c Core/coremark_port/board_clock.c
C_INCLUDES += -ICore/coremark -ICore/coremark_port
# rename CoreMark's main() so it doesn't clash with firmware main():
Core/coremark/core_main.o: CFLAGS += -Dmain=cm_benchmark_main
# stage-specific: -O0 (stage 0/1 baseline), ITERATIONS + run type:
C_DEFS += -DITERATIONS=2000 -DPERFORMANCE_RUN=1 -DFLAGS_STR=\"-O0\"
```
`ITERATIONS` picks the run length (0 = auto-calibrate; a fixed small value like
2000 keeps each run short for trace capture). Bump for a full ~10s score run.
