/*
 * cli.c -- UART command line + workload scheduler.
 *
 * Every experiment knob that used to be a compile-time #define is now a
 * runtime command here, so ONE binary drives every case. The 2026-09-07
 * sweep proved that mixing openocd RCC pokes with a firmware whose
 * SystemCoreClock was set at cold init leaves the two out of sync -- the CLI
 * path avoids that entirely because the firmware itself owns the
 * reprogramming (via pll_ctrl_apply()) and immediately re-inits the UART for
 * the new kernel clock.
 *
 * Commands (argparse-based; every command supports --help):
 *   id                       print firmware + PLL + workload state
 *   pll [--show|--m N ..--apply]  inspect / reprogram PLL1
 *   run selftrace|coremark|idle [--bb N] [--stall N] [--systick N]
 *                            switch the workload; ETM flags apply to
 *                            'selftrace' only (they retune trace_setup_4bit)
 *   trace [--show|--bb N|--stall N|--systick N|--apply]
 *                            reconfigure ETM WITHOUT changing workload
 *   cache on|off             enable/disable I+D cache at runtime
 *   reset                    NVIC_SystemReset()
 */
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include "stm32h7xx_hal.h"
#include "argparse.h"
#include "pll_ctrl.h"
#include "etm_selftrace.h"
#include "coremark_port.h"
#include "cli.h"

/* --- UART plumbing ------------------------------------------------------- */

static UART_HandleTypeDef *s_huart;

int _write(int fd, char *buf, int len)
{
    (void)fd;
    if (!s_huart || len <= 0) return len;
    HAL_UART_Transmit(s_huart, (uint8_t *)buf, (uint16_t)len, 200);
    return len;
}

static unsigned int s_uart_ore = 0;   /* count of overrun events */

static int uart_getc(void)
{
    if (!s_huart) return -1;
    uint32_t isr = s_huart->Instance->ISR;
    /* Overrun: RX byte lost because we didn't drain fast enough. Clear it via
     * ICR and count -- otherwise ORE stays set and RXNE never re-arms. */
    if (isr & USART_ISR_ORE) {
        s_huart->Instance->ICR = USART_ICR_ORECF;
        s_uart_ore++;
    }
    if ((isr & USART_ISR_RXNE_RXFNE) == 0) return -1;
    return (int)(s_huart->Instance->RDR & 0xFFu);
}

/* --- workload state ------------------------------------------------------ */

enum workload_kind { WL_IDLE, WL_SELFTRACE, WL_COREMARK };

static enum workload_kind s_workload = WL_SELFTRACE;   /* boot default */
static struct etm_cfg     s_etm      = { .bb = 1, .stall = 1, .systick = 0 };
static uint8_t            s_etm_dirty = 1;   /* re-apply on next selftrace start */

/* Coordination flag: the workload loop checks this each iteration and, when
 * set, tears down the current work (etm/systick) and re-enters the CLI-level
 * dispatch. Set by `run` / `trace` command callbacks. */
static volatile uint8_t s_workload_changed;

/* --- line assembler ------------------------------------------------------ */

#define CLI_LINE_MAX 96
#define CLI_ARG_MAX  12

static char s_line[CLI_LINE_MAX];
static int  s_len = 0;
static unsigned int s_rx_bytes = 0;

/* --- command handlers ---------------------------------------------------- */

static const char *workload_name(void)
{
    switch (s_workload) {
    case WL_IDLE:      return "idle";
    case WL_SELFTRACE: return "selftrace";
    case WL_COREMARK:  return "coremark";
    }
    return "?";
}

static void print_pll_state(const struct pll_state *s)
{
    printf("PLL1: M=%lu N=%lu P=%lu Q=%lu R=%lu VCIRANGE=%lu\r\n",
           (unsigned long)s->m, (unsigned long)s->n, (unsigned long)s->p,
           (unsigned long)s->q, (unsigned long)s->r,
           (unsigned long)s->vcirange);
    printf("  HSE %lu  ref %lu  VCO %lu\r\n",
           (unsigned long)s->hse_hz, (unsigned long)(s->hse_hz / s->m),
           (unsigned long)((uint64_t)s->hse_hz * s->n / s->m));
    printf("  sysclk %lu  pll1_r_ck=TRACECLK %lu  SystemCoreClock %lu\r\n",
           (unsigned long)pll_ctrl_sysclk_hz(s),
           (unsigned long)pll_ctrl_traceclk_hz(s),
           (unsigned long)SystemCoreClock);
}

