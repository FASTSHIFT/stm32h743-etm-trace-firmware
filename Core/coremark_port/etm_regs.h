/*
 * etm_regs.h -- STM32H743 (Cortex-M7) parallel-trace register map.
 *
 * WHY THIS FILE EXISTS
 * --------------------
 * CMSIS (core_cm7.h) only defines the *core-local* CoreSight blocks: TPI_Type,
 * ITM_Type, DWT_Type, CoreDebug_Type. It deliberately does NOT define the
 * ETM-M7 (an out-of-core CoreSight component) nor the STM32H7 SoC-level trace
 * path -- the trace funnel (CSTF), the embedded trace FIFO (ETF), and the SoC
 * TPIU all live on the D-domain APB-D bus at 0x5C0xxxxx aliases, which no ST or
 * ARM header exposes. So the addresses/bits below have no upstream header and
 * must be defined here.
 *
 * Register offsets/bits follow:
 *   - ARM DDI0494D  CoreSight ETM-M7 TRM               (TRC* registers)
 *   - ARM IHI0064   Embedded Trace Macrocell Arch Spec (ETMv4 TRCCONFIGR bits)
 *   - ARM SoC-400   CoreSight TMC/funnel/TPIU layouts  (ETF/CSTF/TPIU)
 *   - ST RM0433 §60 Debug infrastructure               (H7 bus aliases)
 * TRCCONFIGR bit numbers cross-checked against the Linux/perf CoreSight driver
 * (ETM4_CFG_BIT_TS = 11, ETM4_CFG_BIT_BB = 3).
 *
 * Scope is intentionally minimal: only the registers this firmware programs to
 * bring up 4-bit parallel ETM trace. Not a general CoreSight header.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ETM_REGS_H
#define ETM_REGS_H

#include <stdint.h>

/* 32-bit MMIO accessor. */
#define ETM_REG(a) (*(volatile uint32_t *)(uintptr_t)(a))

/* CoreSight software-lock magic: write to a component's LAR (+0xFB0) to unlock
 * its registers for programming (DDI0494D / SoC-400 common lock). */
#define CS_LAR_UNLOCK 0xC5ACCE55u
#define CS_LAR_OFFSET 0xFB0u

/* ======================================================================
 * GPIOE -- trace pins PE2..PE6 = TRACECK / TRACED0..3 (AF0), very-high speed.
 * H7 GPIO port clocks are in RCC_AHB4ENR. (RM0433 memory map.)
 * ==================================================================== */
#define RCC_AHB4ENR      0x580244E0u
#define RCC_AHB4ENR_GPIOEEN (1u << 4)

#define RCC_APB4ENR      0x580244F4u
#define RCC_APB4ENR_SYSCFGEN (1u << 1)

/* SYSCFG I/O compensation cell: needed for VERY_HIGH-speed pads to reach their
 * rated slew (RM0433 §12.3.2). */
#define SYSCFG_CCCSR     0x58000420u
#define SYSCFG_CCCSR_EN    (1u << 0)
#define SYSCFG_CCCSR_READY (1u << 8)

#define GPIOE_MODER      0x58021000u
#define GPIOE_OTYPER     0x58021004u
#define GPIOE_OSPEEDR    0x58021008u
#define GPIOE_AFRL       0x58021020u

/* ======================================================================
 * DEMCR (core PPB) -- global trace enable, common to all Cortex-M.
 * ==================================================================== */
#define DEMCR            0xE000EDFCu
#define DEMCR_TRCENA       (1u << 24)

/* ======================================================================
 * DBGMCU_CR (H7 D-domain, APB-D alias) -- gate the trace clocks.
 * NB: unlike the F4, there is NO TRACE_MODE/TRACE_IOEN field here; port width
 * is set in the TPIU and pins are muxed via GPIO.
 * ==================================================================== */
