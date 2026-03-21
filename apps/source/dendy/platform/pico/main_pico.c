/*
 * MurmNES - NES Emulator for RP2350
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech | https://github.com/rh1tech/murmnes
 * SPDX-License-Identifier: MIT
 */

#include "pico_hdmi/hstx_data_island_queue.h"
#include "pico_hdmi/hstx_packet.h"
#include "pico_hdmi/video_output.h"

#include "pico/multicore.h"
#include "pico/stdlib.h"
#if !USB_HID_ENABLED
#include "pico/stdio_usb.h"
#endif

#include "hardware/clocks.h"
#include "hardware/pll.h"
#include "hardware/vreg.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/xip_ctrl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board_config.h"
#include "quicknes.h"
#include "psram_init.h"
#include "nespad.h"
#include "ps2kbd_wrapper.h"
#include "settings.h"
#include "rom_selector.h"
#include "i2s_audio.h"
#include "uart_logging.h"
#include "ff.h"
#include "hardware/spi.h"
#include "hardware/dma.h"
#include "hardware/resets.h"
#include "hardware/xip_cache.h"

#if USB_HID_ENABLED
#include "usbhid.h"
#endif

/* 16KB stack in main SRAM — scratch_y (4KB) is too small for QuickNES */
static uint8_t big_stack[16384] __attribute__((aligned(8)));
static void real_main(void);

#define FRAME_WIDTH 640
#define FRAME_HEIGHT 480

#define NES_WIDTH 256
#define NES_HEIGHT 240
#define SAMPLE_RATE 44100


/* ROM embedded in flash by CMake (objcopy) */
#ifdef HAS_NES_ROM
extern const uint8_t nes_rom_data[];
extern const uint8_t nes_rom_end[];
#endif

/* Palette lookup: NES indexed pixel -> doubled RGB565 (two pixels per word).
 * Pre-doubled eliminates shift+OR in the scanline callback hot loop,
 * reducing SRAM accesses and keeping the DMA ISR within timing budget. */
uint32_t rgb565_palette_32[2][256];
int pal_write_idx = 0;
static volatile int pal_read_idx = 0;
volatile int pending_pal_idx = -1;

/* Pointer to current frame pixels — only updated during vblank by vsync_cb */
static const uint8_t *frame_pixels;
static long frame_pitch;

/* Pending frame update: Core 0 writes here after emulation, Core 1 applies
   it during the next vsync callback (vblank). Ensures the pointer never
   changes while active scanlines are being displayed. */
volatile const uint8_t *pending_pixels;
volatile long pending_pitch;

/* Vsync flag — set by Core 1 DMA ISR, cleared by Core 0 after emulating */
volatile uint32_t vsync_flag;

static void __not_in_flash("vsync") vsync_cb(void)
{
    /* Apply pending frame pointer during vblank — safe from tearing */
    const uint8_t *pp = (const uint8_t *)pending_pixels;
    if (pp) {
        frame_pixels = pp;
        frame_pitch = pending_pitch;
        pending_pixels = NULL;
        if (pending_pal_idx != -1) {
            pal_read_idx = pending_pal_idx;
            pending_pal_idx = -1;
        }
    }
    vsync_flag = 1;
    __sev(); /* wake Core 0 from WFE */
}

/*
 * Audio pipeline — same architecture as pico-infonesPlus:
 *   Core 0: encode NES audio → push pre-encoded packets to DI queue
 *   Core 1 ISR: pop from DI queue → HDMI output
 *   One shared queue. No intermediate buffers. No background task.
 *
 * All encoding functions are __not_in_flash (SRAM) so Core 0 can
 * encode without flash contention after running QuickNES from flash.
 * DI queue (512 entries = ~43ms) survives the ~10ms emulation gap.
 */
static int audio_frame_counter = 0;

/* ─── HDMI audio: encode mono NES samples into Data Island packets ─── */
static int16_t audio_carry[3];
static int audio_carry_count = 0;

