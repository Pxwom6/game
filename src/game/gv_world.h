/* gv_world.h — public interface to GRAVITON's simulation.
 *
 * The simulation is a pure function of (previous state, input, fixed dt). It
 * performs no I/O, no allocation and no clock reads, which is what lets the
 * test-suite replay entire runs deterministically from a seed.
 */
#ifndef GV_WORLD_H
#define GV_WORLD_H

#include "gv_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Fill `t` with the multipliers for a difficulty. Out-of-range values are
 * clamped to a valid difficulty rather than producing garbage tuning. */
void gv_tuning_for(gv_tuning *t, int difficulty);

const char *gv_difficulty_name(int difficulty);

/* Reset `w` to the start of a fresh run. Safe to call on uninitialised memory. */
void gv_world_init(gv_world *w, uint64_t seed, int difficulty);

/* Advance the simulation by exactly one fixed step. `dt` is passed explicitly
 * (rather than baked in) so tests can probe alternative step sizes, but the
 * game always uses GV_FIXED_DT. A NULL input is treated as "no buttons, no
 * thrust, aim straight ahead". */
void gv_world_step(gv_world *w, const gv_input *in, float dt);

/* Drop all queued events. The presentation layer calls this after draining. */
void gv_world_clear_events(gv_world *w);

/* Number of enemies currently occupying a pool slot. */
int gv_world_alive_enemies(const gv_world *w);

/* True once the death animation has finished and the run is over. */
bool gv_world_is_over(const gv_world *w);

/* Total score including the end-of-run style bonus. */
int64_t gv_world_final_score(const gv_world *w);

#ifdef __cplusplus
}
#endif

#endif /* GV_WORLD_H */
