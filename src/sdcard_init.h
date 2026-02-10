#ifndef SDCARD_INIT_H
#define SDCARD_INIT_H

#include <stdbool.h>
#include "ff.h"

/* Mount SD card and create /FOS directory if needed (retries 3 times) */
bool sdcard_mount(void);

/* Check if SD card is currently mounted */
bool sdcard_is_mounted(void);

/* Return the FRESULT from the last mount attempt */
FRESULT sdcard_last_error(void);

#endif /* SDCARD_INIT_H */
