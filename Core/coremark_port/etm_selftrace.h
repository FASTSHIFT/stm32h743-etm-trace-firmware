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
    /* ETMv4 TRCCONFIGR.TS (bit 11). Global timestamping:
     *  0 = no in-stream timestamps. Time base must come from elsewhere (the
     *      FPGA egress time base -- which measures ETF drain time, NOT
     *      execution time, so it is wrong for fine-grained per-function timing).
     *  1 = the ETM inserts TIMESTAMP packets (48-bit global count from the SoC
     *      timestamp generator) at sync points and around exceptions. These are
     *      anchored at EXECUTION time, so the decoder can time function
     *      entry/exit by "nearest timestamp + instruction-count interpolation".
     * NB (DDI0494D §3.4.7): TRCSTALLCTLR LEVEL!=0 may SUPPRESS in-stream
     * timestamps. If ts=1 does not yield non-zero, increasing timestamps on the
     * board, lower/disable stall and re-check. */
    uint8_t ts;
    /* TRCCONFIGR.CCI (bit 4): cycle counting. When on, the ETM emits Cycle
     * Count elements (exact CPU cycles between commits), giving the decoder
     * CPU-cycle time resolution (~6.7 ns @150 MHz) to interpolate between the
     * sparse global-timestamp anchors -- the real per-function precision lever
     * on this M7 (TRCIDR0.TRCCCI=1). Costs some trace bandwidth. */
    uint8_t cc;
    /* DWT data-value trace on a WRITE to a watched address (nxtrace thread-
     * switch mechanism, docs/01 P0). When on, etm_selftrace_setup also enables
     * the ITM ATB path + funnel extra ports and programs DWT comparator 0 to
     * emit a data-value packet (payload = written value) for each write to
     * `dwt_watch_addr`. 0 = ETM-only (historical behaviour, unchanged). */
    uint8_t dwt;
    /* Address DWT comparator 0 watches for writes. For the P0 ground-truth test
     * this is a firmware global written every iteration; for NuttX it will be
     * &g_running_tasks. Ignored when dwt=0. */
    uint32_t dwt_watch_addr;
};

/* Default config: BB=1, STALL=1, SysTick off, TS off (matches the pre-CLI
 * hardcoded setup that produced golden traces on 2026-09-04). Kept in sync with
 * the historical bring-up so behaviour is unchanged when the CLI is untouched. */
extern const struct etm_cfg etm_cfg_default;

/* Program ETM/TPIU/CSTF/ETF/GPIO for parallel trace using `cfg`. Called at
 * every workload start and every time the user issues `trace --apply`. */
void etm_selftrace_setup(const struct etm_cfg *cfg);

/* Run ONE iteration of the deterministic workload (det_iter -> 8 * node ->
 * leaf_add + leaf_xor). The scheduler calls this in a tight loop with a
 * cli_poll() between iterations. */
void etm_selftrace_iterate(void);

/* P0 DWT ground-truth probe (docs/01 §6). Address of a firmware global that
 * dwt=1 watches by default; etm_dwt_probe_tick() writes a monotonically
 * increasing value to it so each write should emit exactly one DWT data-value
 * packet whose payload equals the value we wrote -- letting us confirm, off
 * the board, that ETM(stream2) and DWT(streamN) co-exist on the parallel TPIU
 * and share the global timestamp, WITHOUT needing NuttX. */
uint32_t etm_dwt_probe_addr(void);
void     etm_dwt_probe_tick(void);

#endif /* ETM_SELFTRACE_H */
