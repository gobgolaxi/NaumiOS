#include <stddef.h>
#include "input.h"
#include "uart.h"

static virtio_input_dev_t *kbd_dev;
static virtio_input_dev_t *mouse_dev;

void input_init(void) {
    kbd_dev = virtio_input_open("Keyboard");
    /* An absolute pointer (QEMU's virtio-tablet-device), not a relative
       mouse: reports EV_ABS ABS_X/ABS_Y in a fixed 0..32767 logical range
       regardless of the actual display size, so the compositor's cursor
       tracks the real pointer 1:1 no matter how the host window is scaled
       (e.g. QEMU running fullscreen and stretching the framebuffer to fit
       a larger physical monitor). A relative device can't do that: its
       deltas are physical-mouse-movement-based, so under a scaled display
       the guest cursor either can't reach the logical screen edges or
       overshoots them well before the visible window edge — see
       compositor.c's mouse handling. */
    mouse_dev = virtio_input_open("Tablet");

    uart_puts("input: keyboard "); uart_puts(kbd_dev ? "found" : "not found"); uart_puts("\n");
    uart_puts("input: tablet "); uart_puts(mouse_dev ? "found" : "not found"); uart_puts("\n");
}

int input_poll_keyboard(virtio_input_event_t *out_ev) {
    return kbd_dev ? virtio_input_poll(kbd_dev, out_ev) : 0;
}

int input_poll_mouse(virtio_input_event_t *out_ev) {
    return mouse_dev ? virtio_input_poll(mouse_dev, out_ev) : 0;
}