static void __not_in_flash("audio") hdmi_push_samples(const int16_t *buf, int count)
{
    int16_t merged[4];
    int pos = 0;

    if (audio_carry_count > 0) {
        for (int i = 0; i < audio_carry_count; i++)
            merged[i] = audio_carry[i];
        int need = 4 - audio_carry_count;
        if (need > count) need = count;
        for (int i = 0; i < need; i++)
            merged[audio_carry_count + i] = buf[i];
        pos = need;
        if (audio_carry_count + need == 4) {
            audio_sample_t samples[4];
            for (int i = 0; i < 4; i++) {
                samples[i].left = merged[i];
                samples[i].right = merged[i];
            }
            hstx_packet_t packet;
            int new_fc = hstx_packet_set_audio_samples(&packet, samples, 4, audio_frame_counter);
            hstx_data_island_t island;
            hstx_encode_data_island(&island, &packet, false, true);
            if (!hstx_di_queue_push(&island)) {
                return;
            }
            audio_frame_counter = new_fc;
        }
        audio_carry_count = 0;
    }

    while (pos + 4 <= count) {
        audio_sample_t samples[4];
        for (int i = 0; i < 4; i++) {
            samples[i].left = buf[pos + i];
            samples[i].right = buf[pos + i];
        }
        hstx_packet_t packet;
        int new_fc = hstx_packet_set_audio_samples(&packet, samples, 4, audio_frame_counter);
        hstx_data_island_t island;
        hstx_encode_data_island(&island, &packet, false, true);
        if (!hstx_di_queue_push(&island)) {
            break;
        }
        audio_frame_counter = new_fc;
        pos += 4;
    }

    audio_carry_count = count - pos;
    if (audio_carry_count > 3) {
        audio_carry_count = count % 4;
        pos = count - audio_carry_count;
    }
    for (int i = 0; i < audio_carry_count; i++)
        audio_carry[i] = buf[pos + i];
    hstx_di_queue_update_silence(audio_frame_counter);
}

static void hdmi_fill_silence(int count)
{
    for (int i = 0; i < count / 4; i++) {
        audio_sample_t samples[4] = {0};
        hstx_packet_t packet;
        audio_frame_counter = hstx_packet_set_audio_samples(&packet, samples, 4, audio_frame_counter);
        hstx_data_island_t island;
        hstx_encode_data_island(&island, &packet, false, true);
        hstx_di_queue_push(&island);
    }
}

/* ─── Audio routing: HDMI or I2S based on settings ────────────────── */
static bool i2s_initialized = false;

static void ensure_i2s_initialized(void) {
    if (!i2s_initialized) {
        i2s_audio_init(I2S_DATA_PIN, I2S_CLOCK_PIN_BASE, SAMPLE_RATE);
        i2s_initialized = true;
    }
}

/* Apply volume: scale 16-bit samples by g_settings.volume (0-100) */
static int16_t volume_buf[1024];

static const int16_t *apply_volume(const int16_t *buf, int count) {
    if (g_settings.volume >= 100) return buf;
    if (count > 1024) count = 1024;
    int vol = g_settings.volume;
    for (int i = 0; i < count; i++)
        volume_buf[i] = (int16_t)((buf[i] * vol) / 100);
    return volume_buf;
}

static void __not_in_flash("audio") audio_push_samples(const int16_t *buf, int count)
{
    if (g_settings.audio_mode == AUDIO_MODE_DISABLED) {
        hdmi_fill_silence(count);
        return;
    }
    const int16_t *out = (g_settings.volume < 100) ? apply_volume(buf, count) : buf;
    if (g_settings.audio_mode == AUDIO_MODE_I2S) {
        ensure_i2s_initialized();
        i2s_audio_push_samples(out, count);
        hdmi_fill_silence(count);
    } else {
        hdmi_push_samples(out, count);
    }
}

void audio_fill_silence(int count)
{
    if (g_settings.audio_mode == AUDIO_MODE_I2S) {
        ensure_i2s_initialized();
        i2s_audio_fill_silence(count);
    }
    hdmi_fill_silence(count);
}

