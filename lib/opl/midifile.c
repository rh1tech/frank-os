/*
 * MIDI file parser — adapted from Chocolate Doom / murmdoom
 * Copyright(C) 2005-2014 Simon Howard
 * Copyright(C) 2021-2022 Graham Sanderson
 *
 * Stripped of Doom dependencies.  Uses FatFS for file I/O.
 * True streaming: reads one event at a time from the file,
 * no event buffer allocation.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdlib.h>
#include <string.h>
#include "ff.h"
#include "midifile.h"

/* Byte-swap helpers (MIDI is big-endian) */
static inline uint16_t swap16(uint16_t v) {
    return (uint16_t)((v >> 8) | (v << 8));
}
static inline uint32_t swap32(uint32_t v) {
    return ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) |
           ((v << 8) & 0xFF0000) | ((v << 24) & 0xFF000000u);
}

#define HEADER_CHUNK_ID "MThd"
#define TRACK_CHUNK_ID  "MTrk"

#pragma pack(push, 1)
typedef struct {
    uint8_t  chunk_id[4];
    uint32_t chunk_size;
} chunk_header_t;

typedef struct {
    chunk_header_t chunk_header;
    uint16_t format_type;
    uint16_t num_tracks;
    uint16_t time_division;
} midi_header_t;
#pragma pack(pop)

typedef struct {
    unsigned int data_len;

    /* Ping-pong event buffers: events[next_idx] = lookahead,
     * events[next_idx ^ 1] = last returned (still valid for caller) */
    midi_event_t  events[2];
    uint8_t       next_idx;     /* index of the lookahead event */
    bool          has_next;

    /* File streaming state */
    FSIZE_t      file_pos;
    FSIZE_t      initial_file_pos;
    unsigned int last_event_type;
    bool         end_of_track;
} midi_track_t;

struct midi_file_s {
    midi_header_t header;
    midi_track_t *tracks;
    unsigned int  num_tracks;

    /* Keep file open for streaming */
    FIL           fil;
    bool          fil_open;
};

struct midi_track_iter_s {
    midi_track_t *track;
    midi_file_t  *file;
    unsigned int  track_num;
};

/* ------------------------------------------------------------------ */
/* FatFS-based byte reading helpers                                    */
/* ------------------------------------------------------------------ */

static bool ReadByte(uint8_t *result, FIL *f) {
    UINT br;
    if (f_read(f, result, 1, &br) != FR_OK || br != 1)
        return false;
    return true;
}

static bool ReadVariableLength(uint32_t *result, FIL *f) {
    uint8_t b;
    *result = 0;
    for (int i = 0; i < 4; i++) {
        if (!ReadByte(&b, f))
            return false;
        *result = (*result << 7) | (b & 0x7f);
        if ((b & 0x80) == 0)
            return true;
    }
    return false;
}

static void *ReadByteSequence(unsigned int num_bytes, FIL *f) {
    uint8_t *result = malloc(num_bytes + 1);
    if (!result) return NULL;
    UINT br;
    if (f_read(f, result, num_bytes, &br) != FR_OK || br != num_bytes) {
        free(result);
        return NULL;
    }
    return result;
}

/* ------------------------------------------------------------------ */
/* Event readers                                                       */
/* ------------------------------------------------------------------ */

static bool ReadChannelEvent(midi_event_t *event, uint8_t event_type,
                             bool two_param, FIL *f) {
    uint8_t b;
    event->event_type = (midi_event_type_t)(event_type & 0xf0);
    event->data.channel.channel = event_type & 0x0f;
    if (!ReadByte(&b, f)) return false;
    event->data.channel.param1 = b;
    if (two_param) {
        if (!ReadByte(&b, f)) return false;
        event->data.channel.param2 = b;
    } else {
        event->data.channel.param2 = 0;
    }
    return true;
}

static bool ReadSysExEvent(midi_event_t *event, int event_type, FIL *f) {
    uint32_t length;
    event->event_type = (midi_event_type_t)event_type;
    if (!ReadVariableLength(&length, f)) return false;
    event->data.sysex.length = length;
    event->data.sysex.data = NULL;
    if (length > 0)
        f_lseek(f, f_tell(f) + length);
    return true;
}

