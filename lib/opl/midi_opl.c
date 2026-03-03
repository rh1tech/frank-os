/*
 * MIDI OPL FM synthesizer — render-based API
 * Adapted from Chocolate Doom / murmdoom i_oplmusic.c + opl_pico.c
 *
 * This merges the callback-driven OPL engine into a synchronous
 * midi_opl_render() function suitable for a pull-based audio pipeline.
 *
 * Copyright(C) 1993-1996 Id Software, Inc.
 * Copyright(C) 2005-2014 Simon Howard
 * Copyright(C) 2021-2022 Graham Sanderson
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdlib.h>
#include <string.h>
#include "emu8950.h"
#include "midifile.h"
#include "midi_opl.h"
#include "genmidi_data.h"

/* ================================================================== */
/* OPL register definitions                                           */
/* ================================================================== */

#define OPL_NUM_VOICES   9

#define OPL_REG_WAVEFORM_ENABLE 0x01
#define OPL_REGS_TREMOLO        0x20
#define OPL_REGS_LEVEL          0x40
#define OPL_REGS_ATTACK         0x60
#define OPL_REGS_SUSTAIN        0x80
#define OPL_REGS_FREQ_1         0xA0
#define OPL_REGS_FREQ_2         0xB0
#define OPL_REGS_FEEDBACK       0xC0
#define OPL_REGS_WAVEFORM       0xE0

#define MIDI_SAMPLE_RATE 44100

/* ================================================================== */
/* GENMIDI structures                                                 */
/* ================================================================== */

typedef struct {
    uint8_t tremolo;
    uint8_t attack;
    uint8_t sustain;
    uint8_t waveform;
    uint8_t scale;
    uint8_t level;
} genmidi_op_t;

typedef struct {
    genmidi_op_t modulator;
    uint8_t feedback;
    genmidi_op_t carrier;
    uint8_t unused;
    int16_t base_note_offset;
} genmidi_voice_t;

typedef struct {
    uint16_t flags;
    uint8_t fine_tuning;
    uint8_t fixed_note;
    genmidi_voice_t voices[2];
} genmidi_instr_t;

/* ================================================================== */
/* Voice / Channel types                                              */
/* ================================================================== */

typedef struct {
    genmidi_instr_t *instrument;
    int volume;
    int volume_base;
    int pan;
    int bend;
} opl_channel_data_t;

typedef struct {
    midi_track_iter_t *iter;
} opl_track_data_t;

typedef struct {
    int index;
    int op1, op2;
    unsigned int current_instr_voice;
    unsigned int key;
    unsigned int note;
    unsigned int note_volume;
    unsigned int car_volume;
    unsigned int mod_volume;
    unsigned int reg_pan;
    unsigned int priority;
    unsigned int array;
    unsigned int freq;
    genmidi_instr_t *current_instr;
    opl_channel_data_t *channel;
} opl_voice_t;

/* ================================================================== */
/* Main context                                                       */
/* ================================================================== */

struct midi_opl {
    OPL *opl;
    midi_file_t *midi;
    bool playing;
    bool looping;

    /* Instruments */
    genmidi_instr_t *main_instrs;
    genmidi_instr_t *percussion_instrs;

    /* Voices */
    opl_voice_t voices[OPL_NUM_VOICES];
    opl_voice_t *voice_free_list[OPL_NUM_VOICES];
    opl_voice_t *voice_alloced_list[OPL_NUM_VOICES];
    int voice_free_num;
    int voice_alloced_num;

    /* Channels */
    opl_channel_data_t channels[MIDI_CHANNELS_PER_TRACK];

    /* Track data */
    opl_track_data_t *tracks;
    unsigned int num_tracks;
    unsigned int running_tracks;

    /* Tempo */
    unsigned int ticks_per_beat;
    unsigned int us_per_beat;

    /* Time tracking (in microseconds) */
    uint64_t current_time;

    /* Per-track next-event times (in microseconds) */
    uint64_t *track_next_time;

    /* Music volume (0–127) */
    int music_volume;

    /* OPL temp buffer for 32→16 bit conversion */
    int32_t opl_buf[2048];
};

/* ================================================================== */
/* Operator/Voice tables                                              */
/* ================================================================== */

static const int voice_operators[2][OPL_NUM_VOICES] = {
    { 0x00, 0x01, 0x02, 0x08, 0x09, 0x0a, 0x10, 0x11, 0x12 },
    { 0x03, 0x04, 0x05, 0x0b, 0x0c, 0x0d, 0x13, 0x14, 0x15 }
};

