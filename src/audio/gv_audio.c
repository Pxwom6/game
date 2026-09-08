#include "gv_audio.h"

#include <SDL.h>
#include <stdlib.h>
#include <string.h>

#include "../core/gv_math.h"
#include "../core/gv_rng.h"

#define GV_SAMPLE_RATE 48000
#define GV_AUDIO_BUFFER 1024
#define GV_MAX_VOICES 48

/* Soft-clip ceiling. The mix is deliberately allowed to run hot into the
 * limiter — a dense wave of explosions should feel loud — but never past it. */
#define GV_MIX_CEILING 0.92f

typedef enum {
    GV_OSC_SINE = 0,
    GV_OSC_SAW,
    GV_OSC_SQUARE,
    GV_OSC_TRIANGLE,
    GV_OSC_NOISE
} gv_osc_kind;

typedef struct {
    bool active;
    uint8_t osc;
    bool music;          /* mixed through the music bus rather than the SFX bus */

    float phase;
    float freq;
    float freq_target;   /* pitch glides toward this, giving sweeps and drops */
    float glide;         /* per-second rate of approach */

    float amp;
    float env;
    float attack;        /* seconds */
    float decay;         /* seconds to fall to zero after the peak */
    float t;
    float life;

    float pan;           /* -1 left, +1 right */
    float lp;            /* one-pole low-pass state */
    float cutoff;        /* 0..1 */
    uint32_t noise;

    /* Ordering key for voice stealing: the oldest quiet voice goes first. */
    uint32_t serial;
} gv_voice;

struct gv_audio {
    SDL_AudioDeviceID device;
    SDL_AudioSpec spec;
    bool active;
    char status[128];

    gv_voice voices[GV_MAX_VOICES];
    uint32_t next_serial;

    float master, sfx_gain, music_gain;
    bool music_enabled;

    /* Music sequencer state, advanced inside the audio callback. */
    double step_time;    /* seconds accumulated in the current 16th */
    int step;            /* 0..15 within the bar */
    int bar;
    float intensity;     /* 0..1, set from the main thread */
    float intensity_smoothed;
    float music_fade;    /* eases the soundtrack in and out of menus */
    float music_fade_target;

    /* Limiter state. */
    float limiter_gain;

    gv_rng rng;
    bool in_menu;
};

/* ------------------------------------------------------------------ helpers */

static float gv_midi_hz(float note)
{
    return 440.0f * powf(2.0f, (note - 69.0f) / 12.0f);
}

