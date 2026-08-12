#include "mus_player.h"
#include "audio_mixer.h"
#include "../lib/libc.h"

#define MUS_TICKS_HZ 140u /* the original DMX driver's fixed timer rate */

typedef struct {
    const uint8_t *data;
    uint32_t len;
    uint32_t scorestart;
} mus_handle_t;

static const uint8_t *g_data;
static uint32_t g_len;
static uint32_t g_pos;
static uint32_t g_scorestart;
static int g_looping;
static int g_playing;
static int32_t g_samples_remaining;
static uint32_t g_frac_carry;
static uint8_t g_channel_velocity[16];

static uint16_t read_le16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

void *mus_player_register(const uint8_t *data, uint32_t len) {
    if (!data || len < 16 || data[0] != 'M' || data[1] != 'U' ||
        data[2] != 'S' || data[3] != 0x1A) {
        return NULL;
    }
    uint32_t scorestart = read_le16(data + 6);
    if (scorestart >= len) {
        return NULL;
    }
    mus_handle_t *h = (mus_handle_t *)malloc(sizeof(mus_handle_t));
    if (!h) {
        return NULL;
    }
    h->data = data;
    h->len = len;
    h->scorestart = scorestart;
    return h;
}

void mus_player_unregister(void *handle) {
    free(handle);
}

static void silence_all(void) {
    for (int c = 0; c < 16; c++) {
        audio_mixer_channel_notes_off(c);
    }
}

void mus_player_play(void *handle, int looping) {
    if (!handle) {
        return;
    }
    mus_handle_t *h = (mus_handle_t *)handle;
    silence_all();
    g_data = h->data;
    g_len = h->len;
    g_pos = h->scorestart;
    g_scorestart = h->scorestart;
    g_looping = looping;
    g_playing = 1;
    g_samples_remaining = 0;
    g_frac_carry = 0;
    for (int c = 0; c < 16; c++) {
        g_channel_velocity[c] = 127;
        audio_mixer_set_channel_volume(c, 127);
    }
}

void mus_player_stop(void) {
    g_playing = 0;
    silence_all();
}

int mus_player_is_playing(void) {
    return g_playing;
}

/* Processes one block of events (everything up to and including the event
   whose descriptor byte has bit 7 set). Returns 1 if the caller should now
   read a delay value from the stream, 0 if playback stopped or looped
   back to the start instead (score-end, truncated data, or a corrupt
   event code) — in either 0 case there's no delay to read. */
static int process_block(void) {
    for (;;) {
        if (g_pos >= g_len) {
            g_playing = 0;
            return 0;
        }
        uint8_t desc = g_data[g_pos++];
        int channel = desc & 0x0F;
        int event = desc & 0x70;

        switch (event) {
        case 0x00: { /* release key */
            if (g_pos >= g_len) { g_playing = 0; return 0; }
            uint8_t key = g_data[g_pos++];
            if (channel != 15) {
                audio_mixer_note_off(channel, key & 0x7F);
            }
            break;
        }
        case 0x10: { /* press key */
            if (g_pos >= g_len) { g_playing = 0; return 0; }
            uint8_t key = g_data[g_pos++];
            if (key & 0x80) {
                if (g_pos >= g_len) { g_playing = 0; return 0; }
                g_channel_velocity[channel] = g_data[g_pos++] & 0x7F;
            }
            if (channel != 15) {
                audio_mixer_note_on(channel, key & 0x7F, g_channel_velocity[channel]);
            }
            break;
        }
        case 0x20: /* pitch wheel — consumed, not applied (no pitch bend in this synth) */
            if (g_pos >= g_len) { g_playing = 0; return 0; }
            g_pos++;
            break;
        case 0x30: /* system event (all sounds/notes off, mono/poly, reset) */
            if (g_pos >= g_len) { g_playing = 0; return 0; }
            g_pos++;
            audio_mixer_channel_notes_off(channel);
            break;
        case 0x40: { /* change controller */
            if (g_pos + 1 >= g_len) { g_playing = 0; return 0; }
            uint8_t ctrlnum = g_data[g_pos++];
            uint8_t val = g_data[g_pos++];
            if (ctrlnum == 3) { /* channel volume */
                audio_mixer_set_channel_volume(channel, val & 0x7F);
            }
            /* patch/pan/expression/etc: no timbre variety in this synth. */
            break;
        }
        case 0x60: /* score end */
            silence_all();
            if (g_looping) {
                g_pos = g_scorestart;
            } else {
                g_playing = 0;
            }
            return 0;
        default:
            g_playing = 0;
            return 0;
        }

        if (desc & 0x80) {
            return 1;
        }
    }
}

void mus_player_advance(uint32_t nsamples) {
    if (!g_playing) {
        return;
    }

    int32_t remaining = g_samples_remaining - (int32_t)nsamples;

    while (g_playing && remaining <= 0) {
        int need_delay = process_block();
        if (!g_playing || !need_delay) {
            continue; /* looped back to scorestart, or stopped */
        }

        uint32_t delay_ticks = 0;
        for (;;) {
            if (g_pos >= g_len) {
                g_playing = 0;
                break;
            }
            uint8_t b = g_data[g_pos++];
            delay_ticks = delay_ticks * 128u + (b & 0x7Fu);
            if (!(b & 0x80)) {
                break;
            }
        }
        if (!g_playing) {
            break;
        }

        uint32_t total = delay_ticks * MIXER_SAMPLE_RATE + g_frac_carry;
        remaining += (int32_t)(total / MUS_TICKS_HZ);
        g_frac_carry = total % MUS_TICKS_HZ;
    }

    g_samples_remaining = g_playing ? remaining : 0;
}