/* Frequency curve from Doom's OPL driver */
static const uint16_t frequency_curve[] = {
    0x133, 0x133, 0x134, 0x134, 0x135, 0x136, 0x136, 0x137,
    0x137, 0x138, 0x138, 0x139, 0x139, 0x13a, 0x13b, 0x13b,
    0x13c, 0x13c, 0x13d, 0x13d, 0x13e, 0x13f, 0x13f, 0x140,
    0x140, 0x141, 0x142, 0x142, 0x143, 0x143, 0x144, 0x144,
    0x145, 0x146, 0x146, 0x147, 0x147, 0x148, 0x149, 0x149,
    0x14a, 0x14a, 0x14b, 0x14c, 0x14c, 0x14d, 0x14d, 0x14e,
    0x14f, 0x14f, 0x150, 0x150, 0x151, 0x152, 0x152, 0x153,
    0x153, 0x154, 0x155, 0x155, 0x156, 0x157, 0x157, 0x158,
    0x158, 0x159, 0x15a, 0x15a, 0x15b, 0x15b, 0x15c, 0x15d,
    0x15d, 0x15e, 0x15f, 0x15f, 0x160, 0x161, 0x161, 0x162,
    0x162, 0x163, 0x164, 0x164, 0x165, 0x166, 0x166, 0x167,
    0x168, 0x168, 0x169, 0x16a, 0x16a, 0x16b, 0x16c, 0x16c,
    0x16d, 0x16e, 0x16e, 0x16f, 0x170, 0x170, 0x171, 0x172,
    0x172, 0x173, 0x174, 0x174, 0x175, 0x176, 0x176, 0x177,
    0x178, 0x178, 0x179, 0x17a, 0x17a, 0x17b, 0x17c, 0x17c,
    0x17d, 0x17e, 0x17e, 0x17f, 0x180, 0x181, 0x181, 0x182,
    0x183, 0x183, 0x184, 0x185, 0x185, 0x186, 0x187, 0x188,
    0x188, 0x189, 0x18a, 0x18a, 0x18b, 0x18c, 0x18d, 0x18d,
    0x18e, 0x18f, 0x18f, 0x190, 0x191, 0x192, 0x192, 0x193,
    0x194, 0x194, 0x195, 0x196, 0x197, 0x197, 0x198, 0x199,
    0x19a, 0x19a, 0x19b, 0x19c, 0x19d, 0x19d, 0x19e, 0x19f,
    0x1a0, 0x1a0, 0x1a1, 0x1a2, 0x1a3, 0x1a3, 0x1a4, 0x1a5,
    0x1a6, 0x1a6, 0x1a7, 0x1a8, 0x1a9, 0x1a9, 0x1aa, 0x1ab,
    0x1ac, 0x1ad, 0x1ad, 0x1ae, 0x1af, 0x1b0, 0x1b0, 0x1b1,
    0x1b2, 0x1b3, 0x1b4, 0x1b4, 0x1b5, 0x1b6, 0x1b7, 0x1b8,
    0x1b8, 0x1b9, 0x1ba, 0x1bb, 0x1bc, 0x1bc, 0x1bd, 0x1be,
    0x1bf, 0x1c0, 0x1c0, 0x1c1, 0x1c2, 0x1c3, 0x1c4, 0x1c4,
    0x1c5, 0x1c6, 0x1c7, 0x1c8, 0x1c9, 0x1c9, 0x1ca, 0x1cb,
    0x1cc, 0x1cd, 0x1ce, 0x1ce, 0x1cf, 0x1d0, 0x1d1, 0x1d2,
    0x1d3, 0x1d3, 0x1d4, 0x1d5, 0x1d6, 0x1d7, 0x1d8, 0x1d8,
    0x1d9, 0x1da, 0x1db, 0x1dc, 0x1dd, 0x1de, 0x1de, 0x1df,
    0x1e0, 0x1e1, 0x1e2, 0x1e3, 0x1e4, 0x1e5, 0x1e5, 0x1e6,
    0x1e7, 0x1e8, 0x1e9, 0x1ea, 0x1eb, 0x1ec, 0x1ed, 0x1ed,
    0x1ee, 0x1ef, 0x1f0, 0x1f1, 0x1f2, 0x1f3, 0x1f4, 0x1f5,
    0x1f6, 0x1f6, 0x1f7, 0x1f8, 0x1f9, 0x1fa, 0x1fb, 0x1fc,
    0x1fd, 0x1fe, 0x1ff, 0x200, 0x201, 0x201, 0x202, 0x203,
    0x204, 0x205, 0x206, 0x207, 0x208, 0x209, 0x20a, 0x20b,
    0x20c, 0x20d, 0x20e, 0x20f, 0x210, 0x210, 0x211, 0x212,
    0x213, 0x214, 0x215, 0x216, 0x217, 0x218, 0x219, 0x21a,
    0x21b, 0x21c, 0x21d, 0x21e, 0x21f, 0x220, 0x221, 0x222,
    0x223, 0x224, 0x225, 0x226, 0x227, 0x228, 0x229, 0x22a,
    0x22b, 0x22c, 0x22d, 0x22e, 0x22f, 0x230, 0x231, 0x232,
    0x233, 0x234, 0x235, 0x236, 0x237, 0x238, 0x239, 0x23a,
    0x23b, 0x23c, 0x23d, 0x23e, 0x23f, 0x240, 0x241, 0x242,
    0x244, 0x245, 0x246, 0x247, 0x248, 0x249, 0x24a, 0x24b,
    0x24c, 0x24d, 0x24e, 0x24f, 0x250, 0x251, 0x252, 0x253,
    0x254, 0x256, 0x257, 0x258, 0x259, 0x25a, 0x25b, 0x25c,
    0x25d, 0x25e, 0x25f, 0x260, 0x262, 0x263, 0x264, 0x265,
    0x266, 0x267, 0x268, 0x269, 0x26a, 0x26c, 0x26d, 0x26e,
    0x26f, 0x270, 0x271, 0x272, 0x273, 0x275, 0x276, 0x277,
    0x278, 0x279, 0x27a, 0x27b, 0x27d, 0x27e, 0x27f, 0x280,
    0x281, 0x282, 0x284, 0x285, 0x286, 0x287, 0x288, 0x289,
    0x28b, 0x28c, 0x28d, 0x28e, 0x28f, 0x290, 0x292, 0x293,
    0x294, 0x295, 0x296, 0x298, 0x299, 0x29a, 0x29b, 0x29c,
    0x29e, 0x29f, 0x2a0, 0x2a1, 0x2a2, 0x2a4, 0x2a5, 0x2a6,
    0x2a7, 0x2a9, 0x2aa, 0x2ab, 0x2ac, 0x2ae, 0x2af, 0x2b0,
    0x2b1, 0x2b2, 0x2b4, 0x2b5, 0x2b6, 0x2b7, 0x2b9, 0x2ba,
    0x2bb, 0x2bd, 0x2be, 0x2bf, 0x2c0, 0x2c2, 0x2c3, 0x2c4,
    0x2c5, 0x2c7, 0x2c8, 0x2c9, 0x2cb, 0x2cc, 0x2cd, 0x2ce,
    0x2d0, 0x2d1, 0x2d2, 0x2d4, 0x2d5, 0x2d6, 0x2d8, 0x2d9,
    0x2da, 0x2dc, 0x2dd, 0x2de, 0x2e0, 0x2e1, 0x2e2, 0x2e4,
    0x2e5, 0x2e6, 0x2e8, 0x2e9, 0x2ea, 0x2ec, 0x2ed, 0x2ee,
    0x2f0, 0x2f1, 0x2f2, 0x2f4, 0x2f5, 0x2f6, 0x2f8, 0x2f9,
    0x2fb, 0x2fc, 0x2fd, 0x2ff, 0x300, 0x302, 0x303, 0x304,
    0x306, 0x307, 0x309, 0x30a, 0x30b, 0x30d, 0x30e, 0x310,
    0x311, 0x312, 0x314, 0x315, 0x317, 0x318, 0x31a, 0x31b,
    0x31c, 0x31e, 0x31f, 0x321, 0x322, 0x324, 0x325, 0x327,
    0x328, 0x329, 0x32b, 0x32c, 0x32e, 0x32f, 0x331, 0x332,
    0x334, 0x335, 0x337, 0x338, 0x33a, 0x33b, 0x33d, 0x33e,
    0x340, 0x341, 0x343, 0x344, 0x346, 0x347, 0x349, 0x34a,
    0x34c, 0x34d, 0x34f, 0x350, 0x352, 0x353, 0x355, 0x357,
    0x358, 0x35a, 0x35b, 0x35d, 0x35e, 0x360, 0x361, 0x363,
    0x365, 0x366, 0x368, 0x369, 0x36b, 0x36c, 0x36e, 0x370,
    0x371, 0x373, 0x374, 0x376, 0x378, 0x379, 0x37b, 0x37c,
    0x37e, 0x380, 0x381, 0x383, 0x384, 0x386, 0x388, 0x389,
    0x38b, 0x38d, 0x38e, 0x390, 0x392, 0x393, 0x395, 0x397,
    0x398, 0x39a, 0x39c, 0x39d, 0x39f, 0x3a1, 0x3a2, 0x3a4,
    0x3a6, 0x3a7, 0x3a9, 0x3ab, 0x3ac, 0x3ae, 0x3b0, 0x3b1,
    0x3b3, 0x3b5, 0x3b7, 0x3b8, 0x3ba, 0x3bc, 0x3bd, 0x3bf,
    0x3c1, 0x3c3, 0x3c4, 0x3c6, 0x3c8, 0x3ca, 0x3cb, 0x3cd,
    0x3cf, 0x3d1, 0x3d2, 0x3d4, 0x3d6, 0x3d8, 0x3da, 0x3db,
    0x3dd, 0x3df, 0x3e1, 0x3e3, 0x3e4, 0x3e6, 0x3e8, 0x3ea,
    0x3ec, 0x3ed, 0x3ef, 0x3f1, 0x3f3, 0x3f5, 0x3f6, 0x3f8,
    0x3fa, 0x3fc, 0x3fe, 0x36c,
};