static uint32_t gv_noise_next(uint32_t *s)
{
    /* xorshift32: cheap, and its spectrum is flat enough for percussion. */
    uint32_t x = *s ? *s : 0x1234567u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

static float gv_osc_sample(gv_voice *v)
{
    switch (v->osc) {
    case GV_OSC_SINE:
        return sinf(v->phase * GV_TAU);
    case GV_OSC_SAW:
        return v->phase * 2.0f - 1.0f;
    case GV_OSC_SQUARE:
        return (v->phase < 0.5f) ? 1.0f : -1.0f;
    case GV_OSC_TRIANGLE:
        return (v->phase < 0.5f) ? (v->phase * 4.0f - 1.0f) : (3.0f - v->phase * 4.0f);
    case GV_OSC_NOISE:
    default:
        return (float)(gv_noise_next(&v->noise) >> 8) * (1.0f / 8388608.0f) - 1.0f;
    }
}

/* Allocate a voice, stealing the least valuable one when the pool is full. */
static gv_voice *gv_voice_alloc(gv_audio *a)
{
    gv_voice *best = NULL;
    for (int i = 0; i < GV_MAX_VOICES; ++i) {
        if (!a->voices[i].active) {
            best = &a->voices[i];
            break;
        }
    }
    if (!best) {
        /* Steal the quietest voice; if several tie, the oldest. */
        float quietest = 1e30f;
        for (int i = 0; i < GV_MAX_VOICES; ++i) {
            gv_voice *v = &a->voices[i];
            float loudness = v->env * v->amp;
            if (loudness < quietest) {
                quietest = loudness;
                best = v;
            }
        }
    }
    if (!best) return NULL;

    memset(best, 0, sizeof(*best));
    best->active = true;
    best->serial = ++a->next_serial;
    best->noise = 0x9E3779B9u ^ (best->serial * 2654435761u);
    best->cutoff = 1.0f;
    return best;
}

/* Trigger a note. Callable from the main thread; the caller holds the lock. */
static void gv_note(gv_audio *a, int osc, float freq, float freq_target, float glide,
                    float amp, float attack, float decay, float pan, float cutoff, bool music)
{
    gv_voice *v = gv_voice_alloc(a);
    if (!v) return;

    v->osc = (uint8_t)osc;
    v->freq = gv_clampf(freq, 12.0f, 18000.0f);
    v->freq_target = gv_clampf(freq_target > 0.0f ? freq_target : freq, 12.0f, 18000.0f);
    v->glide = gv_maxf(0.0f, glide);
    v->amp = gv_clampf(amp, 0.0f, 1.5f);
    v->attack = gv_maxf(0.0005f, attack);
    v->decay = gv_maxf(0.01f, decay);
    v->life = v->attack + v->decay;
    v->pan = gv_clampf(pan, -1.0f, 1.0f);
    v->cutoff = gv_clampf(cutoff, 0.01f, 1.0f);
    v->music = music;
}

static void gv_note_locked(gv_audio *a, int osc, float freq, float freq_target, float glide,
                           float amp, float attack, float decay, float pan, float cutoff)
{
    if (!a) return;
    if (a->active) SDL_LockAudioDevice(a->device);
    gv_note(a, osc, freq, freq_target, glide, amp, attack, decay, pan, cutoff, false);
    if (a->active) SDL_UnlockAudioDevice(a->device);
}

/* -------------------------------------------------------------- the mixer */

/* One 16th-note of the soundtrack.
 *
 * The music is a four-bar loop in A minor whose density and brightness follow
 * `intensity`, so the arena getting dangerous is something you hear before you
 * finish reading the screen. */
static void gv_music_step(gv_audio *a)
{
    if (!a->music_enabled || a->music_fade <= 0.001f) return;

    float intensity = a->intensity_smoothed;
    /* Roots of i - VI - III - VII in A minor. */
    static const float k_roots[4] = { 45.0f, 41.0f, 48.0f, 43.0f };
    static const int k_pentatonic[5] = { 0, 3, 5, 7, 10 };

    float root = k_roots[a->bar & 3];
    float gain = a->music_fade;

    /* Bass on the half-beat: the spine of the loop. */
    if ((a->step % 4) == 0) {
        float note = root - 12.0f;
        gv_note(a, GV_OSC_SAW, gv_midi_hz(note), gv_midi_hz(note), 0.0f,
                0.30f * gain, 0.006f, 0.34f, 0.0f, 0.18f + 0.22f * intensity, true);
    }

    /* Kick on beats 1 and 3, a pitch drop into a short noise transient. */
    if (a->step == 0 || a->step == 8) {
        gv_note(a, GV_OSC_SINE, 130.0f, 44.0f, 26.0f, 0.42f * gain, 0.002f, 0.22f, 0.0f, 1.0f,
                true);
        gv_note(a, GV_OSC_NOISE, 800.0f, 800.0f, 0.0f, 0.09f * gain, 0.001f, 0.045f, 0.0f, 0.6f,
                true);
    }

    /* Hats fill in as things heat up. */
    if (intensity > 0.25f && (a->step % 2) == 1) {
        float amp = 0.05f * gain * gv_clampf((intensity - 0.25f) / 0.5f, 0.0f, 1.0f);
        gv_note(a, GV_OSC_NOISE, 6000.0f, 6000.0f, 0.0f, amp, 0.001f, 0.035f,
                (a->step % 4 == 1) ? -0.35f : 0.35f, 1.0f, true);
    }

    /* Arpeggio: sparse when calm, running sixteenths when the arena is full. */
    float arp_chance = 0.18f + 0.62f * intensity;
    if (gv_rng_chance(&a->rng, arp_chance)) {
        int degree = k_pentatonic[gv_rng_below(&a->rng, 5)];
        float octave = 12.0f * (float)gv_rng_range_i(&a->rng, 1, 2);
        float note = root + (float)degree + octave;
        float pan = gv_rng_range_f(&a->rng, -0.55f, 0.55f);
        gv_note(a, GV_OSC_TRIANGLE, gv_midi_hz(note), gv_midi_hz(note), 0.0f,
                (0.09f + 0.07f * intensity) * gain, 0.004f, 0.20f, pan, 0.85f, true);
    }

    /* A slow detuned pad, refreshed once per bar. */
    if (a->step == 0) {
        for (int i = 0; i < 2; ++i) {
            float detune = (i == 0) ? -0.09f : 0.09f;
            gv_note(a, GV_OSC_SAW, gv_midi_hz(root + detune), gv_midi_hz(root + detune), 0.0f,
                    0.07f * gain, 0.35f, 1.7f, (i == 0) ? -0.6f : 0.6f,
                    0.10f + 0.30f * intensity, true);
        }
    }
}

static void SDLCALL gv_audio_callback(void *userdata, Uint8 *stream, int len)
{
    gv_audio *a = (gv_audio *)userdata;
    float *out = (float *)stream;
    int frames = len / (int)(sizeof(float) * 2);

    memset(stream, 0, (size_t)len);

    const float dt = 1.0f / (float)GV_SAMPLE_RATE;
    /* Tempo lifts slightly with intensity — subtle, but it lands. */
    float bpm = 104.0f + 16.0f * a->intensity_smoothed;
    double step_len = 60.0 / (double)bpm / 4.0; /* one sixteenth */

    for (int i = 0; i < frames; ++i) {
        /* --- sequencer --- */
        a->step_time += dt;
        if (a->step_time >= step_len) {
            a->step_time -= step_len;
            a->step++;
            if (a->step >= 16) {
                a->step = 0;
                a->bar++;
            }
            gv_music_step(a);
        }

        /* Ease intensity and the menu fade at audio rate so neither steps. */
        a->intensity_smoothed += (a->intensity - a->intensity_smoothed) * gv_minf(1.0f, dt * 1.5f);
        a->music_fade += (a->music_fade_target - a->music_fade) * gv_minf(1.0f, dt * 2.5f);

        /* --- voices --- */
        float left = 0.0f, right = 0.0f;
        for (int vi = 0; vi < GV_MAX_VOICES; ++vi) {
            gv_voice *v = &a->voices[vi];
            if (!v->active) continue;

            v->t += dt;
            if (v->t >= v->life) {
                v->active = false;
                continue;
            }

            /* Attack then exponential-ish decay. */
            if (v->t < v->attack) {
                v->env = v->t / v->attack;
            } else {
                float d = (v->t - v->attack) / v->decay;
                v->env = (1.0f - d) * (1.0f - d);
            }

            if (v->glide > 0.0f) {
                v->freq += (v->freq_target - v->freq) * gv_minf(1.0f, v->glide * dt);
            }

            v->phase += v->freq * dt;
            if (v->phase >= 1.0f) v->phase -= floorf(v->phase);

            float s = gv_osc_sample(v);

            /* One-pole low-pass; the cutoff is what separates a bass note from
             * a click when both are the same oscillator. */
            v->lp += (s - v->lp) * v->cutoff;
            s = v->lp;

            s *= v->env * v->amp;
            s *= v->music ? a->music_gain : a->sfx_gain;

            /* Constant-power panning. */
            float p = (v->pan + 1.0f) * 0.5f;
            left += s * sqrtf(1.0f - p);
            right += s * sqrtf(p);
        }

        left *= a->master;
        right *= a->master;

        /* Feed-forward limiter: pull the gain down quickly when the mix peaks
         * and let it recover slowly, so a screen full of explosions ducks
         * instead of clipping into a buzz. */
        float peak = gv_maxf(gv_absf(left), gv_absf(right));
        float target = (peak > GV_MIX_CEILING) ? (GV_MIX_CEILING / peak) : 1.0f;
        if (target < a->limiter_gain) {
            a->limiter_gain = target; /* attack: immediate */
        } else {
            a->limiter_gain += (target - a->limiter_gain) * gv_minf(1.0f, dt * 2.0f);
        }
        left *= a->limiter_gain;
        right *= a->limiter_gain;

        /* Final safety clamp. A NaN from any single voice would otherwise
         * reach the device as a full-scale click. */
        if (!isfinite(left)) left = 0.0f;
        if (!isfinite(right)) right = 0.0f;
        out[i * 2 + 0] = gv_clampf(left, -1.0f, 1.0f);
        out[i * 2 + 1] = gv_clampf(right, -1.0f, 1.0f);
    }
}

/* ------------------------------------------------------------------ device */

gv_audio *gv_audio_create(void)
{
    gv_audio *a = (gv_audio *)calloc(1, sizeof(gv_audio));
    if (!a) return NULL;

    a->master = 0.8f;
    a->sfx_gain = 1.0f;
    a->music_gain = 0.75f;
    a->music_enabled = true;
    a->music_fade = 0.0f;
    a->music_fade_target = 1.0f;
    a->limiter_gain = 1.0f;
    gv_rng_seed(&a->rng, 0xA0D10u, 31u);

    SDL_AudioSpec want;
    SDL_zero(want);
    want.freq = GV_SAMPLE_RATE;
    want.format = AUDIO_F32SYS;
    want.channels = 2;
    want.samples = GV_AUDIO_BUFFER;
    want.callback = gv_audio_callback;
    want.userdata = a;

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        snprintf(a->status, sizeof(a->status), "audio subsystem: %s", SDL_GetError());
        return a; /* silent, but valid */
    }

    a->device = SDL_OpenAudioDevice(NULL, 0, &want, &a->spec, 0);
    if (a->device == 0) {
        snprintf(a->status, sizeof(a->status), "no audio device: %s", SDL_GetError());
        return a;
    }

    a->active = true;
    snprintf(a->status, sizeof(a->status), "%d Hz, %d ch, %d frames", a->spec.freq,
             a->spec.channels, a->spec.samples);
    SDL_PauseAudioDevice(a->device, 0);
    return a;
}

