#include <stdint.h>
#include "../lib/naumi.h"
#include "../lib/libc.h"
#include "../../third_party/doomgeneric/doomgeneric.h"
#include "../../third_party/doomgeneric/doomkeys.h"
#include "../../third_party/doomgeneric/i_sound.h"
#include "../../third_party/doomgeneric/w_wad.h"
#include "../../third_party/doomgeneric/z_zone.h"
#include "audio_mixer.h"
#include "mus_player.h"

/* The platform layer doomgeneric asks every port to provide (see
   third_party/doomgeneric/doomgeneric.h) — everything else is the actual
   engine, unmodified. DG_sound_module/DG_music_module below are registered
   via the RAISKAOS_SOUND build define (root Makefile) — a pre-existing
   hook in third_party/doomgeneric/i_sound.h/i_sound.c for exactly this,
   not something patched in.

   kernel/drivers/virtio_sound.c is a single-voice, fire-and-forget stream
   — it can't run a music voice and a sound-effect voice at once. So both
   sound effects (dg_sound_start below) and music (mus_player.c, driven by
   a direct MUS-format player — no MIDI/OPL involved) feed into
   audio_mixer.c's software mixer instead of the driver directly; one
   mixed chunk is rendered and pushed via sys_audio_play() every game tic
   (see dg_sound_update() below), which is the only thing that actually
   calls into the driver. */

/* evdev keycode -> DOOM key value. Extends the letters/digits/enter/
   backspace/space table userland/console/main.c already established with
   the arrows/ctrl/shift/alt DOOM's defaults actually bind movement and
   firing to. */
#define KC_ESC 1
#define KC_BACKSPACE 14
#define KC_TAB 15
#define KC_ENTER 28
#define KC_LCTRL 29
#define KC_LSHIFT 42
#define KC_RSHIFT 54
#define KC_LALT 56
#define KC_SPACE 57
#define KC_UP 103
#define KC_LEFT 105
#define KC_RIGHT 106
#define KC_DOWN 108

static const char ASCII_FROM_CODE[58] = {
    /*0*/ 0, 0, '1', '2', '3', '4', '5', '6', '7', '8',
    /*10*/ '9', '0', '-', '=', 0, 0, 'q', 'w', 'e', 'r',
    /*20*/ 't', 'y', 'u', 'i', 'o', 'p', '[', ']', 0, 0,
    /*30*/ 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',
    /*40*/ '\'', '`', 0, '\\', 'z', 'x', 'c', 'v', 'b', 'n',
    /*50*/ 'm', ',', '.', '/', 0, 0, 0, ' ',
};

static unsigned char evdev_to_doomkey(unsigned short code) {
    switch (code) {
    case KC_ESC: return KEY_ESCAPE;
    case KC_ENTER: return KEY_ENTER;
    case KC_BACKSPACE: return KEY_BACKSPACE;
    case KC_TAB: return KEY_TAB;
    case KC_LCTRL: return KEY_FIRE;
    case KC_LSHIFT: case KC_RSHIFT: return KEY_RSHIFT;
    case KC_LALT: return KEY_RALT;
    case KC_SPACE: return KEY_USE;
    case KC_UP: return KEY_UPARROW;
    case KC_DOWN: return KEY_DOWNARROW;
    case KC_LEFT: return KEY_LEFTARROW;
    case KC_RIGHT: return KEY_RIGHTARROW;
    default:
        if (code < sizeof(ASCII_FROM_CODE) && ASCII_FROM_CODE[code]) {
            return (unsigned char)ASCII_FROM_CODE[code];
        }
        return 0;
    }
}

void DG_Init(void) {
    sys_win_create(DOOMGENERIC_RESX, DOOMGENERIC_RESY, 0, "DOOM");
    audio_mixer_init();
}

/* Window's current client size — starts equal to DOOM's native 320x200
   render resolution (see the DOOMGENERIC_RESX/RESY build defines in the
   root Makefile) and only differs after a maximize/restore toggle updates
   it via the SYS_EV_RESIZE event DG_GetKey() watches for below. DOOM
   itself always renders at the fixed native resolution — there's no way
   to ask it for more detail — so scaling up is a plain nearest-neighbor
   stretch of the finished 320x200 frame, not a higher-resolution render. */
static uint32_t g_target_w = DOOMGENERIC_RESX;
static uint32_t g_target_h = DOOMGENERIC_RESY;
static uint32_t *g_scale_buf;
static uint32_t g_scale_buf_cap;

