# H743 ETM Trace Firmware

STM32H743 (Cortex-M7, ETMv4) firmware used as the trace **source** for an
ORBTrace-on-Artix-7 (XC7A35T) capture pipeline: the MCU emits 4-bit parallel
ETM trace over its TPIU pins, an FPGA captures it, ships it to a PC over UDP,
and it is decoded into a function-level Perfetto timeline.

The workload is [CoreMark](https://github.com/eembc/coremark) plus a
self-contained ETM bring-up path, so the board can produce clean, deterministic
trace without an external debugger configuring the trace unit at runtime.

## Layout

Only hand-written / hand-edited sources are tracked. CubeMX-generated HAL/CMSIS
drivers (`Drivers/`), the Keil project (`MDK-ARM/`), and all build outputs are
`.gitignore`d — regenerate `Drivers/` from `H743_Blink.ioc` via CubeMX
("Generate Code").

| Path | Purpose |
|------|---------|
| `Core/Src/` | CubeMX entry (`main.c`), HAL MSP, IT handlers, system init |
| `Core/coremark/` | CoreMark benchmark (git submodule, EEMBC) |
| `Core/coremark_port/` | CubeMX-safe port layer (see below) |
| `H743_Blink.ioc` | CubeMX project (source of truth for `Drivers/` + pinout) |
| `Makefile` | GCC (arm-none-eabi) build |
| `STM32H743ZITx_FLASH.ld`, `startup_stm32h743xx.s` | linker + startup |

### `Core/coremark_port/`

| File | Purpose |
|------|---------|
| `core_portme.c`, `ee_printf.c` | CoreMark platform port |
| `cm_uart.c` | USART1 (PA9/PA10 @115200) output + runtime cache-toggle flag |
| `board_clock.c` | PLL / VOS0 / cache overrides (compile-time macros: `PLL_N_OVR` etc.) |
| `etm_selftrace.c` | Self-contained ETM trace bring-up + deterministic loop |

## Build

Clone with the CoreMark submodule:

```bash
git clone --recurse-submodules <repo-url>
```

Build with the system arm-none-eabi toolchain (not a Vivado/Vitis-shipped one,
which lacks `libc_nano`):

```bash
make GCC_PATH=/usr/bin OPT=-O0
```

Key compile-time options (passed via `C_DEFS`):

- `PLL_N_OVR` / `PLL_P_OVR` / `PLL_R_OVR` — PLL frequency (HSE = 25 MHz).
  E.g. `N=36 P=3 R=2` → 150 MHz sysclk, 112.5 MHz TRACECLK.
- `-DETM_SELFTRACE` — run the firmware ETM bring-up + deterministic call tree
  (`det_iter → node → leaf_add/leaf_xor`) instead of the plain CoreMark loop.
- `-DETM_SELFTRACE_SYSTICK` — also enable the 1 ms SysTick during self-trace.
- `-DBOARD_VOS0 -DBOARD_FLASH_LATENCY=FLASH_LATENCY_4` — required above 300 MHz.

`-O0` keeps the call tree un-inlined so it is visible in the decoded trace.

## Flash

```bash
openocd -f interface/cmsis-dap.cfg -f target/stm32h7x.cfg \
  -c init -c "reset halt" \
  -c "program build/H743_Blink.hex verify" \
  -c "reset run" -c shutdown
```

## Hardware notes

- **HSE = 25 MHz** (board crystal), not 8 MHz.
- Trace pins: `PE2` = TRACECK, `PE3..PE6` = TRACED0..3.
- H743 silicon caps at 400 MHz (480 MHz is not reachable).
