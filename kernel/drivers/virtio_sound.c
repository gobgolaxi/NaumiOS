#include <stdint.h>
#include <stddef.h>
#include "virtio_sound.h"
#include "virtio_mmio.h"
#include "uart.h"
#include "../mm/pmm.h"
#include "../mm/heap.h"

/* Legacy (version 1) virtio-mmio, same register set already confirmed for
   virtio-blk/virtio-input on this machine — see virtio_blk.c. */
#define REG_GUEST_FEATURES_SEL 0x24
#define REG_GUEST_FEATURES     0x20
#define REG_GUEST_PAGE_SIZE    0x28
#define REG_QUEUE_SEL          0x30
#define REG_QUEUE_NUM_MAX      0x34
#define REG_QUEUE_NUM          0x38
#define REG_QUEUE_ALIGN        0x3C
#define REG_QUEUE_PFN          0x40
#define REG_QUEUE_NOTIFY       0x50
#define REG_STATUS             0x70
#define REG_CONFIG             0x100

#define STATUS_ACKNOWLEDGE 1U
#define STATUS_DRIVER      2U
#define STATUS_DRIVER_OK   4U

#define VIRTQ_DESC_F_NEXT  1U
#define VIRTQ_DESC_F_WRITE 2U

#define VIRTIO_ID_SOUND 25U

#define QSIZE 8U /* descriptor slots per queue — small, fixed, well under any QueueNumMax */

/* Only the two queues this driver actually uses: 0 = controlq (device
   enumeration + stream setup), 2 = txq (playback data). 1 = eventq and
   3 = rxq are legal to leave entirely unconfigured (QueuePFN 0, "not in
   use") — nothing here needs jack/stream-change notifications or capture. */
#define QUEUE_CONTROL 0U
#define QUEUE_TX      2U

/* enum virtio_snd_ctl_msg (VIRTIO 1.2 §5.14.6.1) — only the PCM control
   codes this driver issues. */
#define VIRTIO_SND_R_PCM_SET_PARAMS 0x0101U
#define VIRTIO_SND_R_PCM_PREPARE    0x0102U
#define VIRTIO_SND_R_PCM_START      0x0104U

#define VIRTIO_SND_S_OK 0x8000U

#define VIRTIO_SND_PCM_FMT_U8 4U
#define VIRTIO_SND_PCM_RATE_11025 2U

#define STREAM_ID 0U
#define PLAY_RATE_HZ 11025U /* matches VIRTIO_SND_PCM_RATE_11025 above */

/* Generous fixed ceiling for one sound effect's raw PCM payload — DOOM's
   shareware SFX lumps are all well under this (a few KB to ~30KB at
   11025 Hz). One shared buffer (see virtio_sound_play()): this driver is
   single-voice, only one clip is ever in flight at a time. */
#define MAX_CLIP_BYTES (128U * 1024U)

typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} virtq_desc_t;

typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[QSIZE];
} virtq_avail_t;

typedef struct __attribute__((packed)) {
    uint32_t id;
    uint32_t len;
} virtq_used_elem_t;

typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t idx;
    virtq_used_elem_t ring[QSIZE];
} virtq_used_t;

typedef struct __attribute__((packed)) {
    uint32_t jacks;
    uint32_t streams;
    uint32_t chmaps;
} virtio_snd_config_t;

typedef struct __attribute__((packed)) {
    uint32_t code;
} virtio_snd_hdr_t;

typedef struct __attribute__((packed)) {
    virtio_snd_hdr_t hdr;
    uint32_t stream_id;
} virtio_snd_pcm_hdr_t;

typedef struct __attribute__((packed)) {
    virtio_snd_pcm_hdr_t hdr;
    uint32_t buffer_bytes;
    uint32_t period_bytes;
    uint32_t features;
    uint8_t channels;
    uint8_t format;
    uint8_t rate;
    uint8_t padding;
} virtio_snd_pcm_set_params_t;

typedef struct __attribute__((packed)) {
    uint32_t stream_id;
} virtio_snd_pcm_xfer_hdr_t;

typedef struct __attribute__((packed)) {
    uint32_t status;
    uint32_t latency_bytes;
} virtio_snd_pcm_status_t;

/* One virtqueue's worth of state — the control and tx queues each get
   their own instance, same shape (same legacy layout), just used
   differently. */
typedef struct {
    volatile virtq_desc_t *desc;
    volatile virtq_avail_t *avail;
    volatile virtq_used_t *used;
    uint16_t last_used_idx;
} vqueue_t;

static uint64_t mmio_base;
static uint64_t hhdm;
static vqueue_t control_q;
static vqueue_t tx_q;
static int sound_ready;
static int tx_in_flight; /* true between submitting a clip and observing its used-ring completion */

/* Reused for every control request (only one ever in flight — driver
   init is strictly sequential) and for the single in-flight tx clip. */
static void *ctl_req_buf;
static void *ctl_resp_buf;
static uint8_t *tx_clip_buf;          /* xfer header + PCM, one combined allocation */
static virtio_snd_pcm_status_t *tx_status_buf;

