/* gv_hud.h — the in-run heads-up display.
 *
 * Holds only presentation state (eased counters, banner timers). Reads the
 * world; never writes to it.
 */
#ifndef GV_HUD_H
#define GV_HUD_H

#include "../game/gv_types.h"
#include "gv_gfx.h"

typedef struct {
    double score_shown;   /* eased toward the real score so it counts up */
    float banner_timer;
    float banner_life;
    char banner[48];
    char banner_sub[48];
    int last_wave;
    int last_multiplier;
    int last_lives;
    float mult_pop;       /* brief scale-up when the multiplier climbs */
    float life_flash;
    float hunt_pulse;
    int64_t best_score;
} gv_hud_state;

void gv_hud_init(gv_hud_state *h);

/* The high score to display alongside the current run. */
void gv_hud_set_best(gv_hud_state *h, int64_t best);

/* `dt` is real seconds. Drives eased counters and banner timing. */
void gv_hud_update(gv_hud_state *h, const gv_world *w, float dt);

void gv_hud_draw(gv_hud_state *h, gv_gfx *g, const gv_world *w);

#endif /* GV_HUD_H */
