#ifndef _EMU8950_H_
#define _EMU8950_H_

#include <stdint.h>
#include "slot_render.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OPL_DEBUG 0

/* mask */
#define OPL_MASK_CH(x) (1 << (x))
#define OPL_MASK_HH (1 << 9)
#define OPL_MASK_CYM (1 << 10)
#define OPL_MASK_TOM (1 << 11)
#define OPL_MASK_SD (1 << 12)
#define OPL_MASK_BD (1 << 13)
#define OPL_MASK_ADPCM (1 << 14)
#define OPL_MASK_RHYTHM (OPL_MASK_HH | OPL_MASK_CYM | OPL_MASK_TOM | OPL_MASK_SD | OPL_MASK_BD)

#if !EMU8950_NO_RATECONV
/* rate conveter */
typedef struct __OPL_RateConv {
  int ch;
  double timer;
  double f_ratio;
  int16_t *sinc_table;
  int16_t **buf;
} OPL_RateConv;

OPL_RateConv *OPL_RateConv_new(double f_inp, double f_out, int ch);
void OPL_RateConv_reset(OPL_RateConv *conv);
void OPL_RateConv_putData(OPL_RateConv *conv, int ch, int16_t data);
int16_t OPL_RateConv_getData(OPL_RateConv *conv, int ch);
void OPL_RateConv_delete(OPL_RateConv *conv);
#endif

#if EMU8950_SLOT_RENDER
#include "slot_render.h"
// Macros to abstract member access - when SLOT_RENDER enabled, members are in render substruct
#define SLOT_MEMBER(slot, member) ((slot)->render.member)
#else
#define SLOT_MEMBER(slot, member) ((slot)->member)
#endif

/* slot */
typedef struct __OPL_SLOT {
#if EMU8950_SLOT_RENDER
  // Embedded SLOT_RENDER structure - must be first
  struct SLOT_RENDER render;
#else
  // Original structure members for non-SLOT_RENDER builds
  uint8_t eg_state;
  uint8_t eg_rate_h;
  uint8_t eg_rate_l;
  uint8_t rks;
  int16_t eg_out;
  uint16_t tll;
  int32_t output[2];
  int16_t *mod_buffer;
  int32_t *buffer;
  uint32_t eg_shift;
  OPL_PATCH *patch;
  uint16_t fnum;
  uint16_t efix_pg_phase_multiplier;
  uint8_t blk;
  uint32_t pg_phase;
  uint8_t *lfo_am_buffer;
  uint8_t pm_mode;
#if !EMU8950_NO_WAVE_TABLE_MAP
  uint16_t *wave_table;
#endif
#endif
  
  uint8_t number;

#if !EMU8950_NO_PERCUSSION_MODE
  uint8_t type;
#endif

  OPL_PATCH __patch;

  uint32_t pg_out;
#if !EMU8950_NO_PERCUSSION_MODE
  uint8_t pg_keep;
#endif
  uint16_t blk_fnum;

  uint32_t update_requests;

#if OPL_DEBUG
  uint8_t last_eg_state;
#endif
} OPL_SLOT;