static inline uint32_t mmio_read32(uint32_t off) {
    return *(volatile uint32_t *)(mmio_base + off);
}

static inline void mmio_write32(uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(mmio_base + off) = val;
}

static inline uint64_t phys_of(const void *ptr) {
    return (uint64_t)ptr - hhdm;
}

static int setup_queue(uint32_t queue_idx, vqueue_t *q) {
    mmio_write32(REG_QUEUE_SEL, queue_idx);
    uint32_t max = mmio_read32(REG_QUEUE_NUM_MAX);
    if (max == 0 || QSIZE > max) {
        uart_puts("virtio_sound: queue "); uart_puthex(queue_idx); uart_puts(" unavailable\n");
        return -1;
    }
    mmio_write32(REG_QUEUE_NUM, QSIZE);
    mmio_write32(REG_QUEUE_ALIGN, (uint32_t)PAGE_SIZE);

    uint64_t page0 = pmm_alloc_page();
    uint64_t page1 = pmm_alloc_page();
    if (page0 == 0 || page1 == 0 || page1 != page0 + PAGE_SIZE) {
        uart_puts("virtio_sound: no contiguous pages for queue "); uart_puthex(queue_idx); uart_puts("\n");
        return -1;
    }

    q->desc = (volatile virtq_desc_t *)(hhdm + page0);
    q->avail = (volatile virtq_avail_t *)((uint8_t *)q->desc + sizeof(virtq_desc_t) * QSIZE);
    q->used = (volatile virtq_used_t *)(hhdm + page1);
    q->last_used_idx = 0;

    mmio_write32(REG_QUEUE_PFN, (uint32_t)(page0 / PAGE_SIZE));
    return 0;
}

/* Synchronous control request: submit hdr (+ optional trailing bytes
   already part of the same buffer)/response pair on controlq, notify,
   poll until the device answers. Bounded so a device that never responds
   can't hang boot forever. Returns 0 on a VIRTIO_SND_S_OK response. */
static int control_roundtrip(const void *req, uint32_t req_len, void *resp, uint32_t resp_len) {
    volatile virtq_desc_t *desc = control_q.desc;
    volatile virtq_avail_t *avail = control_q.avail;
    volatile virtq_used_t *used = control_q.used;

    desc[0].addr = phys_of(req);
    desc[0].len = req_len;
    desc[0].flags = VIRTQ_DESC_F_NEXT;
    desc[0].next = 1;

    desc[1].addr = phys_of(resp);
    desc[1].len = resp_len;
    desc[1].flags = VIRTQ_DESC_F_WRITE;
    desc[1].next = 0;

    __asm__ volatile ("dsb sy" ::: "memory");
    uint16_t avail_idx = avail->idx;
    avail->ring[avail_idx % QSIZE] = 0;
    __asm__ volatile ("dsb sy" ::: "memory");
    avail->idx = avail_idx + 1;
    __asm__ volatile ("dsb sy" ::: "memory");
    mmio_write32(REG_QUEUE_NOTIFY, QUEUE_CONTROL);

    uint32_t spins = 0;
    while (used->idx == control_q.last_used_idx) {
        spins++;
        if (spins > 100000000U) {
            uart_puts("virtio_sound: control request timed out\n");
            return -1;
        }
    }
    __asm__ volatile ("dsb sy" ::: "memory");
    control_q.last_used_idx = used->idx;

    uint32_t status = *(volatile uint32_t *)resp;
    if (status != VIRTIO_SND_S_OK) {
        uart_puts("virtio_sound: control request failed, status="); uart_puthex(status); uart_puts("\n");
        return -1;
    }
    return 0;
}