/* Build RGB565 palette from QuickNES frame palette + color table */
static void update_palette(int buf_idx)
{
    int pal_size = 0;
    const int16_t *pal = qnes_get_palette(&pal_size);
    const qnes_rgb_t *colors = qnes_get_color_table();

    if (!pal || !colors)
        return;

    for (int i = 0; i < pal_size && i < 256; i++) {
        int idx = pal[i];
        if (idx < 0 || idx >= 512)
            idx = 0x0F; /* black */
        const qnes_rgb_t *c = &colors[idx];
        uint16_t c16 = ((c->r & 0xF8) << 8) | ((c->g & 0xFC) << 3) | (c->b >> 3);
        rgb565_palette_32[buf_idx][i] = c16 | ((uint32_t)c16 << 16);
    }
}

/* Scanline callback: convert indexed pixels to RGB565, doubled to 640x480
 * Runs on Core 1 DMA ISR — must be in RAM, no flash access */
void __not_in_flash("scanline") scanline_callback(
    uint32_t v_scanline, uint32_t active_line, uint32_t *dst)
{
    (void)v_scanline;

    uint32_t nes_line = active_line < 480 ? active_line / 2 : 0;

    if (nes_line < 8 || nes_line >= NES_HEIGHT - 8) {
        for (int i = 32; i < 288; i++)
            dst[i] = 0;
        return;
    }

    const uint8_t *src = frame_pixels + nes_line * frame_pitch;
    uint32_t *out = dst + 32;

    out[0] = 0; out[1] = 0; out[2] = 0; out[3] = 0;
    out[4] = 0; out[5] = 0; out[6] = 0; out[7] = 0;
    out += 8;

    for (int x = 8; x < NES_WIDTH - 8; x += 4) {
        uint32_t p = *(const uint32_t *)(src + x);
        out[0] = rgb565_palette_32[pal_read_idx][p & 0xFF];
        out[1] = rgb565_palette_32[pal_read_idx][(p >> 8) & 0xFF];
        out[2] = rgb565_palette_32[pal_read_idx][(p >> 16) & 0xFF];
        out[3] = rgb565_palette_32[pal_read_idx][(p >> 24)];
        out += 4;
    }

    out[0] = 0; out[1] = 0; out[2] = 0; out[3] = 0;
    out[4] = 0; out[5] = 0; out[6] = 0; out[7] = 0;
}

/* Pixel buffer — used by settings menu for rendering */
uint8_t test_pixels[NES_WIDTH * NES_HEIGHT];

/* Try to initialize PSRAM, returns true on success */
static bool psram_available = false;
#define PSRAM_BASE ((volatile uint8_t *)0x11000000)

static bool try_init_psram(void)
{
    /* Try RP2350B pin first, then RP2350A */
    static const uint cs_pins[] = { PSRAM_CS_PIN_RP2350B, PSRAM_CS_PIN_RP2350A };
    for (int i = 0; i < 2; i++) {
        psram_init(cs_pins[i]);
        /* Write/read test */
        PSRAM_BASE[0] = 0xA5;
        PSRAM_BASE[1] = 0x5A;
        __compiler_memory_barrier();
        if (PSRAM_BASE[0] == 0xA5 && PSRAM_BASE[1] == 0x5A) {
            printf("PSRAM detected on CS pin %u\n", cs_pins[i]);
            psram_available = true;
            return true;
        }
    }
    printf("No PSRAM detected\n");
    return false;
}

/* Load first .nes ROM from SD card "nes" directory.
 * Returns ROM data pointer and sets *out_size. NULL on failure.
 * If PSRAM is available, ROM is loaded there; otherwise uses malloc.
 * Caller may free the buffer after qnes_load_rom() since QuickNES copies it. */
static uint8_t *sd_rom_buf = NULL;

/* Current ROM filename (without path/extension) for save state naming */
char g_rom_name[64] = {0};

