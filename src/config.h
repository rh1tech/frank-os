#ifndef DISPHSTX_CONFIG_H
#define DISPHSTX_CONFIG_H

// DispHSTX configuration for FRANK OS

#define DISPHSTX_PICOSDK    1       // Use PicoSDK (not PicoLibSDK)
#define USE_DISPHSTX        1       // Enable HSTX display driver

#define USE_DRAWCAN         0       // We use our own drawing code
#define USE_DRAWCAN0        0
#define USE_DRAWCAN1        0
#define USE_DRAWCAN2        0
#define USE_DRAWCAN3        0
#define USE_DRAWCAN4        0
#define USE_DRAWCAN6        0
#define USE_DRAWCAN8        0
#define USE_DRAWCAN12       0
#define USE_DRAWCAN16       0
#define USE_RAND            0
#define USE_TEXT            0

// DVI only, no VGA
#define DISPHSTX_USE_DVI    1
#define DISPHSTX_USE_VGA    0

// M2 DVI pinout: CLK-=12, CLK+=13, D0-=14, D0+=15, D1-=16, D1+=17, D2-=18, D2+=19
#define DISPHSTX_DVI_PINOUT 2       // order CLK-..D2+

// Disable unused formats to save RAM
#define DISPHSTX_USE_FORMAT_1           0
#define DISPHSTX_USE_FORMAT_2           0
#define DISPHSTX_USE_FORMAT_3           0
#define DISPHSTX_USE_FORMAT_4           0
#define DISPHSTX_USE_FORMAT_6           0
#define DISPHSTX_USE_FORMAT_8           0
#define DISPHSTX_USE_FORMAT_12          0
#define DISPHSTX_USE_FORMAT_15          0
#define DISPHSTX_USE_FORMAT_16          0
#define DISPHSTX_USE_FORMAT_1_PAL       0
#define DISPHSTX_USE_FORMAT_2_PAL       0
#define DISPHSTX_USE_FORMAT_3_PAL       0
#define DISPHSTX_USE_FORMAT_4_PAL       1   // 4-bit paletted: the only format we use
#define DISPHSTX_USE_FORMAT_6_PAL       0
#define DISPHSTX_USE_FORMAT_8_PAL       0
#define DISPHSTX_USE_FORMAT_COL         0
#define DISPHSTX_USE_FORMAT_MTEXT       0
#define DISPHSTX_USE_FORMAT_ATEXT       0
#define DISPHSTX_USE_FORMAT_TILE4_8     0
#define DISPHSTX_USE_FORMAT_TILE8_8     0
#define DISPHSTX_USE_FORMAT_TILE16_8    0
#define DISPHSTX_USE_FORMAT_TILE32_8    0
#define DISPHSTX_USE_FORMAT_TILE4_8_PAL 0
#define DISPHSTX_USE_FORMAT_TILE8_8_PAL 0
#define DISPHSTX_USE_FORMAT_TILE16_8_PAL 0
#define DISPHSTX_USE_FORMAT_TILE32_8_PAL 0
#define DISPHSTX_USE_FORMAT_HSTX_15    0
#define DISPHSTX_USE_FORMAT_HSTX_16    0
#define DISPHSTX_USE_FORMAT_PAT_8      0
#define DISPHSTX_USE_FORMAT_PAT_8_PAL  0
#define DISPHSTX_USE_FORMAT_RLE8       0
#define DISPHSTX_USE_FORMAT_RLE8_PAL   0
#define DISPHSTX_USE_FORMAT_ATTR1_PAL  0
#define DISPHSTX_USE_FORMAT_ATTR2_PAL  0
#define DISPHSTX_USE_FORMAT_ATTR3_PAL  0
#define DISPHSTX_USE_FORMAT_ATTR4_PAL  0
#define DISPHSTX_USE_FORMAT_ATTR5_PAL  0
#define DISPHSTX_USE_FORMAT_ATTR6_PAL  0
#define DISPHSTX_USE_FORMAT_ATTR7_PAL  0
#define DISPHSTX_USE_FORMAT_ATTR8_PAL  0

// Limit max width to save RAM on render line buffers
#define DISPHSTX_WIDTHMAX   640

// Use only 1 strip and 1 slot
#define DISPHSTX_STRIP_MAX  1
#define DISPHSTX_SLOT_MAX   1

#endif // DISPHSTX_CONFIG_H
