/*
 * etm_selftrace.h -- runtime-configurable ETMv4 trace setup + deterministic
 * self-verifying workload. All knobs that used to be compile switches
 * (ETM_SELFTRACE_SYSTICK, TRACE_BB, TRACE_STALL) live in this struct now, so
 * the UART CLI can flip them without a rebuild.
 */
#ifndef ETM_SELFTRACE_H
#define ETM_SELFTRACE_H

#include <stdint.h>

struct etm_cfg {
    /* ETMv4 TRCCONFIGR.BB (bit 3). Branch broadcast:
     *  0 = only indirect branches emit addresses (decoder walks ELF for direct
     *      branches). LOW byte-rate. Safe at low TRACECLK.
     *  1 = every taken branch emits an address. HIGH byte-rate, dense stream
     *      that beats the halfsync-filler ambiguity, but overwhelms the TPIU
     *      at low TRACECLK. */
    uint8_t bb;
    /* ETMv4 TRCSTALLCTLR:
     *  0 = no stall. Fast, but ETF can overflow -> lossy trace.
     *  1 = ISTALL + LEVEL=max (0x10C). CPU stalls when trace FIFO fills, so
     *      trace is LOSSLESS at the cost of CPU throughput. */
    uint8_t stall;
    /* SysTick interrupt during the loop. 0 = off, 1 = on (exercises ETMv4
     * EXCEPTION / EXCEPTION_RET packets). */
    uint8_t systick;
};

/* Default config: BB=1, STALL=1, SysTick off (matches the pre-CLI hardcoded
 * setup that produced golden traces on 2026-09-04). Kept in sync with the
 * historical bring-up so behaviour is unchanged when the CLI is untouched. */
extern const struct etm_cfg etm_cfg_default;

/* Program ETM/TPIU/CSTF/ETF/GPIO for parallel trace using `cfg`. Called at
 * every workload start and every time the user issues `trace --apply`. */
void etm_selftrace_setup(const struct etm_cfg *cfg);

/* Run ONE iteration of the deterministic workload (det_iter -> 8 * node ->
 * leaf_add + leaf_xor). The scheduler calls this in a tight loop with a
 * cli_poll() between iterations. */
void etm_selftrace_iterate(void);

#endif /* ETM_SELFTRACE_H */
