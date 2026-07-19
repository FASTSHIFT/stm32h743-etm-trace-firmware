/*
 * CoreMark port for STM32H743 (proposal 36).
 *
 * Timing: DWT CYCCNT (free-running CPU cycle counter). start/stop capture the
 * counter; get_time returns the cycle delta; EE_TICKS_PER_SEC=SystemCoreClock
 * so time_in_secs() is correct at any CPU frequency (150M now, 480M later).
 *
 * Reporting: ee_printf (CoreMark's own, barebones/ee_printf.c) -> uart_putchar.
 * The actual UART instance is set up by CubeMX (MX_USARTx_UART_Init) and wired
 * through cm_uart_send_char() in main.c so this file has no HAL dependency.
 */
#include "coremark.h"
#include "core_portme.h"

#if VALIDATION_RUN
volatile ee_s32 seed1_volatile = 0x3415;
volatile ee_s32 seed2_volatile = 0x3415;
volatile ee_s32 seed3_volatile = 0x66;
#endif
#if PERFORMANCE_RUN
volatile ee_s32 seed1_volatile = 0x0;
volatile ee_s32 seed2_volatile = 0x0;
volatile ee_s32 seed3_volatile = 0x66;
#endif
#if PROFILE_RUN
volatile ee_s32 seed1_volatile = 0x8;
volatile ee_s32 seed2_volatile = 0x8;
volatile ee_s32 seed3_volatile = 0x8;
#endif
volatile ee_s32 seed4_volatile = ITERATIONS;
volatile ee_s32 seed5_volatile = 0;

/* ---- DWT cycle-counter timing ---------------------------------------- */
#define DWT_CTRL   (*(volatile ee_u32 *)0xE0001000u)
#define DWT_CYCCNT (*(volatile ee_u32 *)0xE0001004u)
#define DEMCR      (*(volatile ee_u32 *)0xE000EDFCu)
#define DEMCR_TRCENA   (1u << 24)
#define DWT_CYCCNTENA  (1u << 0)

static CORETIMETYPE start_time_val, stop_time_val;

static void dwt_init(void)
{
    DEMCR |= DEMCR_TRCENA;      /* enable trace/debug block */
    DWT_CYCCNT = 0;
    DWT_CTRL |= DWT_CYCCNTENA;  /* start cycle counter */
}

#define GETMYTIME(_t)        (*_t = DWT_CYCCNT)
#define MYTIMEDIFF(fin, ini) ((fin) - (ini))   /* 32-bit wrap is fine for CoreMark */

void start_time(void) { GETMYTIME(&start_time_val); }
void stop_time(void)  { GETMYTIME(&stop_time_val); }

CORE_TICKS get_time(void)
{
    return (CORE_TICKS)MYTIMEDIFF(stop_time_val, start_time_val);
}

secs_ret time_in_secs(CORE_TICKS ticks)
{
    /* ticks are CPU cycles; SystemCoreClock cycles per second. */
    return ((secs_ret)ticks) / (secs_ret)EE_TICKS_PER_SEC;
}

ee_u32 default_num_contexts = 1;

/* printf backend: uart_send_char is defined in this port's ee_printf.c and
 * forwards to cm_uart_send_char() (main.c USER region, CubeMX UART). */

/* ---- init / fini ----------------------------------------------------- */
void portable_init(core_portable *p, int *argc, char *argv[])
{
    (void)argc;
    (void)argv;
    dwt_init();

    if (sizeof(ee_ptr_int) != sizeof(ee_u8 *))
        ee_printf("ERROR! ee_ptr_int size mismatch!\n");
    if (sizeof(ee_u32) != 4)
        ee_printf("ERROR! ee_u32 must be 32-bit!\n");
    p->portable_id = 1;
}

void portable_fini(core_portable *p)
{
    p->portable_id = 0;
}