#define DBGMCU_CR        0x5C001004u
#define DBGMCU_CR_TRACECLKEN (1u << 20)
#define DBGMCU_CR_D1DBGCKEN  (1u << 21)
#define DBGMCU_CR_D3DBGCKEN  (1u << 22)
#define DBGMCU_CR_TRACE_ALL  (DBGMCU_CR_TRACECLKEN | DBGMCU_CR_D1DBGCKEN | DBGMCU_CR_D3DBGCKEN)

/* ======================================================================
 * TPIU (SoC-level, APB-D alias 0x5C015000). The Cortex-M7 core does NOT embed
 * a TPIU (DDI0489F §9.1.1), so this is NOT the CMSIS core-local TPI_BASE.
 * Standard CoreSight SoC-400 TPIU offsets.
 * ==================================================================== */
#define TPIU_BASE        0x5C015000u
#define TPIU_CURPSIZE    (TPIU_BASE + 0x004u) /* current parallel port size */
#define TPIU_SPPR        (TPIU_BASE + 0x0F0u) /* selected pin protocol */
#define TPIU_FFCR        (TPIU_BASE + 0x304u) /* formatter and flush control */
#define TPIU_LAR         (TPIU_BASE + CS_LAR_OFFSET)

#define TPIU_CURPSIZE_4BIT 0x00000008u        /* port size = 4 (bit n-1) */
#define TPIU_SPPR_PARALLEL 0x00000000u        /* pin protocol = parallel */
#define TPIU_FFCR_CONT     0x00000102u        /* EnFCont | EnFTC */

/* ======================================================================
 * CoreSight Trace Funnel (CSTF, APB-D alias 0x5C013000). ETM ATB -> funnel S0.
 * ==================================================================== */
#define CSTF_BASE        0x5C013000u
#define CSTF_CTRL        (CSTF_BASE + 0x000u)
#define CSTF_LAR         (CSTF_BASE + CS_LAR_OFFSET)
#define CSTF_CTRL_ENS0     (1u << 0)          /* enable slave port 0 (ETM) */

/* ======================================================================
 * Embedded Trace FIFO (ETF / CoreSight TMC, APB-D alias 0x5C014000).
 * Topology: ETM -> CSTF -> ETF (4KB) -> TPIU. Put in hardware-FIFO mode so it
 * drains through its ATB master to the TPIU.
 * ==================================================================== */
#define ETF_BASE         0x5C014000u
#define ETF_CTL          (ETF_BASE + 0x020u)  /* TraceCaptEn */
#define ETF_MODE         (ETF_BASE + 0x028u)
#define ETF_FFCR         (ETF_BASE + 0x304u)
#define ETF_LAR          (ETF_BASE + CS_LAR_OFFSET)

#define ETF_CTL_TRACECAPTEN 0x00000001u
#define ETF_MODE_HW_FIFO    0x00000002u       /* 0=circular,1=sw FIFO,2=hw FIFO */
#define ETF_FFCR_ENFT       0x00000001u       /* enable formatting */

/* ======================================================================
 * CoreSight timestamp generator (TSGEN, APB-D alias 0x5C005000).
 * This is the SoC-wide free-running counter whose value the ETM samples into
 * its TIMESTAMP packets. It resets DISABLED (CNTCR=0), so without enabling it
 * every ETM timestamp reads 0 (confirmed on-board: CNTCVL stays 0 until EN=1,
 * then increments freely). Probed component IDs at this base: CIDR1=0xF0
 * (CoreSight class), PIDR=ARM -> the CNTControl frame.
 * Layout: ARM CoreSight SoC-400 TRM (DDI0480), timestamp generator.
 *   CNTCR   (+0x000) counter control: bit0 EN
 *   CNTSR   (+0x004) status
 *   CNTCVL  (+0x008) current count value, low  32 bits
 *   CNTCVU  (+0x00C) current count value, high 32 bits
 *   CNTFID0 (+0x020) base frequency ID (Hz); informational, does not gate count
 * ==================================================================== */