void DG_DrawFrame(void) {
    if (g_target_w == DOOMGENERIC_RESX && g_target_h == DOOMGENERIC_RESY) {
        sys_fb_blit(0, 0, DOOMGENERIC_RESX, DOOMGENERIC_RESY, (const unsigned int *)DG_ScreenBuffer);
        sys_fb_present();
        return;
    }

    uint32_t need = g_target_w * g_target_h;
    if (need > g_scale_buf_cap) {
        free(g_scale_buf);
        g_scale_buf = (uint32_t *)malloc((size_t)need * sizeof(uint32_t));
        g_scale_buf_cap = g_scale_buf ? need : 0;
    }
    if (!g_scale_buf) {
        /* Scale buffer allocation failed — fall back to the native size
           rather than not drawing at all. */
        sys_fb_blit(0, 0, DOOMGENERIC_RESX, DOOMGENERIC_RESY, (const unsigned int *)DG_ScreenBuffer);
        sys_fb_present();
        return;
    }

    const uint32_t *src = (const uint32_t *)DG_ScreenBuffer;
    for (uint32_t y = 0; y < g_target_h; y++) {
        uint32_t sy = (y * DOOMGENERIC_RESY) / g_target_h;
        const uint32_t *srow = src + sy * DOOMGENERIC_RESX;
        uint32_t *drow = g_scale_buf + y * g_target_w;
        for (uint32_t x = 0; x < g_target_w; x++) {
            uint32_t sx = (x * DOOMGENERIC_RESX) / g_target_w;
            drow[x] = srow[sx];
        }
    }
    sys_fb_blit(0, 0, g_target_w, g_target_h, g_scale_buf);
    sys_fb_present();
}

void DG_SleepMs(uint32_t ms) {
    sys_sleep_ms(ms); /* real block — see naumi.h */
}

uint32_t DG_GetTicksMs(void) {
    return (uint32_t)sys_get_ticks_ms();
}

int DG_GetKey(int *pressed, unsigned char *key) {
    /* third_party/doomgeneric/i_input.c drains this in a `while
       (DG_GetKey(...))` loop once per tic — resize/unmapped events have to
       be consumed and skipped internally rather than returning 0 for them,
       or a resize sitting ahead of a real keypress in the queue would
       stall that keypress until the next tic (the while loop would stop
       right there, reading "no more events" instead of "not a key"). */
    struct input_event_wire ev;
    for (;;) {
        if (sys_poll_input(0, &ev) <= 0) {
            return 0;
        }
        if (ev.type == EV_RESIZE) {
            g_target_w = (uint32_t)ev.x;
            g_target_h = (uint32_t)ev.y;
            continue;
        }
        if (ev.type != EV_KEY) {
            continue;
        }
        unsigned char mapped = evdev_to_doomkey((unsigned short)ev.code);
        if (!mapped) {
            continue;
        }
        *pressed = ev.value != 0;
        *key = mapped;
        return 1;
    }
}

void DG_SetWindowTitle(const char *title) {
    (void)title;
}

/* ---- sound module: DOOM's own sound effect lumps are already exactly the
   format virtio_sound.c's stream is fixed to (U8 mono 11025Hz) — see
   virtio_sound.h — so playback is a matter of stripping the DMX sound lump
   header/padding and handing the raw bytes to SYS_AUDIO_PLAY, no
   resampling or format conversion. The header layout and the 16-byte
   leading/trailing padding this strips are DMX's actual on-disk format
   (id Software's original sound driver, used unchanged since — every
   Doom-engine WAD's sound lumps look like this), not something specific
   to this port. ---- */

static boolean dg_sound_init(boolean use_sfx_prefix) {
    (void)use_sfx_prefix;
    return true; /* virtio_sound_init() already ran at boot; nothing per-process to set up */
}

static void dg_sound_shutdown(void) {
}

static int dg_sound_get_lump_num(sfxinfo_t *sfxinfo) {
    char name[9];
    name[0] = 'D';
    name[1] = 'S';
    int i = 0;
    for (; sfxinfo->name[i] && i < 6; i++) {
        name[2 + i] = sfxinfo->name[i];
    }
    name[2 + i] = '\0';
    int lump = W_CheckNumForName(name);
    return lump >= 0 ? lump : 0;
}

/* Drives the whole software audio pipeline — called once per game tic
   (I_UpdateSound(), from S_UpdateSounds() in the main loop) regardless of
   whether music or any sound effect is actually active: advances the MUS
   player by however much wall-clock time has actually elapsed, mixes that
   many samples of music + pending one-shot sound effects, and pushes the
   result to the driver. sys_audio_play() is fire-and-forget and single-
   voice — if the previous chunk hasn't finished playing yet this one is
   just dropped (an occasional dropped ~tic's worth of audio, not a
   silence-forever stall). */
static uint32_t g_audio_last_ms;
static int g_audio_started;

static void dg_sound_update(void) {
    uint32_t now = (uint32_t)sys_get_ticks_ms();
    if (!g_audio_started) {
        g_audio_last_ms = now;
        g_audio_started = 1;
        return;
    }

    uint32_t elapsed_ms = now - g_audio_last_ms;
    if (elapsed_ms == 0) {
        return;
    }
    if (elapsed_ms > 200) {
        elapsed_ms = 200; /* clamp a stall (e.g. level load) rather than bursting */
    }
    g_audio_last_ms = now;

    uint32_t nsamples = (elapsed_ms * MIXER_SAMPLE_RATE) / 1000u;
    if (nsamples == 0) {
        return;
    }
    if (nsamples > 4096) {
        nsamples = 4096;
    }

    mus_player_advance(nsamples);

    static uint8_t chunk[4096];
    audio_mixer_render(chunk, nsamples);
    sys_audio_play(chunk, nsamples);
}

