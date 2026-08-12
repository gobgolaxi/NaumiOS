#ifndef VIRTIO_SOUND_H
#define VIRTIO_SOUND_H

#include <stdint.h>

/* Finds a virtio-sound device (DeviceID 25), brings up its control and tx
   virtqueues (legacy virtio-mmio, same conventions as virtio_blk.c/
   virtio_input.c), and configures/prepares/starts PCM stream 0 as U8
   (unsigned 8-bit) mono at 11025 Hz — the exact format DOOM's own sound
   lumps are already stored in (see doomgeneric_naumios.c's DMX header
   parsing), so playback needs no resampling/format conversion at all.
   Returns 0 on success, -1 if no device is present or setup failed —
   callers should treat that as "no sound", not a fatal error. */
int virtio_sound_init(void);

/* Submits `len` bytes of U8 mono 11025Hz PCM for playback. Fire-and-forget,
   single-voice: if a previous clip is still in flight (hasn't shown up in
   the used ring yet), this drops the new one and returns -1 rather than
   blocking the caller for the clip's duration or corrupting an in-flight
   DMA buffer. Returns 0 if the clip was submitted. */
int virtio_sound_play(const uint8_t *pcm, uint32_t len);

#endif