#define TSGEN_BASE       0x5C005000u
#define TSGEN_CNTCR      (TSGEN_BASE + 0x000u)
#define TSGEN_CNTSR      (TSGEN_BASE + 0x004u)
#define TSGEN_CNTCVL     (TSGEN_BASE + 0x008u)
#define TSGEN_CNTCVU     (TSGEN_BASE + 0x00Cu)
#define TSGEN_CNTFID0    (TSGEN_BASE + 0x020u)
#define TSGEN_CNTCR_EN     (1u << 0)

/* ======================================================================
 * ETM-M7 (ETMv4, core-local 0xE0041000). Register offsets: DDI0494D.
 * ==================================================================== */
#define ETM_BASE         0xE0041000u
#define ETM_TRCPRGCTLR   (ETM_BASE + 0x004u)  /* programming control (EN) */
#define ETM_TRCSTATR     (ETM_BASE + 0x00Cu)  /* status (IDLE/PMSTABLE) */
#define ETM_TRCCONFIGR   (ETM_BASE + 0x010u)  /* trace config (BB/TS/CCI/...) */
#define ETM_TRCSTALLCTLR (ETM_BASE + 0x02Cu)  /* stall control */
#define ETM_TRCSYNCPR    (ETM_BASE + 0x034u)  /* sync period */
#define ETM_TRCTRACEIDR  (ETM_BASE + 0x040u)  /* ATB trace ID */
#define ETM_TRCVICTLR    (ETM_BASE + 0x080u)  /* ViewInst main control */
#define ETM_TRCPDCR      (ETM_BASE + 0x310u)  /* power-down control */
#define ETM_LAR          (ETM_BASE + CS_LAR_OFFSET)

/* TRCPRGCTLR */
#define ETM_TRCPRGCTLR_EN   (1u << 0)

/* TRCSTATR */
#define ETM_TRCSTATR_IDLE     (1u << 0)
#define ETM_TRCSTATR_PMSTABLE (1u << 1)

/* TRCCONFIGR bits (ETMv4, IHI0064; cross-checked vs Linux perf ETM4_CFG_BIT_*).
 *   BB  = branch broadcast          (bit 3)
 *   CCI = cycle-counting instr      (bit 4)
 *   TS  = global timestamping       (bit 11)
 *   RS  = return stack              (bit 12) */
#define ETM_TRCCONFIGR_BB   (1u << 3)
#define ETM_TRCCONFIGR_CCI  (1u << 4)
#define ETM_TRCCONFIGR_TS   (1u << 11)
#define ETM_TRCCONFIGR_RS   (1u << 12)

/* TRCSTALLCTLR
 *   ISTALL (bit 8) = allow the unit to stall the CPU on low trace buffer
 *   LEVEL  (bits 2:1 on M7) = invasion level; 11b = max invasion.
 * 0x10C = ISTALL | LEVEL=max (historical lossless setting).
 * NB (DDI0494D §3.4.7): LEVEL != 0 may SUPPRESS in-stream global timestamps. */
#define ETM_TRCSTALLCTLR_ISTALL   (1u << 8)
#define ETM_TRCSTALLCTLR_LEVEL_MAX 0x0000000Cu
#define ETM_TRCSTALLCTLR_LOSSLESS  (ETM_TRCSTALLCTLR_ISTALL | ETM_TRCSTALLCTLR_LEVEL_MAX)

/* TRCVICTLR: trace ALL instructions.
 *   SSSTATUS (bit 9) = start/stop logic STARTED
 *   SEL[3:0] = 1     = architectural constant-TRUE resource
 * => ViewInst always TRUE = trace-all. */
#define ETM_TRCVICTLR_TRACE_ALL   0x00000201u

/* TRCTRACEIDR: ATB ID = 2 (matches host demux). */
#define ETM_TRCTRACEIDR_ID2       0x00000002u

/* TRCSYNCPR: periodic synchronization period = 2^N bytes of trace. This is the
 * A-sync/trace-info period; board-measured it does NOT change the TIMESTAMP
 * rate on this M7 (TS is driven by TRCTSCTLR/counter, not the sync period). */
