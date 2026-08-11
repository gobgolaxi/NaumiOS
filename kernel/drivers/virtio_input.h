#ifndef VIRTIO_INPUT_H
#define VIRTIO_INPUT_H

#include <stdint.h>

typedef struct virtio_input_dev virtio_input_dev_t;

/* Matches Linux's struct input_event minus the timestamp — exactly what
   virtio-input puts on the wire. type: EV_SYN=0, EV_KEY=1, EV_REL=2, ...
   code: keycode (EV_KEY) or axis (EV_REL: REL_X=0, REL_Y=1). value: 1
   press/0 release/2 repeat for EV_KEY, signed delta for EV_REL. */
typedef struct {
    uint16_t type;
    uint16_t code;
    uint32_t value;
} virtio_input_event_t;

/* Finds a virtio-input device (DeviceID 18) whose config-space name
   contains `name_substr` (e.g. "Keyboard", "Mouse" — QEMU's virtio-input
   HID devices advertise names like "QEMU Virtio Keyboard"), brings it up
   (legacy virtio-mmio, one rx virtqueue with every descriptor pre-posted
   as a device-writable buffer), and returns a handle, or NULL if no
   matching device exists or init failed. */
virtio_input_dev_t *virtio_input_open(const char *name_substr);

/* Non-blocking: returns 1 and fills *out_ev if an event was waiting in the
   queue, 0 otherwise. Automatically re-posts the consumed buffer so the
   device can keep filling it. */
int virtio_input_poll(virtio_input_dev_t *dev, virtio_input_event_t *out_ev);

#endif