/* Volume mapping: MIDI (0-127) → OPL (0-127) */
static const uint8_t volume_mapping_table[] = {
    0, 1, 3, 5, 6, 8, 10, 11, 13, 14, 16, 17, 19, 20, 22, 23,
    25, 26, 27, 29, 30, 32, 33, 34, 36, 37, 39, 41, 43, 45, 47, 49,
    50, 52, 54, 55, 57, 59, 60, 61, 63, 64, 66, 67, 68, 69, 71, 72,
    73, 74, 75, 76, 77, 79, 80, 81, 82, 83, 84, 84, 85, 86, 87, 88,
    89, 90, 91, 92, 92, 93, 94, 95, 96, 96, 97, 98, 99, 99,100,101,
   101,102,103,103,104,105,105,106,107,107,108,109,109,110,110,111,
   112,112,113,113,114,114,115,115,116,117,117,118,118,119,119,120,
   120,121,121,122,122,123,123,123,124,124,125,125,126,126,127,127
};

/* ================================================================== */
/* Internal: OPL register write helper                                */
/* ================================================================== */

static inline void opl_write(midi_opl_t *ctx, unsigned int reg, uint8_t val) {
    OPL_writeReg(ctx->opl, reg, val);
}

/* ================================================================== */
/* Voice management                                                   */
/* ================================================================== */

