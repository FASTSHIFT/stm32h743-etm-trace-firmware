/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "coremark_port.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
/* PLL1 dividers, overridable at build time via -DPLL_[MNPQR]_OVR=... (see
 * build_h743.sh). Kept in the USER region so CubeMX regeneration will NOT wipe
 * them. HSE is 25 MHz on this board (NOT 8 MHz).
 *   ref  = HSE / M          (PLL input, must be 4..16 MHz for RANGE used)
 *   VCO  = ref * N          (192..836 MHz wide mode)
 *   SYSCLK   = VCO / P
 *   TRACECLK = pll1_r_ck = VCO / R
 * Default M=2 N=32 P=2 R=4 with HSE=25M -> ref=12.5M, VCO=400M,
 *   SYSCLK=200M, HCLK=SYSCLK/2=100M, TRACECLK=100M. */
#ifndef PLL_M_OVR
#define PLL_M_OVR 2
#endif
#ifndef PLL_N_OVR
#define PLL_N_OVR 32
#endif
#ifndef PLL_P_OVR
#define PLL_P_OVR 2
#endif
#ifndef PLL_Q_OVR
#define PLL_Q_OVR 2
#endif
#ifndef PLL_R_OVR
#define PLL_R_OVR 4
#endif
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

UART_HandleTypeDef huart1;

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_USART1_UART_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
/* func_test: a call-graph workload for ETM trace bring-up. Every function is
 * kept as a real BL/BLX (build with -O0 and marked noinline) so branch
 * broadcast produces a dense, decodable stream. NB: with -Og/-O2 the leaf
 * helpers get inlined away, which starves the decoder of branch anchors --
 * that is why the high-TRACECLK capture looked mostly like HSYNC filler. */

#define NOINLINE __attribute__((noinline))

static volatile uint32_t sink;       /* defeat dead-code elimination */
static volatile uint32_t isr_count;  /* interrupt counter */

/* ---- leaf functions (return via BX LR, no stack frame) ---- */

static NOINLINE uint32_t leaf_add(uint32_t a, uint32_t b) {
    return a + b;  /* BX LR */
}

static NOINLINE uint32_t leaf_mul(uint32_t a, uint32_t b) {
    return a * b;  /* BX LR */
}

/* ---- callback invoked from a periodic timer ISR ---- */

void timer_isr_callback(void) {
    /* a light but branchy ISR: nested call + conditional branch */
    isr_count++;
    if (isr_count & 1) {
        sink += leaf_add(isr_count, 3);
    } else {
        sink += leaf_mul(isr_count, 2);
    }
}

/* ---- frame function (returns via POP {PC}, has a stack frame) ---- */

static NOINLINE uint32_t frame_func(uint32_t a, uint32_t b, uint32_t c) {
    volatile uint32_t local = a * b + c;
    return local + leaf_add(a, c);  /* nested leaf call */
}

/* ---- direct call chain A -> B -> C ---- */

static NOINLINE uint32_t level_c(uint32_t x) {
    return leaf_mul(x, 2);
}

static NOINLINE uint32_t level_b(uint32_t x) {
    return level_c(x + 1) + leaf_add(x, 1);
}

static NOINLINE uint32_t level_a(uint32_t x) {
    return level_b(x + 1) + frame_func(x, 2, 3);
}

/* ---- indirect calls through a function pointer (BLX) ---- */

typedef uint32_t (*op_fn)(uint32_t, uint32_t);

static NOINLINE uint32_t op_add(uint32_t a, uint32_t b) { return a + b; }
static NOINLINE uint32_t op_sub(uint32_t a, uint32_t b) { return a - b; }
static NOINLINE uint32_t op_mul(uint32_t a, uint32_t b) { return a * b; }

static const op_fn op_table[3] = { op_add, op_sub, op_mul };

static NOINLINE uint32_t indirect_caller(uint32_t idx, uint32_t a, uint32_t b) {
    op_fn fn = op_table[idx % 3];  /* function pointer table lookup */
    return fn(a, b);  /* BLX indirect call */
}

/* ---- callback pattern (models an LVGL-style event callback) ---- */

typedef void (*callback_t)(uint32_t);

static NOINLINE void cb_handler_a(uint32_t arg) {
    sink += arg;
}

static NOINLINE void cb_handler_b(uint32_t arg) {
    sink += arg * 2;
}

static NOINLINE void dispatch_callback(callback_t cb, uint32_t arg) {
    cb(arg);  /* BLX indirect call to the callback */
}

static NOINLINE void callback_test(void) {
    dispatch_callback(cb_handler_a, 1);
    dispatch_callback(cb_handler_b, 2);
    dispatch_callback(cb_handler_a, 3);
}

/* ---- recursion ---- */

static NOINLINE uint32_t factorial(uint32_t n) {
    if (n <= 1) {
        return 1;
    }
    return n * factorial(n - 1);  /* recursive call */
}

/* ---- deep nesting (6 levels) ---- */

static NOINLINE void deep6(void) {  sink++; }
static NOINLINE void deep5(void) {  deep6(); }
static NOINLINE void deep4(void) {  deep5(); }
static NOINLINE void deep3(void) {  deep4(); }
static NOINLINE void deep2(void) {  deep3(); }
static NOINLINE void deep1(void) {  deep2(); }

/* ---- repeated calls to the same function (checks stack balance) ---- */

static NOINLINE uint32_t pingpong(uint32_t n) {
    return leaf_add(n, 1);
}