void gv_audio_destroy(gv_audio *a)
{
    if (!a) return;
    if (a->active) {
        SDL_PauseAudioDevice(a->device, 1);
        SDL_CloseAudioDevice(a->device);
    }
    free(a);
}

bool gv_audio_active(const gv_audio *a) { return a && a->active; }
const char *gv_audio_status(const gv_audio *a) { return a ? a->status : "no audio"; }

void gv_audio_set_volumes(gv_audio *a, float master, float sfx, float music)
{
    if (!a) return;
    if (a->active) SDL_LockAudioDevice(a->device);
    a->master = gv_clampf(master, 0.0f, 1.0f);
    a->sfx_gain = gv_clampf(sfx, 0.0f, 1.0f);
    a->music_gain = gv_clampf(music, 0.0f, 1.0f) * 0.75f;
    if (a->active) SDL_UnlockAudioDevice(a->device);
}

void gv_audio_set_music_enabled(gv_audio *a, bool enabled)
{
    if (!a) return;
    if (a->active) SDL_LockAudioDevice(a->device);
    a->music_enabled = enabled;
    if (a->active) SDL_UnlockAudioDevice(a->device);
}

/* --------------------------------------------------------------- ui sounds */

void gv_audio_ui(gv_audio *a, int ui_sound)
{
    if (!a) return;
    switch (ui_sound) {
    case GV_UI_MOVE:
        gv_note_locked(a, GV_OSC_TRIANGLE, 620.0f, 660.0f, 40.0f, 0.16f, 0.003f, 0.07f, 0.0f, 1.0f);
        break;
    case GV_UI_CONFIRM:
        gv_note_locked(a, GV_OSC_TRIANGLE, 660.0f, 990.0f, 30.0f, 0.20f, 0.003f, 0.15f, 0.0f, 1.0f);
        break;
    case GV_UI_BACK:
        gv_note_locked(a, GV_OSC_TRIANGLE, 520.0f, 340.0f, 30.0f, 0.18f, 0.003f, 0.13f, 0.0f, 1.0f);
        break;
    case GV_UI_DENY:
        gv_note_locked(a, GV_OSC_SQUARE, 180.0f, 150.0f, 24.0f, 0.16f, 0.004f, 0.13f, 0.0f, 0.5f);
        break;
    case GV_UI_START:
        gv_note_locked(a, GV_OSC_TRIANGLE, 440.0f, 880.0f, 12.0f, 0.24f, 0.01f, 0.45f, 0.0f, 1.0f);
        gv_note_locked(a, GV_OSC_SINE, 220.0f, 440.0f, 10.0f, 0.20f, 0.02f, 0.60f, 0.0f, 1.0f);
        break;
    default:
        break;
    }
}