static opl_voice_t *GetFreeVoice(midi_opl_t *ctx) {
    if (ctx->voice_free_num == 0) return NULL;
    opl_voice_t *result = ctx->voice_free_list[0];
    ctx->voice_free_num--;
    for (int i = 0; i < ctx->voice_free_num; i++)
        ctx->voice_free_list[i] = ctx->voice_free_list[i + 1];
    ctx->voice_alloced_list[ctx->voice_alloced_num++] = result;
    return result;
}

static void VoiceKeyOff(midi_opl_t *ctx, opl_voice_t *voice) {
    opl_write(ctx, OPL_REGS_FREQ_2 + voice->index, voice->freq >> 8);
}

static void ReleaseVoice(midi_opl_t *ctx, int index) {
    if (index >= ctx->voice_alloced_num) {
        ctx->voice_alloced_num = 0;
        ctx->voice_free_num = 0;
        return;
    }
    opl_voice_t *voice = ctx->voice_alloced_list[index];
    VoiceKeyOff(ctx, voice);
    voice->channel = NULL;
    voice->note = 0;
    ctx->voice_alloced_num--;
    for (int i = index; i < ctx->voice_alloced_num; i++)
        ctx->voice_alloced_list[i] = ctx->voice_alloced_list[i + 1];
    ctx->voice_free_list[ctx->voice_free_num++] = voice;
}

static void LoadOperatorData(midi_opl_t *ctx, int op, genmidi_op_t *data,
                             bool max_level, unsigned int *volume) {
    int level = data->scale;
    if (max_level)
        level |= 0x3f;
    else
        level |= data->level;
    *volume = level;
    opl_write(ctx, OPL_REGS_LEVEL + op, level);
    opl_write(ctx, OPL_REGS_TREMOLO + op, data->tremolo);
    opl_write(ctx, OPL_REGS_ATTACK + op, data->attack);
    opl_write(ctx, OPL_REGS_SUSTAIN + op, data->sustain);
    opl_write(ctx, OPL_REGS_WAVEFORM + op, data->waveform);
}

static void SetVoiceInstrument(midi_opl_t *ctx, opl_voice_t *voice,
                               genmidi_instr_t *instr, unsigned int instr_voice) {
    if (voice->current_instr == instr &&
        voice->current_instr_voice == instr_voice)
        return;
    voice->current_instr = instr;
    voice->current_instr_voice = instr_voice;
    genmidi_voice_t *data = &instr->voices[instr_voice];
    bool modulating = (data->feedback & 0x01) == 0;
    LoadOperatorData(ctx, voice->op2, &data->carrier, true, &voice->car_volume);
    LoadOperatorData(ctx, voice->op1, &data->modulator, !modulating, &voice->mod_volume);
    opl_write(ctx, OPL_REGS_FEEDBACK + voice->index, data->feedback | voice->reg_pan);
    voice->priority = 0x0f - (data->carrier.attack >> 4)
                    + 0x0f - (data->carrier.sustain & 0x0f);
}

