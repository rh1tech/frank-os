/*
 * FreeRTOS V202212.00
 * Copyright (C) 2020 Amazon.com, Inc. or its affiliates.  All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * https://www.FreeRTOS.org
 * https://github.com/FreeRTOS
 *
 */

/******************************************************************************
 * This project provides two demo applications.  A simple blinky style project,
 * and a more comprehensive test and demo application.  The
 * mainCREATE_SIMPLE_BLINKY_DEMO_ONLY setting (defined in this file) is used to
 * select between the two.  The simply blinky demo is implemented and described
 * in main_blinky.c.  The more comprehensive test and demo application is
 * implemented and described in main_full.c.
 *
 * This file implements the code that is not demo specific, including the
 * hardware setup and standard FreeRTOS hook functions.
 *
 * ENSURE TO READ THE DOCUMENTATION PAGE FOR THIS PORT AND DEMO APPLICATION ON
 * THE http://www.FreeRTOS.org WEB SITE FOR FULL INFORMATION ON USING THIS DEMO
 * APPLICATION, AND ITS ASSOCIATE FreeRTOS ARCHITECTURE PORT!
 *
 */

/* Scheduler include files. */
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

/* Standard demo includes. */
//#include "TimerDemo.h"
//#include "QueueOverwrite.h"
//#include "EventGroupsDemo.h"
//#include "IntSemTest.h"
///#include "TaskNotify.h"

#include "main.h"

/* Library includes. */
#include <stdio.h>
#include "pico/stdlib.h"
#if ( mainRUN_ON_CORE == 1 )
#include "pico/multicore.h"
#endif

#include "hooks.h"
#include "../drivers/psram/psram.h"

/* Deferred PSRAM cleanup queue — filled by task_mem_defer_free() in app.c,
 * drained here in the idle hook after the self-deleting task has switched out. */
#define TASK_MEM_CLEANUP_SLOTS 16
extern void* volatile task_mem_cleanup[TASK_MEM_CLEANUP_SLOTS];

static vApplicationMallocFailedHookPtr _vApplicationMallocFailedHookPtr = NULL;

vApplicationMallocFailedHookPtr getApplicationMallocFailedHookPtr() {
    return _vApplicationMallocFailedHookPtr;
}

void setApplicationMallocFailedHookPtr(vApplicationMallocFailedHookPtr ptr) {
    _vApplicationMallocFailedHookPtr = ptr;
}

/*-----------------------------------------------------------*/
void vApplicationMallocFailedHook( size_t xWantedSize )
{
    /* Called if a call to pvPortMalloc() fails because there is insufficient
    free memory available in the FreeRTOS heap.  pvPortMalloc() is called
    internally by FreeRTOS API functions that create tasks, queues, software
    timers, and semaphores.  The size of the FreeRTOS heap is set by the
    configTOTAL_HEAP_SIZE configuration constant in FreeRTOSConfig.h. */
    if (_vApplicationMallocFailedHookPtr) _vApplicationMallocFailedHookPtr(xWantedSize);
    else {
        // TODO: draw_window
    }
}
/*-----------------------------------------------------------*/
static vApplicationStackOverflowHookPtr _vApplicationStackOverflowHookPtr = NULL;

vApplicationStackOverflowHookPtr getApplicationStackOverflowHookPtr() {
    return _vApplicationStackOverflowHookPtr;
}

void setApplicationStackOverflowHookPtr(vApplicationStackOverflowHookPtr ptr) {
    _vApplicationStackOverflowHookPtr = ptr;
}

void vApplicationStackOverflowHook( TaskHandle_t pxTask, char *pcTaskName )
{
    if (_vApplicationStackOverflowHookPtr) _vApplicationStackOverflowHookPtr(pxTask, pcTaskName);
    else {
        // TODO: draw_window
    }
    /* Run time stack overflow checking is performed if
    configCHECK_FOR_STACK_OVERFLOW is defined to 1 or 2.  This hook
    function is called if a stack overflow is detected. */
}
/*-----------------------------------------------------------*/

void vApplicationIdleHook( void )
{
    volatile size_t xFreeHeapSpace;
  //  draw_text("vApplicationIdleHook", 0, 7, 13, 1);


    /* This is just a trivial example of an idle hook.  It is called on each
    cycle of the idle task.  It must *NOT* attempt to block.  In this case the
    idle task just queries the amount of FreeRTOS heap that remains.  See the
    memory management section on the http://www.FreeRTOS.org web site for memory
    management options.  If there is a lot of heap memory free then the
    configTOTAL_HEAP_SIZE value in FreeRTOSConfig.h can be reduced to free up
    RAM. */
    xFreeHeapSpace = xPortGetFreeHeapSize();

    /* Remove compiler warning about xFreeHeapSpace being set but never used. */
    ( void ) xFreeHeapSpace;

    /* Drain deferred PSRAM cleanup queue (static task stacks/TCBs) */
    for (int i = 0; i < TASK_MEM_CLEANUP_SLOTS; i++) {
        taskENTER_CRITICAL();
        void* p = task_mem_cleanup[i];
        if (p) task_mem_cleanup[i] = NULL;
        taskEXIT_CRITICAL();
        if (p) psram_free(p);
    }
}
/*-----------------------------------------------------------*/
void vApplicationTickHook( void )
{
  //  draw_text("vApplicationTickHook", 0, 8, 13, 1);

#if mainCREATE_SIMPLE_BLINKY_DEMO_ONLY == 0
    {
        /* The full demo includes a software timer demo/test that requires
        prodding periodically from the tick interrupt. */
        #if (mainENABLE_TIMER_DEMO == 1)
        vTimerPeriodicISRTests();
        #endif

        /* Call the periodic queue overwrite from ISR demo. */
        #if (mainENABLE_QUEUE_OVERWRITE == 1)
        vQueueOverwritePeriodicISRDemo();
        #endif

        /* Call the periodic event group from ISR demo. */
        #if (mainENABLE_EVENT_GROUP == 1)
        vPeriodicEventGroupsProcessing();
        #endif

        /* Call the code that uses a mutex from an ISR. */
        #if (mainENABLE_INTERRUPT_SEMAPHORE == 1)
        vInterruptSemaphorePeriodicTest();
        #endif

        /* Call the code that 'gives' a task notification from an ISR. */
        #if (mainENABLE_TASK_NOTIFY == 1)
        xNotifyTaskFromISR();
        #endif
    }
#endif
}

void vApplicationDaemonTaskStartupHook( void ) {
//    draw_text("vApplicationDaemonTaskStartupHook", 0, 9, 13, 1);
}