/* ---------------------------------------------------------------- events */

void gv_audio_handle_events(gv_audio *a, const gv_world *w, gv_v2 listener, float pan_width)
{
    if (!a || !w || w->event_count <= 0) return;
    if (!(pan_width > 1.0f)) pan_width = 640.0f;

    if (a->active) SDL_LockAudioDevice(a->device);

    for (int i = 0; i < w->event_count; ++i) {
        const gv_event *e = &w->events[i];
        float pan = gv_clampf((e->pos.x - listener.x) / pan_width, -1.0f, 1.0f);
        /* Distance attenuation, so a fight across the arena is not as loud as
         * one in your lap. */
        float dist = gv_v2_dist(e->pos, listener);
        float near = gv_clampf(1.0f - dist / (pan_width * 2.2f), 0.12f, 1.0f);
        float mag = gv_clampf(e->magnitude, 0.0f, 1.0f);

        switch (e->kind) {
        case GV_EV_TETHER_FIRE:
            gv_note(a, GV_OSC_SQUARE, 300.0f, 900.0f, 55.0f, 0.13f * near, 0.002f, 0.10f, pan,
                    0.75f, false);
            break;
        case GV_EV_TETHER_ATTACH:
            gv_note(a, GV_OSC_SINE, 880.0f, 1320.0f, 40.0f, 0.20f * near, 0.002f, 0.16f, pan,
                    1.0f, false);
            gv_note(a, GV_OSC_NOISE, 4000.0f, 4000.0f, 0.0f, 0.07f * near, 0.001f, 0.05f, pan,
                    0.9f, false);
            break;
        case GV_EV_TETHER_MISS:
            gv_note(a, GV_OSC_SINE, 260.0f, 170.0f, 30.0f, 0.10f * near, 0.004f, 0.14f, pan,
                    0.5f, false);
            break;
        case GV_EV_TETHER_RELEASE:
            gv_note(a, GV_OSC_NOISE, 2000.0f, 2000.0f, 0.0f, 0.10f * near * (0.4f + mag), 0.02f,
                    0.22f, pan, 0.55f + 0.4f * mag, false);
            break;

        case GV_EV_ENEMY_KILLED: {
            bool boss = (e->a == GV_ENEMY_WARDEN);
            /* Pitch keys the archetype, so kills are audibly distinct. */
            float base = boss ? 90.0f : (200.0f + 40.0f * (float)(e->a % 4));
            gv_note(a, GV_OSC_NOISE, 3000.0f, 400.0f, 18.0f,
                    (boss ? 0.34f : 0.20f) * near, 0.001f, boss ? 0.55f : 0.22f, pan, 0.7f, false);
            gv_note(a, GV_OSC_SAW, base * 2.0f, base * 0.5f, boss ? 8.0f : 22.0f,
                    (boss ? 0.32f : 0.16f) * near, 0.003f, boss ? 0.75f : 0.26f, pan, 0.45f,
                    false);
            break;
        }
        case GV_EV_ENEMY_HIT:
            gv_note(a, GV_OSC_SQUARE, 420.0f, 300.0f, 45.0f, 0.10f * near, 0.001f, 0.07f, pan,
                    0.6f, false);
            break;
        case GV_EV_ENEMY_SPAWN:
            gv_note(a, GV_OSC_TRIANGLE, 300.0f, 520.0f, 14.0f, 0.07f * near, 0.05f, 0.22f, pan,
                    0.8f, false);
            break;
        case GV_EV_ENEMY_FIRE:
            gv_note(a, GV_OSC_SQUARE, 700.0f, 380.0f, 60.0f, 0.075f * near, 0.001f, 0.09f, pan,
                    0.65f, false);
            break;

        case GV_EV_PLAYER_HIT:
            gv_note(a, GV_OSC_SAW, 220.0f, 60.0f, 9.0f, 0.40f, 0.004f, 0.60f, 0.0f, 0.35f, false);
            gv_note(a, GV_OSC_NOISE, 1200.0f, 1200.0f, 0.0f, 0.24f, 0.002f, 0.30f, 0.0f, 0.4f,
                    false);
            break;
        case GV_EV_PLAYER_DIED:
            gv_note(a, GV_OSC_SAW, 420.0f, 40.0f, 3.2f, 0.44f, 0.02f, 1.7f, 0.0f, 0.3f, false);
            gv_note(a, GV_OSC_SINE, 180.0f, 30.0f, 2.4f, 0.36f, 0.02f, 2.0f, 0.0f, 1.0f, false);
            gv_note(a, GV_OSC_NOISE, 900.0f, 900.0f, 0.0f, 0.18f, 0.01f, 1.1f, 0.0f, 0.25f,
                    false);
            break;

        case GV_EV_PULSE:
            gv_note(a, GV_OSC_SINE, 340.0f, 55.0f, 12.0f, 0.34f, 0.004f, 0.50f, 0.0f, 1.0f,
                    false);
            gv_note(a, GV_OSC_NOISE, 2400.0f, 2400.0f, 0.0f, 0.16f, 0.002f, 0.28f, 0.0f, 0.5f,
                    false);
            break;
        case GV_EV_PULSE_EMPTY:
            gv_note(a, GV_OSC_SQUARE, 140.0f, 120.0f, 40.0f, 0.09f, 0.002f, 0.07f, 0.0f, 0.35f,
                    false);
            break;

        case GV_EV_WAVE_START: {
            bool boss = mag > 0.9f;
            gv_note(a, GV_OSC_TRIANGLE, gv_midi_hz(57.0f), gv_midi_hz(57.0f), 0.0f, 0.22f, 0.01f,
                    0.45f, -0.2f, 1.0f, false);
            gv_note(a, GV_OSC_TRIANGLE, gv_midi_hz(boss ? 60.0f : 64.0f),
                    gv_midi_hz(boss ? 60.0f : 64.0f), 0.0f, 0.22f, 0.02f, 0.60f, 0.2f, 1.0f,
                    false);
            if (boss) {
                gv_note(a, GV_OSC_SAW, 55.0f, 55.0f, 0.0f, 0.30f, 0.05f, 1.4f, 0.0f, 0.2f, false);
            }
            break;
        }
        case GV_EV_WAVE_CLEAR:
            for (int n = 0; n < 3; ++n) {
                float note = 64.0f + (float)n * 4.0f;
                gv_voice *v = gv_voice_alloc(a);
                if (!v) break;
                v->osc = GV_OSC_TRIANGLE;
                v->freq = v->freq_target = gv_midi_hz(note);
                v->amp = 0.20f;
                /* Stagger by extending the attack: a cheap arpeggio. */
                v->attack = 0.02f + (float)n * 0.09f;
                v->decay = 0.35f;
                v->life = v->attack + v->decay;
                v->cutoff = 1.0f;
                v->pan = ((float)n - 1.0f) * 0.3f;
            }
            break;

        case GV_EV_PICKUP:
            gv_note(a, GV_OSC_SINE, 1046.0f, 1568.0f, 34.0f, 0.18f * near, 0.002f, 0.20f, pan,
                    1.0f, false);
            break;
        case GV_EV_EXTRA_LIFE:
            for (int n = 0; n < 4; ++n) {
                gv_voice *v = gv_voice_alloc(a);
                if (!v) break;
                v->osc = GV_OSC_TRIANGLE;
                v->freq = v->freq_target = gv_midi_hz(64.0f + (float)n * 3.0f);
                v->amp = 0.22f;
                v->attack = 0.01f + (float)n * 0.07f;
                v->decay = 0.4f;
                v->life = v->attack + v->decay;
                v->cutoff = 1.0f;
            }
            break;
        case GV_EV_MULT_UP: {
            /* Rising pitch with the multiplier: the chain has its own melody. */
            float note = 60.0f + gv_clampf((float)e->a, 0.0f, 24.0f);
            gv_note(a, GV_OSC_TRIANGLE, gv_midi_hz(note), gv_midi_hz(note), 0.0f, 0.11f * near,
                    0.002f, 0.13f, pan, 1.0f, false);
            break;
        }

        case GV_EV_ROCK_HIT:
            gv_note(a, GV_OSC_NOISE, 700.0f, 700.0f, 0.0f, 0.12f * near * (0.3f + mag), 0.001f,
                    0.13f, pan, 0.32f, false);
            gv_note(a, GV_OSC_SINE, 150.0f, 90.0f, 28.0f, 0.14f * near * (0.3f + mag), 0.003f,
                    0.18f, pan, 1.0f, false);
            break;
        case GV_EV_WALL_BOUNCE:
            gv_note(a, GV_OSC_SINE, 200.0f, 130.0f, 30.0f, 0.10f * near * (0.3f + mag), 0.003f,
                    0.14f, pan, 0.8f, false);
            break;
        default:
            break;
        }
    }

    if (a->active) SDL_UnlockAudioDevice(a->device);
}