static void SetVoiceVolume(midi_opl_t *ctx, opl_voice_t *voice, unsigned int volume) {
    voice->note_volume = volume;
    genmidi_voice_t *opl_voice = &voice->current_instr->voices[voice->current_instr_voice];
    unsigned int midi_volume = 2 * (volume_mapping_table[voice->channel->volume] + 1);
    unsigned int full_volume = (volume_mapping_table[voice->note_volume] * midi_volume) >> 9;
    unsigned int car_volume = 0x3f - full_volume;
    if (car_volume != (voice->car_volume & 0x3f)) {
        voice->car_volume = car_volume | (voice->car_volume & 0xc0);
        opl_write(ctx, OPL_REGS_LEVEL + voice->op2, voice->car_volume);
        if ((opl_voice->feedback & 0x01) != 0 && opl_voice->modulator.level != 0x3f) {
            unsigned int mod_volume = opl_voice->modulator.level;
            if (mod_volume < car_volume) mod_volume = car_volume;
            mod_volume |= voice->mod_volume & 0xc0;
            if (mod_volume != voice->mod_volume) {
                voice->mod_volume = mod_volume;
                opl_write(ctx, OPL_REGS_LEVEL + voice->op1,
                          mod_volume | (opl_voice->modulator.scale & 0xc0));
            }
        }
    }
}

static unsigned int FrequencyForVoice(opl_voice_t *voice) {
    int note = voice->note;
    genmidi_voice_t *gm_voice = &voice->current_instr->voices[voice->current_instr_voice];
    if ((voice->current_instr->flags & GENMIDI_FLAG_FIXED) == 0)
        note += gm_voice->base_note_offset;
    while (note < 0) note += 12;
    while (note > 95) note -= 12;
    int freq_index = 64 + 32 * note + voice->channel->bend;
    if (voice->current_instr_voice != 0)
        freq_index += (voice->current_instr->fine_tuning / 2) - 64;
    if (freq_index < 0) freq_index = 0;
    if (freq_index < 284)
        return frequency_curve[freq_index];
    unsigned int sub_index = (freq_index - 284) % (12 * 32);
    unsigned int octave = (freq_index - 284) / (12 * 32);
    if (octave >= 7) octave = 7;
    return frequency_curve[sub_index + 284] | (octave << 10);
}

static void UpdateVoiceFrequency(midi_opl_t *ctx, opl_voice_t *voice) {
    unsigned int freq = FrequencyForVoice(voice);
    if (voice->freq != freq) {
        opl_write(ctx, OPL_REGS_FREQ_1 + voice->index, freq & 0xff);
        opl_write(ctx, OPL_REGS_FREQ_2 + voice->index, (freq >> 8) | 0x20);
        voice->freq = freq;
    }
}

static void ReplaceExistingVoice(midi_opl_t *ctx) {
    int result = 0;
    for (int i = 0; i < ctx->voice_alloced_num; i++) {
        if (ctx->voice_alloced_list[i]->current_instr_voice != 0 ||
            ctx->voice_alloced_list[i]->channel >= ctx->voice_alloced_list[result]->channel)
            result = i;
    }
    ReleaseVoice(ctx, result);
}

static void VoiceKeyOn(midi_opl_t *ctx, opl_channel_data_t *channel,
                       genmidi_instr_t *instrument, unsigned int instrument_voice,
                       unsigned int note, unsigned int key, unsigned int volume) {
    opl_voice_t *voice = GetFreeVoice(ctx);
    if (!voice) return;
    voice->channel = channel;
    voice->key = key;
    if ((instrument->flags & GENMIDI_FLAG_FIXED) != 0)
        voice->note = instrument->fixed_note;
    else
        voice->note = note;
    voice->reg_pan = channel->pan;
    SetVoiceInstrument(ctx, voice, instrument, instrument_voice);
    SetVoiceVolume(ctx, voice, volume);
    voice->freq = 0;
    UpdateVoiceFrequency(ctx, voice);
}

/* ================================================================== */
/* Channel helpers                                                    */
/* ================================================================== */

static opl_channel_data_t *TrackChannel(midi_opl_t *ctx, midi_event_t *event) {
    unsigned int ch = event->data.channel.channel;
    /* MIDI ch9 = percussion → internal ch15 (MUS convention) */
    if (ch == 9) ch = 15;
    else if (ch == 15) ch = 9;
    return &ctx->channels[ch];
}