static bool ReadMetaEvent(midi_event_t *event, FIL *f) {
    uint8_t b;
    uint32_t length;
    event->event_type = MIDI_EVENT_META;
    if (!ReadByte(&b, f)) return false;
    event->data.meta.type = b;
    if (!ReadVariableLength(&length, f)) return false;
    event->data.meta.length = length;
    /* Only store data for SET_TEMPO events */
    if (b == MIDI_META_SET_TEMPO && length == 3) {
        event->data.meta.data = ReadByteSequence(length, f);
        if (!event->data.meta.data) return false;
    } else {
        event->data.meta.data = NULL;
        if (length > 0)
            f_lseek(f, f_tell(f) + length);
    }
    return true;
}

static bool ReadEvent(midi_event_t *event, unsigned int *last_event_type,
                      FIL *f) {
    uint8_t event_type;
    if (!ReadVariableLength(&event->delta_time, f)) return false;
    if (!ReadByte(&event_type, f)) return false;

    if ((event_type & 0x80) == 0) {
        event_type = (uint8_t)*last_event_type;
        f_lseek(f, f_tell(f) - 1);
    } else {
        *last_event_type = event_type;
    }

    switch (event_type & 0xf0) {
        case MIDI_EVENT_NOTE_OFF:
        case MIDI_EVENT_NOTE_ON:
        case MIDI_EVENT_AFTERTOUCH:
        case MIDI_EVENT_CONTROLLER:
        case MIDI_EVENT_PITCH_BEND:
            return ReadChannelEvent(event, event_type, true, f);
        case MIDI_EVENT_PROGRAM_CHANGE:
        case MIDI_EVENT_CHAN_AFTERTOUCH:
            return ReadChannelEvent(event, event_type, false, f);
        default:
            break;
    }
    switch (event_type) {
        case MIDI_EVENT_SYSEX:
        case MIDI_EVENT_SYSEX_SPLIT:
            return ReadSysExEvent(event, event_type, f);
        case MIDI_EVENT_META:
            return ReadMetaEvent(event, f);
        default:
            break;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Free meta/sysex data from a single event                            */
/* ------------------------------------------------------------------ */

static void FreeEventData(midi_event_t *event) {
    if (event->event_type == MIDI_EVENT_META) {
        free(event->data.meta.data);
        event->data.meta.data = NULL;
    } else if (event->event_type == MIDI_EVENT_SYSEX ||
               event->event_type == MIDI_EVENT_SYSEX_SPLIT) {
        free(event->data.sysex.data);
        event->data.sysex.data = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* Read the next event for a track into the lookahead slot             */
/* ------------------------------------------------------------------ */

static void ReadAheadEvent(midi_file_t *file, midi_track_t *track) {
    if (track->end_of_track || !file->fil_open) {
        track->has_next = false;
        return;
    }

    midi_event_t *ev = &track->events[track->next_idx];

    /* Free any previous data in this slot */
    FreeEventData(ev);

    /* Seek to this track's read position */
    f_lseek(&file->fil, track->file_pos);

    memset(ev, 0, sizeof(*ev));
    if (!ReadEvent(ev, &track->last_event_type, &file->fil)) {
        track->has_next = false;
        track->end_of_track = true;
        return;
    }

    /* Save file position for next read */
    track->file_pos = f_tell(&file->fil);
    track->has_next = true;

    if (ev->event_type == MIDI_EVENT_META &&
        ev->data.meta.type == MIDI_META_END_OF_TRACK) {
        track->end_of_track = true;
    }
}

/* ------------------------------------------------------------------ */
/* Track header reading                                                */
/* ------------------------------------------------------------------ */

static bool ReadTrackHeader(midi_track_t *track, FIL *f) {
    chunk_header_t hdr;
    UINT br;
    if (f_read(f, &hdr, sizeof(hdr), &br) != FR_OK || br != sizeof(hdr))
        return false;
    if (memcmp(hdr.chunk_id, TRACK_CHUNK_ID, 4) != 0)
        return false;
    track->data_len = swap32(hdr.chunk_size);
    return true;
}

/* ------------------------------------------------------------------ */
/* Reset a track's streaming state                                     */
/* ------------------------------------------------------------------ */

static void ResetTrack(midi_file_t *file, midi_track_t *track) {
    FreeEventData(&track->events[0]);
    FreeEventData(&track->events[1]);
    memset(track->events, 0, sizeof(track->events));
    track->next_idx = 0;
    track->has_next = false;
    track->file_pos = track->initial_file_pos;
    track->last_event_type = 0;
    track->end_of_track = false;

    /* Read first event into lookahead */
    ReadAheadEvent(file, track);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

midi_file_t *MIDI_LoadFile(const char *filename) {
    midi_file_t *file = calloc(1, sizeof(midi_file_t));
    if (!file) return NULL;

    if (f_open(&file->fil, filename, FA_READ | FA_OPEN_EXISTING) != FR_OK) {
        free(file);
        return NULL;
    }
    file->fil_open = true;

    /* Read MIDI header */
    UINT br;
    if (f_read(&file->fil, &file->header, sizeof(midi_header_t), &br) != FR_OK ||
        br != sizeof(midi_header_t)) {
        goto fail;
    }
    if (memcmp(file->header.chunk_header.chunk_id, HEADER_CHUNK_ID, 4) != 0)
        goto fail;

    file->num_tracks = swap16(file->header.num_tracks);
    if (file->num_tracks == 0)
        goto fail;

    file->tracks = calloc(file->num_tracks, sizeof(midi_track_t));
    if (!file->tracks) goto fail;

    /* Read each track header and record file position */
    for (unsigned int i = 0; i < file->num_tracks; i++) {
        midi_track_t *track = &file->tracks[i];
        if (!ReadTrackHeader(track, &file->fil))
            goto fail;
        track->initial_file_pos = f_tell(&file->fil);
        track->file_pos = track->initial_file_pos;
        /* Skip track data (will be streamed event by event) */
        f_lseek(&file->fil, f_tell(&file->fil) + track->data_len);
    }

    return file;

fail:
    if (file->tracks) free(file->tracks);
    if (file->fil_open) f_close(&file->fil);
    free(file);
    return NULL;
}

void MIDI_FreeFile(midi_file_t *file) {
    if (!file) return;
    if (file->tracks) {
        for (unsigned int i = 0; i < file->num_tracks; i++) {
            FreeEventData(&file->tracks[i].events[0]);
            FreeEventData(&file->tracks[i].events[1]);
        }
        free(file->tracks);
    }
    if (file->fil_open)
        f_close(&file->fil);
    free(file);
}

unsigned int MIDI_GetFileTimeDivision(midi_file_t *file) {
    return swap16(file->header.time_division);
}

unsigned int MIDI_NumTracks(midi_file_t *file) {
    return file->num_tracks;
}

midi_track_iter_t *MIDI_IterateTrack(midi_file_t *file, unsigned int track_num) {
    if (track_num >= file->num_tracks) return NULL;
    midi_track_iter_t *iter = calloc(1, sizeof(midi_track_iter_t));
    if (!iter) return NULL;
    iter->track = &file->tracks[track_num];
    iter->file = file;
    iter->track_num = track_num;

    /* Reset and read first event */
    ResetTrack(file, iter->track);

    return iter;
}

void MIDI_FreeIterator(midi_track_iter_t *iter) {
    free(iter);
}

unsigned int MIDI_GetDeltaTime(midi_track_iter_t *iter) {
    if (!iter->track->has_next)
        return 0;
    return iter->track->events[iter->track->next_idx].delta_time;
}

int MIDI_GetNextEvent(midi_track_iter_t *iter, midi_event_t **event) {
    midi_track_t *track = iter->track;
    if (!track->has_next)
        return 0;

    /* Return pointer to current lookahead event */
    uint8_t cur_idx = track->next_idx;
    *event = &track->events[cur_idx];

    /* Toggle to the other slot for the next lookahead */
    track->next_idx ^= 1;

    /* Read ahead the next event into the new slot */
    if (!track->end_of_track) {
        ReadAheadEvent(iter->file, track);
    } else {
        track->has_next = false;
    }

    return 1;
}

void MIDI_RestartIterator(midi_track_iter_t *iter) {
    ResetTrack(iter->file, iter->track);
}