static void dg_sound_update_params(int channel, int vol, int sep) {
    (void)channel; (void)vol; (void)sep;
}

static int dg_sound_start(sfxinfo_t *sfxinfo, int channel, int vol, int sep) {
    (void)sep;
    if (!sfxinfo || vol <= 0) {
        return channel;
    }
    if (sfxinfo->lumpnum < 0) {
        sfxinfo->lumpnum = dg_sound_get_lump_num(sfxinfo);
    }

    const uint8_t *data = (const uint8_t *)W_CacheLumpNum(sfxinfo->lumpnum, PU_CACHE);
    uint32_t lumplen = (uint32_t)W_LumpLength((unsigned int)sfxinfo->lumpnum);
    if (!data || lumplen < 8 || data[0] != 0x03 || data[1] != 0x00) {
        return channel; /* not a DMX PCM lump (format tag != 3) */
    }

    uint32_t length = (uint32_t)data[4] | ((uint32_t)data[5] << 8) |
                       ((uint32_t)data[6] << 16) | ((uint32_t)data[7] << 24);
    if (length == 0 || length > lumplen - 8 || length <= 32) {
        return channel;
    }

    /* 8-byte header (format+rate+length) + 16 bytes of leading pad, and
       16 more trailing pad counted in `length` — DMX always includes
       both regardless of the lump's actual sample rate. Queued into the
       software mixer (audio_mixer.c), not sent to the driver directly —
       see dg_sound_update() above for why. */
    audio_mixer_play_sfx(data + 24, length - 32);
    return channel;
}

static void dg_sound_stop(int channel) {
    (void)channel;
}

static boolean dg_sound_is_playing(int channel) {
    (void)channel;
    return false; /* single-voice driver never reports a channel as still busy */
}

static void dg_sound_cache(sfxinfo_t *sounds, int num_sounds) {
    (void)sounds; (void)num_sounds;
}

static snddevice_t dg_sound_devices[] = { SNDDEVICE_SB };

sound_module_t DG_sound_module = {
    dg_sound_devices,
    1,
    dg_sound_init,
    dg_sound_shutdown,
    dg_sound_get_lump_num,
    dg_sound_update,
    dg_sound_update_params,
    dg_sound_start,
    dg_sound_stop,
    dg_sound_is_playing,
    dg_sound_cache,
};

/* MUS-format music, played through mus_player.c straight into
   audio_mixer.c's software synth (see dg_sound_update() above for the
   actual per-tic render/submit step — Poll() here has nothing left to do
   once that's driving things every tic regardless). No pause/resume or
   volume control: DOOM calls these on menu open/game pause, but skipping
   them just means music keeps playing through the menu/pause screen
   rather than ducking — a fidelity gap, not a crash risk. */
static snddevice_t dg_music_devices[] = { SNDDEVICE_SB };
static boolean dg_music_init(void) { return true; }
static void dg_music_shutdown(void) { mus_player_stop(); }
static void dg_music_set_volume(int volume) { (void)volume; }
static void dg_music_pause(void) { }
static void dg_music_resume(void) { }

static void *dg_music_register(void *data, int len) {
    return mus_player_register((const uint8_t *)data, (uint32_t)len);
}

static void dg_music_unregister(void *handle) {
    mus_player_unregister(handle);
}

static void dg_music_play(void *handle, boolean looping) {
    mus_player_play(handle, looping ? 1 : 0);
}

static void dg_music_stop(void) {
    mus_player_stop();
}

static boolean dg_music_is_playing(void) {
    return mus_player_is_playing() ? true : false;
}

static void dg_music_poll(void) { }

music_module_t DG_music_module = {
    dg_music_devices,
    1,
    dg_music_init,
    dg_music_shutdown,
    dg_music_set_volume,
    dg_music_pause,
    dg_music_resume,
    dg_music_register,
    dg_music_unregister,
    dg_music_play,
    dg_music_stop,
    dg_music_is_playing,
    dg_music_poll,
};

static char *fake_argv[] = { "doom", "-iwad", "doom1.wad", NULL };

void _start(void) {
    /* This doomgeneric fork's D_DoomLoop() (third_party/doomgeneric/d_main.c)
       runs setup and exactly one doomgeneric_Tick() call, then returns —
       unlike vanilla DOOM's D_DoomLoop, it does not loop forever itself.
       doomgeneric.h exports doomgeneric_Tick() specifically so the
       platform drives the outer loop; every other backend in this tree
       does the same (see e.g. doomgeneric_sdl.c's own `while (1)`). */
    doomgeneric_Create(3, fake_argv);
    for (;;) {
        doomgeneric_Tick();
    }
}