static void InitChannel(midi_opl_t *ctx, opl_channel_data_t *channel) {
    channel->instrument = &ctx->main_instrs[0];
    channel->volume = ctx->music_volume;
    channel->volume_base = 100;
    if (channel->volume > channel->volume_base)
        channel->volume = channel->volume_base;
    channel->pan = 0x30;
    channel->bend = 0;
}

static void SetChannelVolume(midi_opl_t *ctx, opl_channel_data_t *channel,
                             unsigned int volume) {
    channel->volume_base = volume;
    if (volume > (unsigned)ctx->music_volume)
        volume = ctx->music_volume;
    channel->volume = volume;
    for (int i = 0; i < OPL_NUM_VOICES; i++) {
        if (ctx->voices[i].channel == channel)
            SetVoiceVolume(ctx, &ctx->voices[i], ctx->voices[i].note_volume);
    }
}

/* ================================================================== */
/* MIDI event handlers                                                */
/* ================================================================== */

static void KeyOffEvent(midi_opl_t *ctx, midi_event_t *event) {
    opl_channel_data_t *channel = TrackChannel(ctx, event);
    unsigned int key = event->data.channel.param1;
    for (int i = 0; i < ctx->voice_alloced_num; i++) {
        if (ctx->voice_alloced_list[i]->channel == channel &&
            ctx->voice_alloced_list[i]->key == key) {
            ReleaseVoice(ctx, i);
            i--;
        }
    }
}

static void KeyOnEvent(midi_opl_t *ctx, midi_event_t *event) {
    unsigned int note = event->data.channel.param1;
    unsigned int key = event->data.channel.param1;
    unsigned int volume = event->data.channel.param2;
    if (volume == 0) { KeyOffEvent(ctx, event); return; }

    opl_channel_data_t *channel = TrackChannel(ctx, event);
    genmidi_instr_t *instrument;

    if (event->data.channel.channel == 9) {
        if (key < 35 || key > 81) return;
        instrument = &ctx->percussion_instrs[key - 35];
        note = 60;
    } else {
        instrument = channel->instrument;
    }

    bool double_voice = (instrument->flags & GENMIDI_FLAG_2VOICE) != 0;
    if (ctx->voice_free_num == 0)
        ReplaceExistingVoice(ctx);
    VoiceKeyOn(ctx, channel, instrument, 0, note, key, volume);
    if (double_voice)
        VoiceKeyOn(ctx, channel, instrument, 1, note, key, volume);
}

static void ProgramChangeEvent(midi_opl_t *ctx, midi_event_t *event) {
    opl_channel_data_t *channel = TrackChannel(ctx, event);
    int instrument = event->data.channel.param1;
    channel->instrument = &ctx->main_instrs[instrument];
}

static void ControllerEvent(midi_opl_t *ctx, midi_event_t *event) {
    opl_channel_data_t *channel = TrackChannel(ctx, event);
    unsigned int controller = event->data.channel.param1;
    unsigned int param = event->data.channel.param2;
    switch (controller) {
        case MIDI_CONTROLLER_MAIN_VOLUME:
            SetChannelVolume(ctx, channel, param);
            break;
        case MIDI_CONTROLLER_ALL_NOTES_OFF:
            for (int i = 0; i < ctx->voice_alloced_num; i++) {
                if (ctx->voice_alloced_list[i]->channel == channel) {
                    ReleaseVoice(ctx, i);
                    i--;
                }
            }
            break;
        default:
            break;
    }
}

static void PitchBendEvent(midi_opl_t *ctx, midi_event_t *event) {
    opl_channel_data_t *channel = TrackChannel(ctx, event);
    channel->bend = event->data.channel.param2 - 64;
    for (int i = 0; i < ctx->voice_alloced_num; i++) {
        if (ctx->voice_alloced_list[i]->channel == channel)
            UpdateVoiceFrequency(ctx, ctx->voice_alloced_list[i]);
    }
}

static void ProcessEvent(midi_opl_t *ctx, midi_event_t *event) {
    switch (event->event_type) {
        case MIDI_EVENT_NOTE_OFF:    KeyOffEvent(ctx, event); break;
        case MIDI_EVENT_NOTE_ON:     KeyOnEvent(ctx, event); break;
        case MIDI_EVENT_CONTROLLER:  ControllerEvent(ctx, event); break;
        case MIDI_EVENT_PROGRAM_CHANGE: ProgramChangeEvent(ctx, event); break;
        case MIDI_EVENT_PITCH_BEND:  PitchBendEvent(ctx, event); break;
        case MIDI_EVENT_META:
            if (event->data.meta.type == MIDI_META_SET_TEMPO &&
                event->data.meta.length == 3 && event->data.meta.data) {
                uint8_t *d = event->data.meta.data;
                ctx->us_per_beat = ((unsigned)d[0] << 16) |
                                   ((unsigned)d[1] << 8) | d[2];
            }
            break;
        default:
            break;
    }
}