#define ETM_TRCSYNCPR_256         0x00000008u /* 2^8  */
#define ETM_TRCSYNCPR_1K          0x0000000Au /* 2^10 */
#define ETM_TRCSYNCPR_4K          0x0000000Cu /* 2^12 */

/* ---- Timestamp-rate control (TRCTSCTLR) -----------------------------------
 * The ETM TIMESTAMP rate is controlled by TRCTSCTLR.EVENT (a resource
 * selector), NOT by TRCSYNCPR. The usual way to make it periodic is an ETMv4
 * counter in self-reload mode whose "counter-at-zero" drives the TS event.
 *
 * DEAD END ON THIS PART: the M7 in instruction-only configuration exposes a
 * counter (TRCIDR5.NUMCNTR=1) and its reload register (TRCCNTRLDVR0), but its
 * control and value registers (TRCCNTCTLR0/TRCCNTVR0) are RAZ/WI here
 * (DDI0494D Table 3-1 note a: those exist only in the instruction+data
 * configuration). Board-confirmed: TRCCNTCTLR0 stays 0, the counter never
 * counts, and wiring TRCTSCTLR to counter-at-zero leaves that resource
 * permanently active and corrupts the trace (~2.17M dropped calls). So we keep
 * TRCTSCTLR = 0 (implicit TS anchors, ~105 us apart) and interpolate on the
 * host. The register addresses are kept for reference / a future instr+data
 * part. */
#define ETM_TRCTSCTLR    (ETM_BASE + 0x030u)  /* global timestamp control */

/* ---- Cycle counting (the real per-function precision lever) ---------------
 * TRCCONFIGR.CCI (bit 4) enables cycle counting; TRCCCCTLR sets the threshold.
 * When enabled, the ETM emits Cycle Count elements giving the exact number of
 * processor cycles between commits, so the decoder can time instruction ranges
 * to CPU-cycle resolution (150 MHz -> 6.7 ns) and INTERPOLATE between the
 * sparse global-timestamp anchors with real cycle counts, not a uniform-rate
 * guess. Cortex-M7 implements it (TRCIDR0.TRCCCI=1), unlike the M4.
 *
 * A Cycle Count element is only emitted when the accumulated count reaches the
 * TRCCCCTLR threshold (>= TRCIDR3.CCITMIN, board-read = 4). A smaller threshold
 * = denser cycle counts = finer time but more bandwidth. TRCCCCTLR is RW here
 * (board-confirmed: wrote 0x10, read back 0x10). */
#define ETM_TRCCCCTLR    (ETM_BASE + 0x038u)  /* cycle count threshold */
#define ETM_TRCCCCTLR_THRESHOLD 0x00000010u   /* 16 cycles (>= CCITMIN=4) */

/* TRCPDCR: PU (bit 3) = power-up request. Without it the ETM stays power-gated
 * (TRCSTATR.PMSTABLE never sets) even though other registers read back fine. */
#define ETM_TRCPDCR_PU            (1u << 3)

/* ======================================================================
 * SysTick (core PPB) -- optional exception source during the workload.
 * ==================================================================== */
#define SYST_CSR         0xE000E010u
#define SYST_RVR         0xE000E014u
#define SYST_CVR         0xE000E018u
#define SYST_CSR_ENABLE    (1u << 0)
#define SYST_CSR_TICKINT   (1u << 1)
#define SYST_CSR_CLKSOURCE (1u << 2)
#define SYST_CSR_RUN       (SYST_CSR_ENABLE | SYST_CSR_TICKINT | SYST_CSR_CLKSOURCE)

/* SCB ICSR: PENDSTCLR (bit 25) clears a latched-pending SysTick. */
#define SCB_ICSR         0xE000ED04u
#define SCB_ICSR_PENDSTCLR (1u << 25)

#endif /* ETM_REGS_H */