/* Simplest possible sanity command: prints its argv unmodified. Used to
 * verify UART round-trip + line-splitter without touching argparse or PLL. */
static int cmd_echo(int argc, const char **argv)
{
    printf("[echo] argc=%d\r\n", argc);
    for (int i = 0; i < argc; i++)
        printf("  argv[%d] = %s\r\n", i, argv[i]);
    return 0;
}

/* argparse smoke test: parse a tiny flag set, print what argparse produced.
 * No side effects. If this doesn't work, PLL commands never will. */
static int cmd_ap_test(int argc, const char **argv)
{
    int flag_a = 0, flag_b = 0;
    int int_x = -1, int_y = -1;
    static const char *usages[] = { "ap-test [flags]", NULL };
    struct argparse_option opts[] = {
        OPT_HELP(),
        OPT_BOOLEAN('a', "a-flag", &flag_a, "flag a", NULL, 0, 0),
        OPT_BOOLEAN('b', "b-flag", &flag_b, "flag b", NULL, 0, 0),
        OPT_INTEGER('x', "x", &int_x, "int x", NULL, 0, 0),
        OPT_INTEGER('y', "y", &int_y, "int y", NULL, 0, 0),
        OPT_END(),
    };
    struct argparse ap;
    argparse_init(&ap, opts, usages, 0);
    int rc = argparse_parse(&ap, argc, argv);
    printf("[ap-test] rc=%d  a=%d b=%d x=%d y=%d\r\n",
           rc, flag_a, flag_b, int_x, int_y);
    return 0;
}

static int cmd_id(int argc, const char **argv)
{
    (void)argc; (void)argv;
    printf("H743 selftrace fw, argparse CLI over USART1 @ 115200 8N1\r\n");
    printf("build: %s %s\r\n", __DATE__, __TIME__);
    print_pll_state(pll_ctrl_get_current());
    printf("workload: %s  (etm: bb=%u stall=%u systick=%u)\r\n",
           workload_name(), s_etm.bb, s_etm.stall, s_etm.systick);
    return 0;
}

static int cmd_reset(int argc, const char **argv)
{
    (void)argc; (void)argv;
    printf("resetting...\r\n");
    HAL_Delay(20);
    NVIC_SystemReset();
    return 0;
}