/* ================================================================== */
/* Song restart                                                       */
/* ================================================================== */

static void RestartSong(midi_opl_t *ctx) {
    ctx->running_tracks = ctx->num_tracks;
    for (unsigned int i = 0; i < ctx->num_tracks; i++) {
        MIDI_RestartIterator(ctx->tracks[i].iter);
        /* Schedule first event */
        unsigned int nticks = MIDI_GetDeltaTime(ctx->tracks[i].iter);
        uint64_t us = ((uint64_t)nticks * ctx->us_per_beat) / ctx->ticks_per_beat;
        ctx->track_next_time[i] = ctx->current_time + us;
    }
    for (unsigned int i = 0; i < MIDI_CHANNELS_PER_TRACK; i++)
        InitChannel(ctx, &ctx->channels[i]);
}

/* ================================================================== */
/* Process MIDI events that are due at current_time                   */
/* ================================================================== */

static void ProcessPendingEvents(midi_opl_t *ctx) {
    for (unsigned int t = 0; t < ctx->num_tracks; t++) {
        while (ctx->track_next_time[t] <= ctx->current_time) {
            midi_event_t *event;
            if (!MIDI_GetNextEvent(ctx->tracks[t].iter, &event)) {
                break;
            }
            ProcessEvent(ctx, event);

            /* End of track? */
            if (event->event_type == MIDI_EVENT_META &&
                event->data.meta.type == MIDI_META_END_OF_TRACK) {
                ctx->running_tracks--;
                ctx->track_next_time[t] = UINT64_MAX;
                if (ctx->running_tracks == 0 && ctx->looping) {
                    /* Small delay before restart (5ms) */
                    ctx->current_time += 5000;
                    RestartSong(ctx);
                }
                break;
            }

            /* Schedule next event */
            unsigned int nticks = MIDI_GetDeltaTime(ctx->tracks[t].iter);
            uint64_t us = ((uint64_t)nticks * ctx->us_per_beat) / ctx->ticks_per_beat;
            ctx->track_next_time[t] += us;
        }
    }
}

/* ================================================================== */
/* Init voices                                                        */
/* ================================================================== */

static void InitVoices(midi_opl_t *ctx) {
    ctx->voice_free_num = OPL_NUM_VOICES;
    ctx->voice_alloced_num = 0;
    for (int i = 0; i < OPL_NUM_VOICES; i++) {
        ctx->voices[i].index = i;
        ctx->voices[i].op1 = voice_operators[0][i];
        ctx->voices[i].op2 = voice_operators[1][i];
        ctx->voices[i].array = 0;
        ctx->voices[i].current_instr = NULL;
        ctx->voice_free_list[i] = &ctx->voices[i];
    }
}

/* ================================================================== */
/* Public API                                                         */
/* ================================================================== */

midi_opl_t *midi_opl_init(void) {
    midi_opl_t *ctx = calloc(1, sizeof(midi_opl_t));
    if (!ctx) return NULL;

    ctx->opl = OPL_new(3579552, MIDI_SAMPLE_RATE);
    if (!ctx->opl) { free(ctx); return NULL; }
    OPL_reset(ctx->opl);

    /* Enable waveform selection */
    opl_write(ctx, OPL_REG_WAVEFORM_ENABLE, 0x20);

    ctx->music_volume = 127;

    /* Load GENMIDI instrument data from embedded array */
    ctx->main_instrs = (genmidi_instr_t *)genmidi_data;
    ctx->percussion_instrs = ctx->main_instrs + GENMIDI_NUM_INSTRS;

    InitVoices(ctx);
    return ctx;
}

