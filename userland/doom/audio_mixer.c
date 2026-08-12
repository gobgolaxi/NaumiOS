#include "audio_mixer.h"
#include "../lib/libc.h"

/* Phase-step per MIDI note (0-127) in Q16.16 fixed point, i.e.
   round(440 * 2^((note-69)/12) * 65536 / MIXER_SAMPLE_RATE) — equal
   temperament, A4=440Hz, precomputed offline (no floating point in this
   freestanding, soft-float-less build). */
static const uint32_t NOTE_PHASE_STEP[128] = {
    49u, 51u, 55u, 58u, 61u, 65u, 69u, 73u,
    77u, 82u, 87u, 92u, 97u, 103u, 109u, 116u,
    122u, 130u, 137u, 146u, 154u, 163u, 173u, 183u,
    194u, 206u, 218u, 231u, 245u, 259u, 275u, 291u,
    309u, 327u, 346u, 367u, 389u, 412u, 436u, 462u,
    490u, 519u, 550u, 583u, 617u, 654u, 693u, 734u,
    778u, 824u, 873u, 925u, 980u, 1038u, 1100u, 1165u,
    1234u, 1308u, 1386u, 1468u, 1555u, 1648u, 1746u, 1849u,
    1959u, 2076u, 2199u, 2330u, 2469u, 2615u, 2771u, 2936u,
    3110u, 3295u, 3491u, 3699u, 3919u, 4152u, 4399u, 4660u,
    4937u, 5231u, 5542u, 5872u, 6221u, 6591u, 6983u, 7398u,
    7838u, 8304u, 8797u, 9321u, 9875u, 10462u, 11084u, 11743u,
    12441u, 13181u, 13965u, 14795u, 15675u, 16607u, 17595u, 18641u,
    19750u, 20924u, 22168u, 23486u, 24883u, 26363u, 27930u, 29591u,
    31351u, 33215u, 35190u, 37282u, 39499u, 41848u, 44336u, 46973u,
    49766u, 52725u, 55860u, 59182u, 62701u, 66429u, 70380u, 74565u,
};

#define MAX_MUS_VOICES 32
typedef struct {
    int active;
    int channel;
    int note;
    int velocity;
    uint32_t phase; /* Q16.16 */
} mus_voice_t;

static mus_voice_t g_voices[MAX_MUS_VOICES];
static int g_channel_volume[16]; /* 0-127, default 127 (set in audio_mixer_init) */

#define MAX_SFX_VOICES 8
typedef struct {
    int active;
    uint8_t *pcm; /* owned, malloc'd copy */
    uint32_t len;
    uint32_t pos;
} sfx_voice_t;

static sfx_voice_t g_sfx[MAX_SFX_VOICES];

void audio_mixer_init(void) {
    for (int i = 0; i < MAX_MUS_VOICES; i++) {
        g_voices[i].active = 0;
    }
    for (int i = 0; i < MAX_SFX_VOICES; i++) {
        g_sfx[i].active = 0;
        g_sfx[i].pcm = NULL;
    }
    for (int c = 0; c < 16; c++) {
        g_channel_volume[c] = 127;
    }
}

void audio_mixer_note_on(int channel, int note, int velocity) {
    if (note < 0 || note > 127 || velocity <= 0) {
        return;
    }
    /* Retrigger: if this exact (channel, note) is already sounding, reuse
       its slot instead of stacking a second identical oscillator. */
    for (int i = 0; i < MAX_MUS_VOICES; i++) {
        if (g_voices[i].active && g_voices[i].channel == channel && g_voices[i].note == note) {
            g_voices[i].velocity = velocity;
            return;
        }
    }
    for (int i = 0; i < MAX_MUS_VOICES; i++) {
        if (!g_voices[i].active) {
            g_voices[i].active = 1;
            g_voices[i].channel = channel;
            g_voices[i].note = note;
            g_voices[i].velocity = velocity;
            g_voices[i].phase = 0;
            return;
        }
    }
    /* All voices busy — drop the note (matches dropping overlapping
       sounds elsewhere in this port rather than stealing a voice). */
}

