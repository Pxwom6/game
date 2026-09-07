/* gv_scene.h — draws the world.
 *
 * Owns the parallax starfield and the camera. Reads the simulation but never
 * writes to it, so drawing can be skipped entirely (headless tests) without
 * changing a run.
 */
#ifndef GV_SCENE_H
#define GV_SCENE_H

#include "../game/gv_types.h"
#include "gv_gfx.h"

typedef struct gv_scene gv_scene;

typedef struct {
    bool screen_shake;
    bool motion_blur_trails;
    float particle_density; /* 0..1, mirrors world.fx_quality */
    bool high_contrast;     /* boosts outlines, dims background clutter */
    bool show_offscreen_markers;
} gv_scene_options;

gv_scene *gv_scene_create(uint64_t seed);
void gv_scene_destroy(gv_scene *s);

void gv_scene_set_options(gv_scene *s, const gv_scene_options *opt);

/* Snap the camera straight to the player, for the start of a run. */
void gv_scene_reset(gv_scene *s, const gv_world *w);

/* Advance camera smoothing and shake. `dt` is real (unscaled) seconds. */
void gv_scene_update(gv_scene *s, const gv_world *w, float dt);

/* Draw the world. `lag` is the unsimulated time left in the accumulator, used
 * to extrapolate positions so motion stays smooth when the display refresh
 * does not divide evenly into the fixed timestep. */
void gv_scene_draw(gv_scene *s, gv_gfx *g, const gv_world *w, float lag);

/* Where the mouse currently points, in world space. */
gv_v2 gv_scene_screen_to_world(const gv_scene *s, gv_gfx *g, gv_v2 view_point);

#endif /* GV_SCENE_H */