typedef struct __OPL {
  uint32_t clk;
  uint32_t rate;

#if !EMU8950_NO_TIMER
  uint8_t csm_mode;
  uint8_t csm_key_count;
#endif
  uint8_t notesel;

  uint32_t inp_step;
  uint32_t out_step;
  uint32_t out_time;

#if EMU8950_LINEAR
#if EMU8950_SLOT_RENDER
    uint8_t *lfo_am_buffer_lsl3;
#else
  uint8_t *lfo_am_buffer;
#endif
    int16_t *mod_buffer;
    int32_t *buffer;
#endif
#if !EMU8950_NO_TEST_FLAG
  uint8_t test_flag;
#endif
  uint32_t slot_key_status;
#if !EMU8950_NO_PERCUSSION_MODE
  uint8_t perc_mode;
#endif

  uint32_t eg_counter;

  uint32_t pm_phase;
  uint32_t pm_dphase;

#if !EMU8950_NO_TEST_FLAG
  int32_t am_phase;
#else
  uint8_t am_phase_index;
#endif
  uint8_t lfo_am;

#if !EMU8950_NO_PERCUSSION_MODE
  uint32_t noise;
  uint8_t short_noise;
#endif

  uint8_t reg[0x100];
  uint8_t ch_alg[9]; // alg for each channels

  uint8_t pan[16];

  uint32_t mask;
  uint8_t am_mode;
  uint8_t pm_mode;

  /* channel output */
  /* 0..8:tone 9:bd 10:hh 11:sd 12:tom 13:cym 14:adpcm */
  int16_t ch_out[15];

  int16_t mix_out[2];

  OPL_SLOT slot[18];

#if !EMU8950_NO_RATECONV
  OPL_RateConv *conv;
#endif

#if !EMU8950_NO_TIMER
  uint32_t timer1_counter; //  80us counter
  uint32_t timer2_counter; // 320us counter
  void *timer1_user_data;
  void *timer2_user_data;
  void (*timer1_func)(void *user);
  void (*timer2_func)(void *user);
#endif
  uint8_t status;

} OPL;

#if !EMU8950_NO_TEST_FLAG
#define opl_test_flag(opl) opl->test_flag
#else
// waveform enable only
#define opl_test_flag(opl) 0x20
#endif

OPL *OPL_new(uint32_t clk, uint32_t rate);
void OPL_delete(OPL *);

void OPL_reset(OPL *);

/** 
 * Set output wave sampling rate. 
 * @param rate sampling rate. If clock / 72 (typically 49716 or 49715 at 3.58MHz) is set, the internal rate converter is disabled.
 */
void OPL_setRate(OPL *opl, uint32_t rate);

/** 
 * Set internal calcuration quality. Currently no effects, just for compatibility.
 * >= v1.0.0 always synthesizes internal output at clock/72 Hz.
 */
void OPL_setQuality(OPL *opl, uint8_t q);

/**
 * Set fine-grained panning
 * @param ch 0..8:tone 9:bd 10:hh 11:sd 12:tom 13:cym 14,15:reserved
 * @param pan output strength of left/right channel. 
 *            pan[0]: left, pan[1]: right. pan[0]=pan[1]=1.0f for center.
 */
void OPL_setPanFine(OPL *opl, uint32_t ch, float pan[2]);

void OPL_writeIO(OPL *opl, uint32_t reg, uint8_t val);
void OPL_writeReg(OPL *opl, uint32_t reg, uint8_t val);

/**
 * Calculate sample
 */
int16_t OPL_calc(OPL *opl);

void OPL_calc_buffer(OPL *opl, int16_t *buffer, uint32_t nsamples);
// LE left/right channels int16:int16
void OPL_calc_buffer_stereo(OPL *opl, int32_t *buffer, uint32_t nsamples);

/**
 *  Set channel mask 
 *  @param mask mask flag: OPL_MASK_* can be used.
 *  - bit 0..8: mask for ch 1 to 9 (OPL_MASK_CH(i))
 *  - bit 9: mask for Hi-Hat (OPL_MASK_HH)
 *  - bit 10: mask for Top-Cym (OPL_MASK_CYM)
 *  - bit 11: mask for Tom (OPL_MASK_TOM)
 *  - bit 12: mask for Snare Drum (OPL_MASK_SD)
 *  - bit 13: mask for Bass Drum (OPL_MASK_BD)
 */
uint32_t OPL_setMask(OPL *, uint32_t mask);

/**
 * Read OPL status register
 * @returns
 * 76543210
 * |||||  +- D0: PCM/BSY
 * ||||+---- D3: BUF/RDY
 * |||+----- D4: EOS
 * ||+------ D5: TIMER2
 * |+------- D6: TIMER1
 * +-------- D7: IRQ
 */
uint8_t OPL_status(OPL *opl);

/* for compatibility */
#define OPL_set_rate OPL_setRate
#define OPL_set_quality OPL_setQuality
#define OPL_set_pan OPL_setPan
#define OPL_set_pan_fine OPL_setPanFine

#ifdef __cplusplus
}
#endif

#endif