static NOINLINE void repeat_test(void) {
    for (int i = 0; i < 5; i++) {
        pingpong(i);  /* same target 5x -> a B/E atom each time */
    }
}

/* ---- conditional branch (checks B / !B atom decoding) ---- */

static NOINLINE uint32_t conditional(uint32_t x) {
    if (x & 1) {
        return leaf_add(x, 10);
    } else {
        return leaf_mul(x, 10);
    }
}

/* ---- mixed: direct + indirect + recursion + callback ---- */

static NOINLINE uint32_t mixed_test(uint32_t x) {
    uint32_t r1 = level_a(x);               /* direct call chain */
    uint32_t r2 = indirect_caller(x, 3, 4); /* indirect call */
    uint32_t r3 = factorial(3);             /* recursion */
    return r1 + r2 + r3;
}

/* ---- main loop (tight, no delay, high trace density) ----
 * REPS batches the whole test suite so each capture window sees many
 * iterations back-to-back, keeping the parallel trace port fed even at high
 * TRACECLK (avoids the HSYNC-filler / anchor-starvation regime).
 *
 * REPS is large and the hot loop has NO HAL_GetTick()/LED polling: at high
 * TRACECLK the ETF (4KB) drains faster than a light workload emits trace, so
 * the TPIU pads with full-sync frames that shred ETM frame phase. A dense,
 * uninterrupted branch stream keeps the ETF fed so real ETM dominates the
 * port instead of sync filler. The heartbeat was removed from the hot path
 * because HAL_GetTick polling injects idle gaps + pulls HAL code into trace. */
#define REPS 200

void main_loop(void) {
    volatile uint32_t acc = 0;
    while (1) {
        for (int r = 0; r < REPS; r++) {
            /* Test 1: direct call chain */
            acc += level_a(r);

            /* Test 2: indirect calls (all 3 operators) */
            acc += indirect_caller(0, r, 20);
            acc += indirect_caller(1, r, 20);
            acc += indirect_caller(2, r, 20);

            /* Test 3: callback pattern */
            callback_test();

            /* Test 4: recursion (varying depth for more branches) */
            acc += factorial(1 + (r & 7));

            /* Test 5: deep nesting */
            deep1();

            /* Test 6: repeated calls */
            repeat_test();

            /* Test 7: conditional branches (both arms, data-dependent) */
            acc += conditional(r);
            acc += conditional(r + 1);

            /* Test 8: mixed */
            acc += mixed_test(r & 3);
        }
        sink = acc;   /* keep acc live so nothing is optimised out */
    }
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{
  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_USART1_UART_Init();
  /* USER CODE BEGIN 2 */
  /* proposal 36 stage 0: run CoreMark instead of func_test.
   * board_clock_override() re-programs PLL to 150M sysclk / 112.5M TRACECLK
   * (survives CubeMX regen); cm_uart_init registers USART1 (PA9/PA10) for
   * ee_printf; coremark_main() runs the benchmark, prints the score, idles. */
  board_clock_override();
  cm_uart_init(&huart1);
  /* main_loop();  // old func_test workload (unused now) */
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* Run CoreMark continuously so the trace stream always contains a full
     * round to capture (ITERATIONS kept small so one round fits in a small
     * slice). Was a single coremark_main() then idle -- too short/ill-timed to
     * catch a complete round on a saturated stream. */
    coremark_main();
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Supply configuration update enable
  */
  HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY);
  /** Configure the main internal regulator output voltage
  */
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE3);

  while(!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}
  /** Macro to configure the PLL clock source
  */
  __HAL_RCC_PLL_PLLSOURCE_CONFIG(RCC_PLLSOURCE_HSE);
  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 2;
  RCC_OscInitStruct.PLL.PLLN = 32;
  RCC_OscInitStruct.PLL.PLLP = 2;
  RCC_OscInitStruct.PLL.PLLQ = 2;
  RCC_OscInitStruct.PLL.PLLR = 4;
  RCC_OscInitStruct.PLL.PLLRGE = RCC_PLL1VCIRANGE_3;
  RCC_OscInitStruct.PLL.PLLVCOSEL = RCC_PLL1VCOWIDE;
  RCC_OscInitStruct.PLL.PLLFRACN = 0;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }
  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2
                              |RCC_CLOCKTYPE_D3PCLK1|RCC_CLOCKTYPE_D1PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.SYSCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB3CLKDivider = RCC_APB3_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV1;
  RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief USART1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART1_UART_Init(void)
{

  /* USER CODE BEGIN USART1_Init 0 */

  /* USER CODE END USART1_Init 0 */

  /* USER CODE BEGIN USART1_Init 1 */

  /* USER CODE END USART1_Init 1 */
  huart1.Instance = USART1;
  huart1.Init.BaudRate = 115200;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  huart1.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart1.Init.ClockPrescaler = UART_PRESCALER_DIV1;
  huart1.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_SetTxFifoThreshold(&huart1, UART_TXFIFO_THRESHOLD_1_8) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_SetRxFifoThreshold(&huart1, UART_RXFIFO_THRESHOLD_1_8) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_DisableFifoMode(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART1_Init 2 */

  /* USER CODE END USART1_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOE_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOG_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin : LED_Pin */
  GPIO_InitStruct.Pin = LED_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  HAL_GPIO_Init(LED_GPIO_Port, &GPIO_InitStruct);

}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}

#ifdef  USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */

