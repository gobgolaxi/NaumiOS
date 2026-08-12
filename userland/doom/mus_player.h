#ifndef NAUMI_MUS_PLAYER_H
#define NAUMI_MUS_PLAYER_H

#include <stdint.h>

/* Direct MUS-format player (id Software's DMX music format, used by every
   Doom-engine WAD) — deliberately skips the mus2mid.c round trip already
   in this tree: MUS's own event stream is simpler than general MIDI, and
   this player only ever needs note-on/off/volume, so parsing it straight
   into audio_mixer.c calls is less code than converting to MIDI bytes and
   then parsing those back out. Percussion (MUS channel 15) is silently
   ignored — DOOM's built-in scores barely use it, and a real drum kit is
   out of scope for the square-wave synth in audio_mixer.c.

   Timing: MUS delay values count "MUS ticks" at a fixed 140 Hz (the
   original DMX driver's timer rate) — see mus_player_advance(). */

/* Validates the MUS header (magic + scorestart) and returns an opaque
   handle for mus_player_play()/mus_player_unregister(), or NULL if `data`
   doesn't look like a MUS lump. Does not copy `data` — same convention as
   every other backend's RegisterSong (see s_sound.c: the WAD lump is
   cached PU_STATIC, so the pointer stays valid for as long as the handle
   does). */
void *mus_player_register(const uint8_t *data, uint32_t len);
void mus_player_unregister(void *handle);

/* Only one song plays at a time (matches DOOM's own single mus_playing
   channel) — starting a new one implicitly stops whatever was playing. */
void mus_player_play(void *handle, int looping);
void mus_player_stop(void);
int mus_player_is_playing(void);

/* Advances playback by `nsamples` (at MIXER_SAMPLE_RATE) worth of time,
   issuing audio_mixer_note_on/off/etc calls for every event crossed. Call
   this right before audio_mixer_render() covering the same nsamples, once
   per game tic — see doomgeneric_naumios.c's dg_music_poll(). */
void mus_player_advance(uint32_t nsamples);

#endif
