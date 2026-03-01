/*
 * Originally from Murmulator OS 2 by DnCraptor
 * https://github.com/DnCraptor/murmulator-os2
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "cmd.h"

#define FIRMWARE_MARKER_FN "/.firmware"

void reboot_me(void);
bool load_firmware(char* pathname);
void run_app(char * name);
void link_firmware(FIL* pf, const char* pathname);

bool is_new_app(cmd_ctx_t* ctx);
bool run_new_app(cmd_ctx_t* ctx);
void exec_sync(cmd_ctx_t* ctx);

bool load_app(cmd_ctx_t* ctx);
void exec(cmd_ctx_t* ctx);
void cleanup_bootb_ctx(cmd_ctx_t* ctx);
void flash_block(uint8_t* buffer, size_t flash_target_offset);
size_t free_app_flash(void);

void mallocFailedHandler(size_t);
void overflowHook( TaskHandle_t pxTask, char *pcTaskName );
void vCmdTask(void *pv);
void app_signal(void);
int kill(uint32_t task_number);
void __exit(int status);

/* Launch a standalone ELF app from an SD card path (detached) */
void launch_elf_app(const char *path);

/* Launch an ELF app with a file argument: argc=2, argv[1]=file_path */
void launch_elf_app_with_file(const char *app_path, const char *file_path);

/* Launch an ELF app with multiple file arguments: argc=file_count+1 */
void launch_elf_app_with_files(const char *app_path,
                               const char **files, int file_count);

typedef struct driver_api_s {
    const char* dev_node_name;
    uint32_t driver_version;
    void (*irq_handler)(int irq);
    void* (*open)(void* params);
    int   (*read)(void* h, void* buf, int sz);
    int   (*write)(void* h, const void* buf, int sz);
    int   (*ioctl)(void* h, int cmd, void* arg);
    void (*cleanup)(void);
} driver_api_t;

typedef struct hardware_request_s {
    uint32_t dmas; // DMAs required bitmask
    uint32_t irqs; // IRQs required bitmask
    uint32_t sms; // state machines bitmask
    uint64_t pios; // GPIOs bitmask (0-47 bits for rp2350b, 0-32 - for rp2350a)
    bool relative_pios; // automatic remap pios by OS

    uint8_t  uarts;  // UART controllers
    uint8_t  spis;   // SPI controllers
    uint8_t  i2cs;   // I2C controllers
    uint16_t pwms;   // PWM slices
    uint8_t  adcs;   // ADC inputs
    uint8_t  alarms; // HW alarms
    bool usb;        // USB controller
    bool watchdog;   // watchdog timer
    bool rtc;        // real time clock
    bool crypto;     // crypto accelerator
    bool rng;        // hardware RNG
} hardware_request_t;

bool register_driver(driver_api_t* da, hardware_request_t* hr);
const hardware_request_t* drivers_info(void);

#ifdef __cplusplus
}
#endif