static int cmd_pll(int argc, const char **argv)
{
    struct pll_state cfg = *pll_ctrl_get_current();
    int show    = 0;
    int apply   = 0;
    int m_v = (int)cfg.m, n_v = (int)cfg.n, p_v = (int)cfg.p,
        q_v = (int)cfg.q, r_v = (int)cfg.r;

    static const char *usages[] = {
        "pll [--show]",
        "pll [--m N] [--n N] [--p N] [--q N] [--r N] --apply",
        NULL,
    };
    struct argparse_option opts[] = {
        OPT_HELP(),
        OPT_BOOLEAN(0, "show",  &show,  "print current dividers", NULL, 0, 0),
        OPT_BOOLEAN(0, "apply", &apply, "commit dividers to RCC", NULL, 0, 0),
        OPT_INTEGER('m', "m", &m_v, "DIVM1", NULL, 0, 0),
        OPT_INTEGER('n', "n", &n_v, "DIVN1 (VCO multiplier)", NULL, 0, 0),
        OPT_INTEGER('p', "p", &p_v, "DIVP1 (sysclk divider)", NULL, 0, 0),
        OPT_INTEGER('q', "q", &q_v, "DIVQ1", NULL, 0, 0),
        OPT_INTEGER('r', "r", &r_v, "DIVR1 (TRACECLK divider)", NULL, 0, 0),
        OPT_END(),
    };
    struct argparse ap;
    argparse_init(&ap, opts, usages, 0);
    argparse_describe(&ap,
        "\nInspect or reprogram PLL1 at runtime.\n", NULL);
    int rc_parse = argparse_parse(&ap, argc, argv);
    printf("[pll] parse rc=%d  flags: show=%d apply=%d  vals: m=%d n=%d p=%d q=%d r=%d\r\n",
           rc_parse, show, apply, m_v, n_v, p_v, q_v, r_v);
    if (rc_parse < 0) {
        printf("[pll] argparse rejected the command; NOT touching PLL\r\n");
        return 1;
    }

    if (show || !apply) {
        print_pll_state(pll_ctrl_get_current());
        if (!apply) return 0;
    }
    cfg.m = (uint32_t)m_v; cfg.n = (uint32_t)n_v;
    cfg.p = (uint32_t)p_v; cfg.q = (uint32_t)q_v; cfg.r = (uint32_t)r_v;
    printf("[pll] applying M=%d N=%d P=%d Q=%d R=%d ...\r\n",
           m_v, n_v, p_v, q_v, r_v);
    /* Force TX to drain so this line lands even if the reprogram bricks us. */
    while ((s_huart->Instance->ISR & USART_ISR_TC) == 0) { }
    int rc = pll_ctrl_apply(&cfg);
    printf("[pll] pll_ctrl_apply returned %d\r\n", rc);
    if (rc == 0) {
        /* UART kernel is HSI (see cli_init) so baud is unaffected by the
         * reprogram -- no HAL_UART_Init needed here. Read RCC back and
         * compare requested vs actual: RM0433 §8.5.5 lets a busy PLL
         * silently reject divider writes. */
        struct pll_state live;
        pll_ctrl_read(&live);
        live.hse_hz = cfg.hse_hz;
        pll_ctrl_set_current(&live);
        printf("OK  requested M=%d N=%d P=%d Q=%d R=%d\r\n",
               m_v, n_v, p_v, q_v, r_v);
        printf("readback: M=%lu N=%lu P=%lu Q=%lu R=%lu\r\n",
               (unsigned long)live.m, (unsigned long)live.n,
               (unsigned long)live.p, (unsigned long)live.q,
               (unsigned long)live.r);
        if ((int)live.m != m_v || (int)live.n != n_v ||
            (int)live.p != p_v || (int)live.q != q_v || (int)live.r != r_v)
        {
            printf("WARNING: readback differs from request -- divider write "
                   "was rejected by hardware (PLL still enabled? "
                   "sysclk not parked? see RM0433 §8.5.5)\r\n");
        }
        print_pll_state(&live);
        s_etm_dirty = 1;
    } else {
        printf("FAIL rc=%d\r\n", rc);
    }
    return rc;
}

static int cmd_trace(int argc, const char **argv)
{
    int show    = 0;
    int apply   = 0;
    int bb_v      = s_etm.bb;
    int stall_v   = s_etm.stall;
    int systick_v = s_etm.systick;

    static const char *usages[] = {
        "trace [--show]",
        "trace [--bb 0|1] [--stall 0|1] [--systick 0|1] --apply",
        NULL,
    };
    struct argparse_option opts[] = {
        OPT_HELP(),
        OPT_BOOLEAN(0, "show",  &show,  "print current ETM config", NULL, 0, 0),
        OPT_BOOLEAN(0, "apply", &apply, "re-program ETM/TPIU/ETF", NULL, 0, 0),
        OPT_INTEGER(0, "bb",      &bb_v,      "TRCCONFIGR.BB (branch broadcast)",
                    NULL, 0, 0),
        OPT_INTEGER(0, "stall",   &stall_v,   "TRCSTALLCTLR (0/1: off/lossless)",
                    NULL, 0, 0),
        OPT_INTEGER(0, "systick", &systick_v, "SysTick 1kHz interrupt on/off",
                    NULL, 0, 0),
        OPT_END(),
    };
    struct argparse ap;
    argparse_init(&ap, opts, usages, 0);
    argparse_describe(&ap,
        "\nReconfigure the ETMv4 without changing the workload.\n"
        "Effective on the next workload start unless --apply is given.\n", NULL);
    int rc_parse = argparse_parse(&ap, argc, argv);
    printf("[trace] parse rc=%d  show=%d apply=%d bb=%d stall=%d systick=%d\r\n",
           rc_parse, show, apply, bb_v, stall_v, systick_v);
    if (rc_parse < 0) {
        printf("[trace] argparse rejected the command\r\n");
        return 1;
    }

    if (show || !apply) {
        printf("etm: bb=%u stall=%u systick=%u\r\n",
               s_etm.bb, s_etm.stall, s_etm.systick);
        if (!apply) return 0;
    }
    s_etm.bb      = (uint8_t)(bb_v != 0);
    s_etm.stall   = (uint8_t)(stall_v != 0);
    s_etm.systick = (uint8_t)(systick_v != 0);
    s_etm_dirty = 1;
    if (s_workload == WL_SELFTRACE) {
        s_workload_changed = 1;   /* force re-setup on next loop iteration */
    }
    printf("etm queued: bb=%u stall=%u systick=%u  (%s)\r\n",
           s_etm.bb, s_etm.stall, s_etm.systick,
           s_workload == WL_SELFTRACE ? "reapplying" : "pending workload start");
    return 0;
}

