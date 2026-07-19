/*
 * CoreMark port for STM32H743 (proposal 36).
 * Timing = DWT CYCCNT (CPU cycles); reporting = ee_printf over UART.
 * EE_TICKS_PER_SEC = SystemCoreClock so time_in_secs() is correct at any freq.
 */
#ifndef CORE_PORTME_H
#define CORE_PORTME_H

/* No host libc float/time/stdio on bare metal; CoreMark ships its own ee_printf. */
#ifndef HAS_FLOAT
#define HAS_FLOAT 1
#endif
#ifndef HAS_TIME_H
#define HAS_TIME_H 0
#endif
#ifndef USE_CLOCK
#define USE_CLOCK 0
#endif
#ifndef HAS_STDIO
#define HAS_STDIO 0
#endif
#ifndef HAS_PRINTF
#define HAS_PRINTF 0          /* use CoreMark's ee_printf (barebones/ee_printf.c) */
#endif

/* Compiler/flags string for the report. */
#ifndef COMPILER_VERSION
#define COMPILER_VERSION "arm-none-eabi-gcc " __VERSION__
#endif
#ifndef COMPILER_FLAGS
#define COMPILER_FLAGS FLAGS_STR
#endif
#ifndef MEM_LOCATION
#define MEM_LOCATION "STACK"
#endif

/* Data types (32-bit Cortex-M7). */
typedef signed short   ee_s16;
typedef unsigned short ee_u16;
typedef signed int     ee_s32;
typedef double         ee_f32;
typedef unsigned char  ee_u8;
typedef unsigned int   ee_u32;
typedef ee_u32         ee_ptr_int;
typedef unsigned int   ee_size_t;

#define NULL ((void *)0)

/* align_mem: CoreMark wants 4-byte alignment for its scratch buffers. */
#define align_mem(x) (void *)(4 + (((ee_ptr_int)(x)-1) & ~3))

/* Seed method: static (compile-time) seeds via VALIDATION_RUN/PERFORMANCE_RUN. */
#define SEED_METHOD SEED_VOLATILE
/* Single-threaded, one context. */
#define MEM_METHOD  MEM_STACK
#define MULTITHREAD 1
#define USE_PTHREAD 0
#define USE_FORK    0
#define USE_SOCKET  0

/* Core timing types: use CPU cycle counter (DWT). secs_ret is typedef'd by
 * coremark.h (double when HAS_FLOAT) -- do NOT redefine it here. */
#define CORETIMETYPE ee_u32
#define CORE_TICKS   ee_u32

/* seconds = ticks / EE_TICKS_PER_SEC; ticks are CPU cycles -> per-sec = clock. */
#define EE_TICKS_PER_SEC (SystemCoreClock)

/* CoreMark boilerplate. */
#if (SEED_METHOD == SEED_VOLATILE)
#if (VALIDATION_RUN || PERFORMANCE_RUN || PROFILE_RUN)
#define RUN_TYPE_FLAG 1
#else
#if (TOTAL_DATA_SIZE == 1200)
#define PROFILE_RUN 1
#else
#define PERFORMANCE_RUN 1
#endif
#endif
#endif

typedef struct CORE_PORTABLE_S
{
    ee_u8 portable_id;
} core_portable;

/* Sysclock provided by CMSIS system_stm32h7xx.c */
extern unsigned int SystemCoreClock;

void portable_init(core_portable *p, int *argc, char *argv[]);
void portable_fini(core_portable *p);

#if !defined(PROFILE_RUN) && !defined(PERFORMANCE_RUN) && !defined(VALIDATION_RUN)
#define PERFORMANCE_RUN 1
#endif

#endif /* CORE_PORTME_H */