static uint8_t *try_load_rom_from_sd(long *out_size)
{
    *out_size = 0;

    FATFS fs;
    FRESULT fr = f_mount(&fs, "", 1);
    if (fr != FR_OK) {
        printf("SD mount failed (%d)\n", fr);
        return NULL;
    }
    printf("SD card mounted\n");

    DIR dir;
    fr = f_opendir(&dir, "/nes");
    if (fr != FR_OK) {
        printf("SD: /nes directory not found (%d)\n", fr);
        f_unmount("");
        return NULL;
    }

    /* Find first .nes file */
    char filepath[280];
    FILINFO fno;
    bool found = false;
    while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != '\0') {
        if (fno.fattrib & AM_DIR)
            continue;
        size_t len = strlen(fno.fname);
        if (len >= 4 &&
            (fno.fname[len-4] == '.') &&
            (fno.fname[len-3] == 'n' || fno.fname[len-3] == 'N') &&
            (fno.fname[len-2] == 'e' || fno.fname[len-2] == 'E') &&
            (fno.fname[len-1] == 's' || fno.fname[len-1] == 'S')) {
            snprintf(filepath, sizeof(filepath), "/nes/%s", fno.fname);
            /* Store ROM name (without extension) for save state naming */
            {
                size_t nlen = len >= 4 ? len - 4 : len;
                if (nlen >= sizeof(g_rom_name)) nlen = sizeof(g_rom_name) - 1;
                memcpy(g_rom_name, fno.fname, nlen);
                g_rom_name[nlen] = '\0';
            }
            found = true;
            printf("SD: found ROM: %s (%lu bytes)\n", filepath, (unsigned long)fno.fsize);
            break;
        }
    }
    f_closedir(&dir);

    if (!found) {
        printf("SD: no .nes files in /nes\n");
        f_unmount("");
        return NULL;
    }

    FIL fil;
    fr = f_open(&fil, filepath, FA_READ);
    if (fr != FR_OK) {
        printf("SD: failed to open %s (%d)\n", filepath, fr);
        f_unmount("");
        return NULL;
    }

    FSIZE_t fsize = f_size(&fil);
    if (fsize < 16 || fsize > 2 * 1024 * 1024) {
        printf("SD: invalid ROM size %lu\n", (unsigned long)fsize);
        f_close(&fil);
        f_unmount("");
        return NULL;
    }

    /* Allocate buffer: prefer PSRAM, fall back to SRAM malloc */
    if (psram_available) {
        sd_rom_buf = (uint8_t *)PSRAM_BASE;
        printf("SD: loading ROM into PSRAM\n");
    } else {
        sd_rom_buf = (uint8_t *)malloc(fsize);
        if (!sd_rom_buf) {
            printf("SD: malloc failed for %lu bytes\n", (unsigned long)fsize);
            f_close(&fil);
            f_unmount("");
            return NULL;
        }
        printf("SD: loading ROM into SRAM\n");
    }

    UINT bytes_read;
    fr = f_read(&fil, sd_rom_buf, (UINT)fsize, &bytes_read);
    f_close(&fil);
    f_unmount("");

    if (fr != FR_OK || bytes_read != (UINT)fsize) {
        printf("SD: read error (%d, got %u/%lu)\n", fr, bytes_read, (unsigned long)fsize);
        if (!psram_available)
            free(sd_rom_buf);
        sd_rom_buf = NULL;
        return NULL;
    }

    printf("SD: loaded %lu bytes\n", (unsigned long)fsize);
    *out_size = (long)fsize;
    return sd_rom_buf;
}

/* Stack watermark: paint stack with 0xDEADBEEF, later check how much was used */
static void paint_stack(void)
{
    volatile uint32_t sp;
    __asm volatile ("MOV %0, SP" : "=r" (sp));
    uint32_t *p = (uint32_t *)big_stack;
    uint32_t *end = (uint32_t *)(sp - 256);
    while (p < end)
        *p++ = 0xDEADBEEF;
}

static uint32_t check_stack_free(void)
{
    uint32_t *p = (uint32_t *)big_stack;
    uint32_t count = 0;
    while (*p == 0xDEADBEEF) {
        p++;
        count += 4;
    }
    return count;
}

/* HardFault handler — store fault info, pump USB to flush, blink LED */
static volatile uint32_t fault_pc, fault_lr, fault_cfsr, fault_mmfar, fault_bfar;
static volatile bool fault_occurred = false;

