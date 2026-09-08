/* gv_audio.h — procedural audio.
 *
 * Every sound in GRAVITON is synthesised at runtime: there are no WAVs, no
 * decoder and no music files. Sound effects are short synth voices triggered
 * by simulation events, and the soundtrack is a sequencer that reacts to how
 * dangerous the arena currently is.
 *
 * Audio failure is never fatal. If the device cannot be opened the game runs
 * silently rather than refusing to start.
 */
#ifndef GV_AUDIO_H
#define GV_AUDIO_H

#include <stdbool.h>

#include "../game/gv_types.h"

typedef struct gv_audio gv_audio;

/* Interface sounds, which are not driven by simulation events. */
typedef enum {
    GV_UI_MOVE = 0,
    GV_UI_CONFIRM,
    GV_UI_BACK,
    GV_UI_DENY,
    GV_UI_START
} gv_ui_sound;

/* Returns NULL only on allocation failure. A device that cannot be opened
 * yields a valid, silent gv_audio — check gv_audio_active(). */
gv_audio *gv_audio_create(void);
void gv_audio_destroy(gv_audio *a);

/* False when no output device could be opened. */
bool gv_audio_active(const gv_audio *a);
const char *gv_audio_status(const gv_audio *a);

/* Volumes are 0..1 and are applied immediately. */
void gv_audio_set_volumes(gv_audio *a, float master, float sfx, float music);
void gv_audio_set_music_enabled(gv_audio *a, bool enabled);

/* Turn this frame's simulation events into sound. `listener` is the camera
 * centre and `pan_width` the half-width of the view in world units, used to
 * place effects in the stereo field. Safe to call with a NULL world. */
void gv_audio_handle_events(gv_audio *a, const gv_world *w, gv_v2 listener, float pan_width);

/* Per-frame update: drives the soundtrack's intensity from the state of the
 * arena. `dt` is real seconds. */
void gv_audio_update(gv_audio *a, const gv_world *w, float dt, bool in_menu);

void gv_audio_ui(gv_audio *a, int ui_sound);

/* Render `frames` stereo samples into `out` without a device. Used by the
 * tests to exercise the synth and assert that it never emits NaN, denormals
 * or samples outside [-1, 1]. */
void gv_audio_render_offline(gv_audio *a, float *out, int frames);

#endif /* GV_AUDIO_H */
