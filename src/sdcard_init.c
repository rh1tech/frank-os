#include "sdcard_init.h"
#include "ff.h"
#include "pico/stdlib.h"
#include <stdio.h>

FATFS fatfs;
static bool mounted = false;
static FRESULT last_mount_result = FR_NOT_READY;

bool sdcard_mount(void) {
    /* Retry a few times — some cards need multiple init attempts.
     * Use longer delays to give the card time to stabilize. */
    FRESULT res = FR_NOT_READY;
    for (int attempt = 0; attempt < 5; attempt++) {
        f_mount(NULL, "", 0);  /* unmount first to reset state */
        res = f_mount(&fatfs, "", 1);
        if (res == FR_OK) break;
        printf("SD mount attempt %d failed: %d\n", attempt + 1, res);
        sleep_ms(500);
    }
    last_mount_result = res;

    if (res != FR_OK) {
        printf("SD card mount failed after retries: %d\n", res);
        mounted = false;
        return false;
    }
    mounted = true;
    printf("SD card mounted\n");

    /* Create /FOS directory if it doesn't exist */
    FILINFO fno;
    if (f_stat("/FOS", &fno) != FR_OK) {
        res = f_mkdir("/FOS");
        if (res == FR_OK) {
            printf("Created /FOS directory\n");
        }
    }

    /* Create /tmp directory if it doesn't exist (needed for ELF loader temp files) */
    if (f_stat("/tmp", &fno) != FR_OK) {
        res = f_mkdir("/tmp");
        if (res == FR_OK) {
            printf("Created /tmp directory\n");
        }
    }

    return true;
}

bool sdcard_is_mounted(void) {
    return mounted;
}

FRESULT sdcard_last_error(void) {
    return last_mount_result;
}