void gv_audio_update(gv_audio *a, const gv_world *w, float dt, bool in_menu)
{
    (void)dt;
    if (!a) return;

    /* Intensity: how busy and how dangerous. Both matter — a single warden
     * should raise the temperature as much as a crowd of drones. */
    float intensity = 0.0f;
    if (w) {
        float crowd = gv_clampf((float)w->enemy_count / 18.0f, 0.0f, 1.0f);
        float depth = gv_clampf((float)w->wave.index / 14.0f, 0.0f, 1.0f);
        float peril = (w->player.lives <= 1) ? 0.25f : 0.0f;
        float speed = gv_clampf(w->player.speed / GV_PLAYER_MAX_SPEED, 0.0f, 1.0f) * 0.2f;
        intensity = gv_clampf(crowd * 0.55f + depth * 0.3f + peril + speed, 0.0f, 1.0f);
        if (w->run_state != GV_RUN_PLAYING) intensity *= 0.35f;
    }

    if (a->active) SDL_LockAudioDevice(a->device);
    a->in_menu = in_menu;
    a->intensity = in_menu ? 0.12f : intensity;
    a->music_fade_target = 1.0f;
    if (a->active) SDL_UnlockAudioDevice(a->device);
}

void gv_audio_render_offline(gv_audio *a, float *out, int frames)
{
    if (!a || !out || frames <= 0) return;
    gv_audio_callback(a, (Uint8 *)out, frames * (int)sizeof(float) * 2);
}