void __attribute__((naked)) isr_hardfault(void)
{
    __asm volatile (
        "MRS r0, MSP\n"
        "B hardfault_handler_c\n"
    );
}

void __attribute__((used)) hardfault_handler_c(uint32_t *stack)
{
    fault_pc = stack[6];
    fault_lr = stack[5];
    fault_cfsr = *(volatile uint32_t *)0xE000ED28;
    fault_mmfar = *(volatile uint32_t *)0xE000ED34;
    fault_bfar = *(volatile uint32_t *)0xE000ED38;
    fault_occurred = true;

    /* Re-enable interrupts so USB can flush */
    __asm volatile ("CPSIE i");

    for (int attempt = 0; attempt < 20; attempt++) {
        printf("!FAULT! PC=%08lx LR=%08lx CFSR=%08lx MMFAR=%08lx BFAR=%08lx stk=%lu\n",
               (unsigned long)fault_pc, (unsigned long)fault_lr,
               (unsigned long)fault_cfsr, (unsigned long)fault_mmfar,
               (unsigned long)fault_bfar, (unsigned long)check_stack_free());
        for (volatile int d = 0; d < 2000000; d++) {} /* busy delay */
    }

    while (1) {
        gpio_put(PICO_DEFAULT_LED_PIN, 1);
        for (volatile int d = 0; d < 500000; d++) {}
        gpio_put(PICO_DEFAULT_LED_PIN, 0);
        for (volatile int d = 0; d < 500000; d++) {}
    }
}

int main(void)
{
    /* Switch to large stack before doing anything else */
    __asm volatile ("MSR MSP, %0" :: "r" (big_stack + sizeof(big_stack)));
    real_main();
    __builtin_unreachable();
}

/* Configure flash timing for overclocked CPU.
 * Must run from SRAM — flash is being reconfigured. */
static void __no_inline_not_in_flash_func(set_flash_timings)(int cpu_mhz, int flash_max_mhz)
{
    const int clock_hz = cpu_mhz * 1000000;
    const int max_flash_freq = flash_max_mhz * 1000000;

    int divisor = (clock_hz + max_flash_freq - (max_flash_freq >> 4) - 1) / max_flash_freq;
    if (divisor == 1 && clock_hz >= 166000000)
        divisor = 2;

    int rxdelay = divisor;
    if (clock_hz / divisor > 100000000 && clock_hz >= 166000000)
        rxdelay += 1;

    qmi_hw->m[0].timing = 0x60007000 |
                           rxdelay << QMI_M0_TIMING_RXDELAY_LSB |
                           divisor << QMI_M0_TIMING_CLKDIV_LSB;
}

/* Convert nespad button state to QuickNES joypad bitmask.
 * QuickNES: A=0, B=1, Select=2, Start=3, Up=4, Down=5, Left=6, Right=7 */
static int nespad_to_qnes(uint32_t pad)
{
    int joy = 0;
    if (pad & DPAD_A)      joy |= 0x01;
    if (pad & DPAD_B)      joy |= 0x02;
    if (pad & DPAD_SELECT) joy |= 0x04;
    if (pad & DPAD_START)  joy |= 0x08;
    if (pad & DPAD_UP)     joy |= 0x10;
    if (pad & DPAD_DOWN)   joy |= 0x20;
    if (pad & DPAD_LEFT)   joy |= 0x40;
    if (pad & DPAD_RIGHT)  joy |= 0x80;
    return joy;
}

/* Convert PS/2 keyboard state to QuickNES joypad bitmask */
static int ps2kbd_to_qnes(uint16_t kbd)
{
    int joy = 0;
    if (kbd & KBD_STATE_A)      joy |= 0x01;
    if (kbd & KBD_STATE_B)      joy |= 0x02;
    if (kbd & KBD_STATE_SELECT) joy |= 0x04;
    if (kbd & KBD_STATE_START)  joy |= 0x08;
    if (kbd & KBD_STATE_UP)     joy |= 0x10;
    if (kbd & KBD_STATE_DOWN)   joy |= 0x20;
    if (kbd & KBD_STATE_LEFT)   joy |= 0x40;
    if (kbd & KBD_STATE_RIGHT)  joy |= 0x80;
    return joy;
}