static int cmd_cache(int argc, const char **argv)
{
    if (argc < 2) {
        printf("usage: cache on|off\r\n");
        return 1;
    }
    if (strcmp(argv[1], "on") == 0) {
        SCB_EnableICache();
        SCB_EnableDCache();
        printf("I/D cache: ON\r\n");
    } else if (strcmp(argv[1], "off") == 0) {
        SCB_DisableDCache();
        SCB_DisableICache();
        printf("I/D cache: OFF\r\n");
    } else {
        printf("usage: cache on|off\r\n");
        return 1;
    }
    return 0;
}

static int cmd_run(int argc, const char **argv)
{
    if (argc < 2) {
        printf("usage: run selftrace|coremark|idle [--bb N] [--stall N] [--systick N]\r\n");
        return 1;
    }
    enum workload_kind wl;
    if      (strcmp(argv[1], "selftrace") == 0) wl = WL_SELFTRACE;
    else if (strcmp(argv[1], "coremark")  == 0) wl = WL_COREMARK;
    else if (strcmp(argv[1], "idle")      == 0) wl = WL_IDLE;
    else {
        printf("unknown workload `%s`; expected selftrace|coremark|idle\r\n",
               argv[1]);
        return 1;
    }
    /* Optional ETM knobs when starting selftrace. Simple manual parse rather
     * than a nested argparse to keep the code short; only three flags. */
    for (int i = 2; i + 1 < argc; i += 2) {
        int v = atoi(argv[i + 1]);
        if      (strcmp(argv[i], "--bb")      == 0) s_etm.bb      = (uint8_t)!!v;
        else if (strcmp(argv[i], "--stall")   == 0) s_etm.stall   = (uint8_t)!!v;
        else if (strcmp(argv[i], "--systick") == 0) s_etm.systick = (uint8_t)!!v;
    }
    s_workload = wl;
    s_etm_dirty = 1;
    s_workload_changed = 1;
    printf("run: %s  etm: bb=%u stall=%u systick=%u\r\n",
           workload_name(), s_etm.bb, s_etm.stall, s_etm.systick);
    return 0;
}

struct cli_cmd {
    const char *name;
    int (*fn)(int argc, const char **argv);
    const char *summary;
};

static const struct cli_cmd s_cmds[] = {
    { "echo",    cmd_echo,    "print argv verbatim (UART smoke test)" },
    { "ap-test", cmd_ap_test, "argparse smoke test, no side effects" },
    { "id",      cmd_id,      "print firmware + PLL + workload state" },
    { "pll",     cmd_pll,     "inspect / reprogram PLL1" },
    { "trace",   cmd_trace,   "inspect / reprogram ETMv4 (bb/stall/systick)" },
    { "cache",   cmd_cache,   "I+D cache on|off" },
    { "run",     cmd_run,     "switch workload: selftrace|coremark|idle" },
    { "reset",   cmd_reset,   "NVIC_SystemReset()" },
    { NULL, NULL, NULL },
};

