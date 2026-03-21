/*
 * FRANK OS
 * Copyright (c) 2025 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/* RP2350 single-core FreeRTOS config (Core 0 only, Core 1 is bare-metal HDMI) */

/* Scheduler */
#define configUSE_PREEMPTION                    1
#define configUSE_TIME_SLICING                  1
#define configNUMBER_OF_CORES                   1
#define configTICK_RATE_HZ                      ((TickType_t)1000)
#define configMAX_PRIORITIES                    8
#define configMINIMAL_STACK_SIZE                ((configSTACK_DEPTH_TYPE)256)
#define configUSE_16_BIT_TICKS                  0
#define configIDLE_SHOULD_YIELD                 1

/* Memory */
#define configSUPPORT_DYNAMIC_ALLOCATION        1
#define configSUPPORT_STATIC_ALLOCATION         1
#define configTOTAL_HEAP_SIZE                   ((size_t)(135 * 1024))
#define configAPPLICATION_ALLOCATED_HEAP        0

/* CPU */
#define configCPU_CLOCK_HZ                      504000000
#define configMAX_SYSCALL_INTERRUPT_PRIORITY     16  /* priority 1 shifted left by 4 for Cortex-M33 */
#define configKERNEL_INTERRUPT_PRIORITY          (255)

/* Hooks */
#define configUSE_IDLE_HOOK                     1
#define configUSE_TICK_HOOK                     0
#define configUSE_MALLOC_FAILED_HOOK            0
#define configCHECK_FOR_STACK_OVERFLOW          2

/* Features */
#define configUSE_MUTEXES                       1
#define configUSE_COUNTING_SEMAPHORES           1
#define configUSE_RECURSIVE_MUTEXES             1
#define configUSE_QUEUE_SETS                    0
#define configUSE_TASK_NOTIFICATIONS            1
#define configUSE_TRACE_FACILITY                1
#define configUSE_STATS_FORMATTING_FUNCTIONS    0

/* Task */
#define configMAX_TASK_NAME_LEN                 16
#define configUSE_CO_ROUTINES                   0
#define configNUM_THREAD_LOCAL_STORAGE_POINTERS 3

/* Timer */
#define configUSE_TIMERS                        1
#define configTIMER_TASK_PRIORITY               (configMAX_PRIORITIES - 1)
#define configTIMER_QUEUE_LENGTH                10
#define configTIMER_TASK_STACK_DEPTH            configMINIMAL_STACK_SIZE

/* Assert — blink LED (GPIO 25) on failure using RP2350 SIO registers.
 * SIO base = 0xd0000000. Long delay (~1s at 252MHz) for visible blink. */
#define configASSERT(x) do { if (!(x)) { \
    *(volatile unsigned int *)0xd0000038 = (1u << 25); /* SIO GPIO_OE_SET */ \
    for (;;) { \
        *(volatile unsigned int *)0xd0000018 = (1u << 25); /* SIO GPIO_OUT_SET */ \
        for (volatile int _i=0;_i<30000000;_i++); \
        *(volatile unsigned int *)0xd0000020 = (1u << 25); /* SIO GPIO_OUT_CLR */ \
        for (volatile int _i=0;_i<30000000;_i++); \
    } \
} } while (0)

/* ARM Cortex-M33 specific (per RP2350 NTZ port README) */
#define configENABLE_MPU                        0
#define configENABLE_FPU                        1
#define configENABLE_TRUSTZONE                  0
#define configRUN_FREERTOS_SECURE_ONLY          1


/* INCLUDE options */
#define INCLUDE_vTaskPrioritySet                1
#define INCLUDE_uxTaskPriorityGet               1
#define INCLUDE_vTaskDelete                     1
#define INCLUDE_vTaskSuspend                    1
#define INCLUDE_vTaskDelayUntil                 1
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_xTaskGetSchedulerState          1
#define INCLUDE_xTaskGetCurrentTaskHandle       1
#define INCLUDE_xTimerPendFunctionCall          1
#define INCLUDE_xTaskResumeFromISR              1
#define INCLUDE_xTaskAbortDelay                 1
#define INCLUDE_xTaskGetHandle                  1
#define INCLUDE_uxTaskGetStackHighWaterMark     1
#define INCLUDE_eTaskGetState                   1

/* RP2350 port: use static exception handler linking (weak symbol override)
 * instead of runtime exception_set_exclusive_handler which can hard_assert
 * if handler slots are already claimed. */
#define configUSE_DYNAMIC_EXCEPTION_HANDLERS   0

/* RP2040/RP2350 port interop with Pico SDK */
/* DIAGNOSTIC: disabled to isolate USB serial issue */
#define configSUPPORT_PICO_SYNC_INTEROP        0
#define configSUPPORT_PICO_TIME_INTEROP        0

#endif /* FREERTOS_CONFIG_H */