static void real_main(void)
{
    /* Overclock to 378 MHz — same pattern as murmgenesis */
    vreg_disable_voltage_limit();
    vreg_set_voltage(VREG_VOLTAGE_1_60);
    sleep_ms(10);
#if USB_HID_ENABLED
    // Use 252 MHz to get an even divider (2) for the 126 MHz HSTX clock.
    // Odd dividers (like 3 for 378 MHz) result in a 33% duty cycle, causing HDMI signal drops!
    set_flash_timings(252, 88);
    sleep_ms(10);
    set_sys_clock_khz(252000, true);
#else
    set_flash_timings(378, 88);
    sleep_ms(10);
    if (!set_sys_clock_khz(378000, false)) {
        set_flash_timings(252, 88);
        sleep_ms(10);
        set_sys_clock_khz(252000, true);
    }
#endif

    stdio_init_all();
    uart_logging_init();
    uart_logging_register();
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    gpio_put(PICO_DEFAULT_LED_PIN, 1);

    paint_stack();

    printf("\n=== murmnes (QuickNES) ===\n");
    printf("sys_clk: %lu Hz\n", (unsigned long)clock_get_hz(clk_sys));

    /* Init PSRAM early — operator new/realloc redirect to PSRAM */
    try_init_psram();

    printf("qnes_init...\n");
    if (qnes_init(SAMPLE_RATE) != 0) {
        printf("qnes_init FAILED\n");
        while (1) sleep_ms(100);
    }
    printf("qnes_init OK\n");

    /* Provide PSRAM for tile cache (large CHR ROMs need ~256KB+) */
    if (psram_available) {
        /* Tile cache in PSRAM via UNCACHED alias (0x15xxxxxx).
         * Uncached access bypasses XIP cache, preventing cache pollution
         * that disrupts HSTX DMA when writing tile data during HDMI output. */
        void *tc = (void *)(0x15000000 + 2 * 1024 * 1024);
        qnes_set_tile_cache_buf(tc, 1 * 1024 * 1024);
    }

    settings_load();

    /* Scan SD for ROMs and load metadata (CRCs, titles) before HDMI starts.
     * ROM data is loaded on demand when selected. */
    int num_roms = 0;
    long preloaded_rom_size = 0;
    if (psram_available) {
        num_roms = rom_selector_preload(&preloaded_rom_size);
        /* PSRAM metadata writes dirty the XIP cache — stale entries prevent
         * HSTX from starting correctly. Invalidate before HDMI init. */
        xip_cache_invalidate_all();
    }

    bool rom_loaded = false;
    xip_cache_invalidate_all();

    /* Start HDMI */
    frame_pixels = test_pixels;
    frame_pitch = NES_WIDTH;

    hstx_di_queue_init();
    audio_fill_silence(SAMPLE_RATE / 60 * 6); /* pre-fill ~100ms */
    video_output_set_vsync_callback(vsync_cb);
    video_output_init(FRAME_WIDTH, FRAME_HEIGHT);

#if USB_HID_ENABLED
    /* If USB HID is enabled, we MUST leave pll_usb at 48 MHz for TinyUSB.
     * Derive clk_hstx from pll_sys instead to avoid clk_sys mux jitter!
     * pll_sys is either 378 MHz or 252 MHz, we configure divider for 126 MHz. */
    clock_configure(clk_hstx, 0,
                    CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
                    clock_get_hz(clk_sys), 126000000);
#else
    /* Override HSTX clock: PLL_USB reconfigured to 126 MHz.
     * Independent of sys_clk — works at any CPU speed. */
    pll_deinit(pll_usb);
    pll_init(pll_usb, 1, 756000000, 6, 1); /* 12 × 63 / 6 = 126 MHz */
    clock_configure(clk_hstx, 0,
                    CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB,
                    126000000, 126000000);
#endif

    pico_hdmi_set_audio_sample_rate(SAMPLE_RATE);
    video_output_set_scanline_callback(scanline_callback);
    multicore_launch_core1(video_output_core1_run);
    sleep_ms(100);

    /* Init NES gamepad PIO driver (after HDMI, matching murmgenesis order) */
    nespad_begin(clock_get_hz(clk_sys) / 1000,
                 NESPAD_CLK_PIN, NESPAD_DATA_PIN, NESPAD_LATCH_PIN);

    /* Init PS/2 keyboard (PIO0, CLK=GPIO2, DATA=GPIO3) */
    ps2kbd_init();
    printf("PS/2 keyboard initialized (CLK=%d, DATA=%d)\n", PS2_PIN_CLK, PS2_PIN_DATA);

#if USB_HID_ENABLED
    usbhid_init();
    printf("USB HID Host initialized\n");
#endif

    /* Show welcome screen */
    welcome_screen_show();

    /* Check SD card availability */
    {
        bool sd_ok = false;
        if (psram_available) {
            sd_ok = rom_selector_sd_ok();
        } else {
            /* Quick SD card check for non-PSRAM path */
            FATFS tmp_fs;
            if (f_mount(&tmp_fs, "", 1) == FR_OK) {
                sd_ok = true;
                f_unmount("");
            }
        }
        if (!sd_ok) {
            sd_error_show();
        }
    }

    while (1) {  /* outer loop: ROM selector → emulation → reset → ROM selector */

    /* Show ROM selector */
    rom_loaded = false;
    if (num_roms > 0) {
        long rom_size = 0;
        if (rom_selector_show(&rom_size)) {
            sd_rom_buf = (uint8_t *)rom_selector_get_rom_data();
            printf("Initializing emulator (%ld bytes)...\n", rom_size);
            if (qnes_load_rom_inplace(sd_rom_buf, rom_size) == 0) {
                printf("Emulator initialized OK\n");
                rom_loaded = true;
            }
        }
    }

    /* Fallback: try loading first .nes from SD — only when selector was
     * not available (no PSRAM or no ROMs found).  If the selector was shown
     * but the chosen ROM failed to load, loop back instead of silently
     * loading a different game (which also risks OOM). */
    if (!rom_loaded && num_roms == 0) {
        long sd_rom_size = 0;
        uint8_t *sd_rom = try_load_rom_from_sd(&sd_rom_size);
        if (sd_rom) {
            if (qnes_load_rom_inplace(sd_rom, sd_rom_size) == 0) {
                printf("SD ROM loaded (fallback)\n");
                rom_loaded = true;
            }
            if (!rom_loaded && !psram_available && sd_rom_buf)
                free(sd_rom_buf);
        }
    }

#ifdef HAS_NES_ROM
    if (!rom_loaded) {
        long rom_size = (long)(nes_rom_end - nes_rom_data);
        if (qnes_load_rom_inplace(nes_rom_data, rom_size) == 0) {
            printf("Flash ROM loaded\n");
            rom_loaded = true;
            if (g_rom_name[0] == '\0')
                snprintf(g_rom_name, sizeof(g_rom_name), "flash_rom");
        }
    }
#endif

    if (rom_loaded) {
        bool reset_requested = false;
        while (1) {
            /* Wait for vsync — Core 1 applies pending frame during vblank. */
            while (pending_pixels != NULL) {
#if USB_HID_ENABLED
                usbhid_task();
#endif
                __wfe();
            }
            vsync_flag = 0;

            /* Fresh gamepad read right at vsync — input from NOW, not
             * from the previous frame. ~100µs cost is negligible. */
            nespad_read();
            ps2kbd_tick();
            
            uint16_t kbd_state = ps2kbd_get_state();

#if USB_HID_ENABLED
            kbd_state |= usbhid_get_kbd_state();
#endif

            /* Check for menu hotkey (Start+Select, F12) */
            if (settings_check_hotkey()) {
                settings_result_t result = settings_menu_show(test_pixels);
                if (result == SETTINGS_RESULT_RESET) {
                    qnes_close();
                    g_rom_name[0] = '\0';
                    reset_requested = true;
                    break;
                }
                /* Restore game palette after menu */
                update_palette(pal_write_idx);
                pending_pal_idx = pal_write_idx;
                pal_write_idx ^= 1;
                continue;
            }

            /* Build per-source joypad values */
            int nespad1_joy = nespad_to_qnes(nespad_state);
            int nespad2_joy = nespad_to_qnes(nespad_state2);
            int kbd_joy = ps2kbd_to_qnes(kbd_state);
            int usb1_joy = 0, usb2_joy = 0;

#if USB_HID_ENABLED
            for (int ui = 0; ui < 2; ui++) {
                if (!usbhid_gamepad_connected_idx(ui)) continue;
                usbhid_gamepad_state_t gp;
                usbhid_get_gamepad_state_idx(ui, &gp);

                int uj = 0;
                if (gp.dpad & 0x01) uj |= 0x10; // Up
                if (gp.dpad & 0x02) uj |= 0x20; // Down
                if (gp.dpad & 0x04) uj |= 0x40; // Left
                if (gp.dpad & 0x08) uj |= 0x80; // Right
                if (gp.buttons & 0x01) uj |= 0x01; // A -> NES A
                if (gp.buttons & 0x02) uj |= 0x02; // B -> NES B
                if (gp.buttons & 0x04) uj |= 0x01; // C -> NES A
                if (gp.buttons & 0x08) uj |= 0x02; // X -> NES B
                if (gp.buttons & 0x40) uj |= 0x08; // Start -> NES Start
                if (gp.buttons & 0x80) uj |= 0x04; // Select -> NES Select

                if (ui == 0) usb1_joy = uj; else usb2_joy = uj;
            }
#endif

            /* Route inputs based on settings */
            int joypad1 = 0, joypad2 = 0;

            switch (g_settings.p1_mode) {
                case INPUT_MODE_ANY:      joypad1 = nespad1_joy | nespad2_joy | kbd_joy | usb1_joy | usb2_joy; break;
                case INPUT_MODE_NES1:     joypad1 = nespad1_joy; break;
                case INPUT_MODE_NES2:     joypad1 = nespad2_joy; break;
                case INPUT_MODE_USB1:     joypad1 = usb1_joy; break;
                case INPUT_MODE_USB2:     joypad1 = usb2_joy; break;
                case INPUT_MODE_KEYBOARD: joypad1 = kbd_joy; break;
                case INPUT_MODE_DISABLED: break;
            }

            switch (g_settings.p2_mode) {
                case INPUT_MODE_ANY:      joypad2 = nespad1_joy | nespad2_joy | kbd_joy | usb1_joy | usb2_joy; break;
                case INPUT_MODE_NES1:     joypad2 = nespad1_joy; break;
                case INPUT_MODE_NES2:     joypad2 = nespad2_joy; break;
                case INPUT_MODE_USB1:     joypad2 = usb1_joy; break;
                case INPUT_MODE_USB2:     joypad2 = usb2_joy; break;
                case INPUT_MODE_KEYBOARD: joypad2 = kbd_joy; break;
                case INPUT_MODE_DISABLED: break;
            }

            qnes_emulate_frame(joypad1, joypad2);

            /* Push NES audio into DI queue. No padding — produce only what
             * the NES generates. Carry handles 4-sample boundary. Tiny rate
             * deficit (~1.5 samples/frame) handled by ISR silence fallback. */
            int16_t tmp[1024];
            long n = qnes_read_samples(tmp, 1024);

            if (n > 0) {
                audio_push_samples(tmp, (int)n);
            }

            update_palette(pal_write_idx);
            pending_pal_idx = pal_write_idx;
            pal_write_idx ^= 1;

            /* Post frame to pending buffer — vsync callback will apply it
             * during vblank for tear-free display. */
            pending_pitch = 272;
            pending_pixels = qnes_get_pixels();

        }
        if (reset_requested) continue;
    } else if (num_roms > 0) {
        /* Selector was available but load failed — loop back to selector */
        printf("ROM load failed, returning to selector\n");
        continue;
    } else {
        printf("No ROM loaded (no SD card ROM, no flash ROM).\n");
        while (1) { sleep_ms(100); }
    }

    break;  /* no reset — should not reach here normally */

    }  /* end outer while(1) loop */
}
