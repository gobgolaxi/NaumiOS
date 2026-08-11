#ifndef INPUT_H
#define INPUT_H

#include "virtio_input.h"

/* Opens the keyboard and mouse virtio-input devices (if present) and
   prints what it found. Safe to call even if one or both are missing —
   input_poll_keyboard()/input_poll_mouse() just always report "nothing
   waiting" for a device that isn't there. */
void input_init(void);

int input_poll_keyboard(virtio_input_event_t *out_ev);
int input_poll_mouse(virtio_input_event_t *out_ev);

#endif