bool midi_opl_load(midi_opl_t *ctx, const char *filepath) {
    if (!ctx) return false;

    /* Free previous MIDI */
    if (ctx->midi) {
        if (ctx->tracks) {
            for (unsigned int i = 0; i < ctx->num_tracks; i++)
                MIDI_FreeIterator(ctx->tracks[i].iter);
            free(ctx->tracks);
            ctx->tracks = NULL;
        }
        free(ctx->track_next_time);
        ctx->track_next_time = NULL;
        MIDI_FreeFile(ctx->midi);
        ctx->midi = NULL;
    }

    ctx->midi = MIDI_LoadFile(filepath);
    if (!ctx->midi) return false;

    ctx->num_tracks = MIDI_NumTracks(ctx->midi);
    ctx->running_tracks = ctx->num_tracks;
    ctx->ticks_per_beat = MIDI_GetFileTimeDivision(ctx->midi);
    ctx->us_per_beat = 500000; /* Default 120 BPM */
    ctx->current_time = 0;

    ctx->tracks = calloc(ctx->num_tracks, sizeof(opl_track_data_t));
    ctx->track_next_time = calloc(ctx->num_tracks, sizeof(uint64_t));
    if (!ctx->tracks || !ctx->track_next_time) {
        MIDI_FreeFile(ctx->midi);
        ctx->midi = NULL;
        return false;
    }

    /* Reset OPL and voices */
    OPL_reset(ctx->opl);
    opl_write(ctx, OPL_REG_WAVEFORM_ENABLE, 0x20);
    InitVoices(ctx);

    for (unsigned int i = 0; i < MIDI_CHANNELS_PER_TRACK; i++)
        InitChannel(ctx, &ctx->channels[i]);

    /* Start all tracks */
    for (unsigned int i = 0; i < ctx->num_tracks; i++) {
        ctx->tracks[i].iter = MIDI_IterateTrack(ctx->midi, i);
        unsigned int nticks = MIDI_GetDeltaTime(ctx->tracks[i].iter);
        uint64_t us = ((uint64_t)nticks * ctx->us_per_beat) / ctx->ticks_per_beat;
        ctx->track_next_time[i] = us;
    }

    ctx->playing = true;
    return true;
}

int midi_opl_render(midi_opl_t *ctx, int16_t *buf, int max_frames) {
    if (!ctx || !ctx->playing || !ctx->midi) return 0;

    int total_rendered = 0;

    while (total_rendered < max_frames) {
        /* Find time of next MIDI event */
        uint64_t next_event_time = UINT64_MAX;
        for (unsigned int t = 0; t < ctx->num_tracks; t++) {
            if (ctx->track_next_time[t] < next_event_time)
                next_event_time = ctx->track_next_time[t];
        }

        /* How many samples until next event? */
        uint64_t us_to_event;
        if (next_event_time > ctx->current_time)
            us_to_event = next_event_time - ctx->current_time;
        else
            us_to_event = 0;

        unsigned int samples_to_event = (unsigned int)
            ((us_to_event * MIDI_SAMPLE_RATE + 999999) / 1000000);

        int remaining = max_frames - total_rendered;
        if (samples_to_event > (unsigned)remaining)
            samples_to_event = remaining;

        if (samples_to_event == 0 && us_to_event == 0) {
            /* Process events at current time */
            ProcessPendingEvents(ctx);
            if (ctx->running_tracks == 0 && !ctx->looping) {
                ctx->playing = false;
                break;
            }
            continue;
        }

        if (samples_to_event == 0)
            samples_to_event = 1; /* Always render at least 1 sample */

        /* Render OPL audio in chunks */
        unsigned int to_render = samples_to_event;
        if (to_render > 2048) to_render = 2048;

        OPL_calc_buffer_stereo(ctx->opl, ctx->opl_buf, to_render);

        /* Convert 32-bit packed stereo → 16-bit interleaved + amplify */
        int16_t *out = buf + total_rendered * 2;
        for (unsigned int i = 0; i < to_render; i++) {
            int32_t packed = ctx->opl_buf[i];
            int16_t left = (int16_t)(packed >> 16);
            int16_t right = (int16_t)(packed & 0xFFFF);
            /* Amplify by 8x for audible output */
            int32_t l = (int32_t)left << 3;
            int32_t r = (int32_t)right << 3;
            if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
            if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
            out[i * 2] = (int16_t)l;
            out[i * 2 + 1] = (int16_t)r;
        }

        /* Advance time */
        uint64_t us_rendered = ((uint64_t)to_render * 1000000) / MIDI_SAMPLE_RATE;
        ctx->current_time += us_rendered;
        total_rendered += to_render;

        /* Process any events that are now due */
        ProcessPendingEvents(ctx);
        if (ctx->running_tracks == 0 && !ctx->looping) {
            ctx->playing = false;
            break;
        }
    }

    return total_rendered;
}

bool midi_opl_playing(midi_opl_t *ctx) {
    return ctx && ctx->playing;
}

void midi_opl_set_loop(midi_opl_t *ctx, bool loop) {
    if (ctx) ctx->looping = loop;
}

void midi_opl_free(midi_opl_t *ctx) {
    if (!ctx) return;
    if (ctx->tracks) {
        for (unsigned int i = 0; i < ctx->num_tracks; i++)
            if (ctx->tracks[i].iter)
                MIDI_FreeIterator(ctx->tracks[i].iter);
        free(ctx->tracks);
    }
    free(ctx->track_next_time);
    if (ctx->midi) MIDI_FreeFile(ctx->midi);
    if (ctx->opl) OPL_delete(ctx->opl);
    free(ctx);
}