static void help_all(void)
{
    printf("commands (use `<cmd> --help` for options):\r\n");
    for (const struct cli_cmd *c = s_cmds; c->name; c++)
        printf("  %-8s %s\r\n", c->name, c->summary);
}

static void execute_line(char *line)
{
    const char *argv[CLI_ARG_MAX];
    int argc = 0;
    char *p = line;
    while (*p && argc < CLI_ARG_MAX) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = '\0';
    }
    if (argc == 0) return;

    if (strcmp(argv[0], "help") == 0 || strcmp(argv[0], "?") == 0) {
        help_all(); return;
    }
    for (const struct cli_cmd *c = s_cmds; c->name; c++) {
        if (strcmp(argv[0], c->name) == 0) { c->fn(argc, argv); return; }
    }
    printf("unknown command: `%s`\r\n", argv[0]);
    help_all();
}

/* --- public API ---------------------------------------------------------- */

void cli_init(UART_HandleTypeDef *huart)
{
    s_huart = huart;
    /* Decouple UART baud from PLL by pointing its kernel at HSI. Without this
     * a `pll --apply` command momentarily changes the UART clock domain and
     * the reply comes out garbled -- confirmed on 2026-09-07. */
    pll_ctrl_uart1_to_hsi(huart);
    /* Enable RX FIFO. CubeMX disables it (MX_USART1_UART_Init runs
     * HAL_UARTEx_DisableFifoMode), which limits RX buffering to a single byte.
     * The selftrace loop only polls between iterations (~tens of us), longer
     * than one UART byte time at 115200 (87us) -- so multi-char commands lose
     * every second/third char to OVERRUN. FIFO on gives us 16-byte cushion. */
    HAL_UARTEx_EnableFifoMode(huart);
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("\r\n[cli] ready. type `help` or `<cmd> --help`.\r\n");
    print_pll_state(pll_ctrl_get_current());
    printf("workload: %s (change with `run selftrace|coremark|idle`)\r\n> ",
           workload_name());
}

void cli_poll(void)
{
    for (;;) {
        int c = uart_getc();
        if (c < 0) return;
        s_rx_bytes++;
        if (c == '\r' || c == '\n') {
            if (s_len == 0) { printf("> "); continue; }
            s_line[s_len] = '\0';
            printf("\r\n");
            execute_line(s_line);
            s_len = 0;
            printf("> ");
            continue;
        }
        if (c == 0x08 || c == 0x7F) {
            if (s_len > 0) { s_len--; printf("\b \b"); }
            continue;
        }
        if (s_len < (int)sizeof(s_line) - 1) {
            s_line[s_len++] = (char)c;
            char ch = (char)c;
            _write(1, &ch, 1);
        }
    }
}

unsigned int cli_rx_count(void)
{
    return s_rx_bytes;
}

/* --- workload scheduler -------------------------------------------------- */

void workload_run(void)
{
    for (;;) {
        s_workload_changed = 0;
        enum workload_kind wl = s_workload;

        switch (wl) {
        case WL_SELFTRACE:
            if (s_etm_dirty) {
                etm_selftrace_setup(&s_etm);
                s_etm_dirty = 0;
            }
            while (!s_workload_changed && s_workload == WL_SELFTRACE) {
                etm_selftrace_iterate();
                cli_poll();
            }
            break;

        case WL_COREMARK:
            /* coremark_main() loops internally; break out to CLI between
             * runs by having it check a public flag would be nice, but
             * CoreMark upstream is untouched. Instead we set a short
             * ITERATIONS at build time so each round returns quickly, and
             * we cli_poll() between rounds via cm_uart_puts side effects.
             * For now: run one round then re-check workload. */
            coremark_main_one();
            cli_poll();
            break;

        case WL_IDLE:
        default:
            while (!s_workload_changed && s_workload == WL_IDLE) {
                cli_poll();
                __asm volatile ("wfi");
            }
            break;
        }
    }
}
