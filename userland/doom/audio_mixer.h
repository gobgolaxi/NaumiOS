#ifndef NAUMI_AUDIO_MIXER_H
#define NAUMI_AUDIO_MIXER_H

#include <stdint.h>

/* Software mixer feeding the single fire-and-forget virtio-sound stream
   (see kernel/drivers/virtio_sound.c — one voice, U8 mono 11025Hz). Since
   the device can't run a music voice and an SFX voice at once, everything
   — MUS-driven square-wave oscillators and one-shot DMX sound effects —
   is summed here in software into one continuous PCM stream, which
   mus_player.c/doomgeneric_naumios.c then push out via sys_audio_play()
   once per game tic. No floating point anywhere (this build is
   -mgeneral-regs-only): oscillator phase runs in Q16.16 fixed point,
   driven by a precomputed per-MIDI-note phase-step table. */

#define MIXER_SAMPLE_RATE 11025u

void audio_mixer_init(void);

/* channel: MUS channel 0-15 (15 is percussion — silently ignored, see
   mus_player.c). note: MIDI note number 0-127. velocity: 0-127. */
void audio_mixer_note_on(int channel, int note, int velocity);
void audio_mixer_note_off(int channel, int note);
void audio_mixer_channel_notes_off(int channel);
void audio_mixer_set_channel_volume(int channel, int volume);

/* Queues a one-shot PCM clip (already-stripped DMX sound data, U8 mono) for
   playback — copies it into an internally owned buffer since the WAD cache
   entry it usually points at may be reused/purged before playback finishes.
   Silently drops the clip if every SFX voice slot is already busy (matches
   the original single-voice driver's "some overlapping sounds get
   dropped" behavior, just with more headroom). */
void audio_mixer_play_sfx(const uint8_t *pcm, uint32_t len);

/* Mixes `nsamples` of U8 PCM into `out` (advancing every active
   oscillator/SFX voice by that many samples). Always fills the whole
   buffer (silence, i.e. 128, where nothing is playing). */
void audio_mixer_render(uint8_t *out, uint32_t nsamples);

#endif