int virtio_sound_init(void) {
    hhdm = pmm_hhdm_offset();

    mmio_base = virtio_mmio_find_device(VIRTIO_ID_SOUND);
    if (mmio_base == 0) {
        uart_puts("virtio_sound: no device found\n");
        return -1;
    }

    mmio_write32(REG_STATUS, 0);
    mmio_write32(REG_STATUS, STATUS_ACKNOWLEDGE);
    mmio_write32(REG_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER);

    mmio_write32(REG_GUEST_FEATURES_SEL, 0);
    mmio_write32(REG_GUEST_FEATURES, 0);

    mmio_write32(REG_GUEST_PAGE_SIZE, (uint32_t)PAGE_SIZE);

    if (setup_queue(QUEUE_CONTROL, &control_q) != 0 || setup_queue(QUEUE_TX, &tx_q) != 0) {
        return -1;
    }

    ctl_req_buf = kmalloc(sizeof(virtio_snd_pcm_set_params_t));
    ctl_resp_buf = kmalloc(sizeof(virtio_snd_hdr_t));
    tx_clip_buf = (uint8_t *)kmalloc(sizeof(virtio_snd_pcm_xfer_hdr_t) + MAX_CLIP_BYTES);
    tx_status_buf = (virtio_snd_pcm_status_t *)kmalloc(sizeof(virtio_snd_pcm_status_t));
    if (!ctl_req_buf || !ctl_resp_buf || !tx_clip_buf || !tx_status_buf) {
        uart_puts("virtio_sound: kmalloc failed for driver buffers\n");
        return -1;
    }

    mmio_write32(REG_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_DRIVER_OK);

    /* SET_PARAMS: U8 mono @ 11025 Hz, stream 0 — see virtio_sound.h for
       why this needs no format/rate conversion against DOOM's own sound
       lumps. buffer_bytes/period_bytes are generous fixed sizes; this
       driver only ever has one clip in flight so the exact period
       granularity the device reports progress at doesn't matter to us. */
    virtio_snd_pcm_set_params_t *params = (virtio_snd_pcm_set_params_t *)ctl_req_buf;
    params->hdr.hdr.code = VIRTIO_SND_R_PCM_SET_PARAMS;
    params->hdr.stream_id = STREAM_ID;
    params->buffer_bytes = MAX_CLIP_BYTES;
    params->period_bytes = MAX_CLIP_BYTES;
    params->features = 0;
    params->channels = 1;
    params->format = VIRTIO_SND_PCM_FMT_U8;
    params->rate = VIRTIO_SND_PCM_RATE_11025;
    params->padding = 0;
    if (control_roundtrip(params, sizeof(*params), ctl_resp_buf, sizeof(virtio_snd_hdr_t)) != 0) {
        uart_puts("virtio_sound: SET_PARAMS failed\n");
        return -1;
    }

    virtio_snd_pcm_hdr_t *pcm_hdr = (virtio_snd_pcm_hdr_t *)ctl_req_buf;
    pcm_hdr->hdr.code = VIRTIO_SND_R_PCM_PREPARE;
    pcm_hdr->stream_id = STREAM_ID;
    if (control_roundtrip(pcm_hdr, sizeof(*pcm_hdr), ctl_resp_buf, sizeof(virtio_snd_hdr_t)) != 0) {
        uart_puts("virtio_sound: PREPARE failed\n");
        return -1;
    }

    pcm_hdr->hdr.code = VIRTIO_SND_R_PCM_START;
    pcm_hdr->stream_id = STREAM_ID;
    if (control_roundtrip(pcm_hdr, sizeof(*pcm_hdr), ctl_resp_buf, sizeof(virtio_snd_hdr_t)) != 0) {
        uart_puts("virtio_sound: START failed\n");
        return -1;
    }

    sound_ready = 1;
    uart_puts("virtio_sound: ready, base "); uart_puthex(mmio_base); uart_puts("\n");
    return 0;
}

int virtio_sound_play(const uint8_t *pcm, uint32_t len) {
    if (!sound_ready || len == 0) {
        return -1;
    }
    if (len > MAX_CLIP_BYTES) {
        len = MAX_CLIP_BYTES; /* clip rather than refuse — a truncated sound beats none */
    }

    volatile virtq_desc_t *desc = tx_q.desc;
    volatile virtq_avail_t *avail = tx_q.avail;
    volatile virtq_used_t *used = tx_q.used;

    /* Single-voice: if the previous clip hasn't completed yet, don't
       clobber tx_clip_buf out from under an in-flight DMA — drop the new
       one instead of blocking the caller for however long playback takes.
       used->idx alone can't tell "nothing ever submitted" apart from
       "submitted, still pending" (both read 0), hence the explicit flag. */
    if (tx_in_flight) {
        if (used->idx == tx_q.last_used_idx) {
            return -1; /* previous clip still playing */
        }
        __asm__ volatile ("dsb sy" ::: "memory");
        tx_q.last_used_idx = used->idx; /* it finished since we last checked; reclaim the buffer */
        tx_in_flight = 0;
    }

    virtio_snd_pcm_xfer_hdr_t *xfer = (virtio_snd_pcm_xfer_hdr_t *)tx_clip_buf;
    xfer->stream_id = STREAM_ID;
    uint8_t *payload = tx_clip_buf + sizeof(virtio_snd_pcm_xfer_hdr_t);
    for (uint32_t i = 0; i < len; i++) {
        payload[i] = pcm[i];
    }

    desc[0].addr = phys_of(tx_clip_buf);
    desc[0].len = (uint32_t)sizeof(virtio_snd_pcm_xfer_hdr_t) + len;
    desc[0].flags = VIRTQ_DESC_F_NEXT;
    desc[0].next = 1;

    desc[1].addr = phys_of(tx_status_buf);
    desc[1].len = sizeof(virtio_snd_pcm_status_t);
    desc[1].flags = VIRTQ_DESC_F_WRITE;
    desc[1].next = 0;

    __asm__ volatile ("dsb sy" ::: "memory");
    uint16_t avail_idx = avail->idx;
    avail->ring[avail_idx % QSIZE] = 0;
    __asm__ volatile ("dsb sy" ::: "memory");
    avail->idx = avail_idx + 1;
    __asm__ volatile ("dsb sy" ::: "memory");
    mmio_write32(REG_QUEUE_NOTIFY, QUEUE_TX);

    tx_in_flight = 1;
    return 0;
}