void audio_mixer_note_off(int channel, int note) {
    for (int i = 0; i < MAX_MUS_VOICES; i++) {
        if (g_voices[i].active && g_voices[i].channel == channel && g_voices[i].note == note) {
            g_voices[i].active = 0;
        }
    }
}

void audio_mixer_channel_notes_off(int channel) {
    for (int i = 0; i < MAX_MUS_VOICES; i++) {
        if (g_voices[i].active && g_voices[i].channel == channel) {
            g_voices[i].active = 0;
        }
    }
}

void audio_mixer_set_channel_volume(int channel, int volume) {
    if (channel < 0 || channel > 15) {
        return;
    }
    if (volume < 0) volume = 0;
    if (volume > 127) volume = 127;
    g_channel_volume[channel] = volume;
}

void audio_mixer_play_sfx(const uint8_t *pcm, uint32_t len) {
    if (!pcm || len == 0) {
        return;
    }
    for (int i = 0; i < MAX_SFX_VOICES; i++) {
        if (!g_sfx[i].active) {
            uint8_t *copy = (uint8_t *)malloc(len);
            if (!copy) {
                return;
            }
            memcpy(copy, pcm, len);
            g_sfx[i].active = 1;
            g_sfx[i].pcm = copy;
            g_sfx[i].len = len;
            g_sfx[i].pos = 0;
            return;
        }
    }
    /* Every SFX slot busy — drop this one. */
}

/* Target headroom for the music voices combined, leaving room below the
   U8 mixer's +-128 ceiling for SFX on top (see the SFX loop below) — a
   fixed per-voice amplitude clipped constantly once more than a handful
   of notes were held at once (DOOM's title theme alone regularly stacks
   8+ simultaneous notes across channels), so this is divided across
   however many voices are actually active for THIS render() call rather
   than being a flat per-voice constant. */
#define MUSIC_BUDGET 110
#define MUSIC_VOICE_AMP_MAX 20
#define MUSIC_VOICE_AMP_MIN 3

void audio_mixer_render(uint8_t *out, uint32_t nsamples) {
    int active_count = 0;
    for (int v = 0; v < MAX_MUS_VOICES; v++) {
        if (g_voices[v].active) {
            active_count++;
        }
    }
    int32_t base_amp = 0;
    if (active_count > 0) {
        base_amp = MUSIC_BUDGET / active_count;
        if (base_amp > MUSIC_VOICE_AMP_MAX) base_amp = MUSIC_VOICE_AMP_MAX;
        if (base_amp < MUSIC_VOICE_AMP_MIN) base_amp = MUSIC_VOICE_AMP_MIN;
    }

    for (uint32_t i = 0; i < nsamples; i++) {
        int32_t acc = 0;

        for (int v = 0; v < MAX_MUS_VOICES; v++) {
            mus_voice_t *mv = &g_voices[v];
            if (!mv->active) {
                continue;
            }
            uint32_t step = NOTE_PHASE_STEP[mv->note];
            mv->phase += step;
            /* Plain square wave: one full cycle is 0x10000 (the table is
               Q16.16 — phase-step = freq * 65536 / MIXER_SAMPLE_RATE), so
               bit 15 flips exactly at the half-cycle point. Cheap, and
               gives DOOM's MIDI lines a period-appropriate "chiptune"
               character rather than needing a wavetable. */
            int32_t amp = (base_amp * mv->velocity * g_channel_volume[mv->channel]) / (127 * 127);
            acc += (mv->phase & 0x8000u) ? -amp : amp;
        }

        for (int s = 0; s < MAX_SFX_VOICES; s++) {
            sfx_voice_t *sv = &g_sfx[s];
            if (!sv->active) {
                continue;
            }
            acc += (int32_t)sv->pcm[sv->pos] - 128;
            sv->pos++;
            if (sv->pos >= sv->len) {
                sv->active = 0;
                free(sv->pcm);
                sv->pcm = NULL;
            }
        }

        if (acc > 127) acc = 127;
        if (acc < -128) acc = -128;
        out[i] = (uint8_t)(acc + 128);
    }
}
