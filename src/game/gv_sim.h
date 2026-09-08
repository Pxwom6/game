/* gv_sim.h — internal interface shared between the simulation's translation
 * units. Not part of the public API; presentation code should use gv_world.h.
 */
#ifndef GV_SIM_H
#define GV_SIM_H

#include "gv_types.h"

/* Why an enemy stopped existing. Drives score, particles and audio. */
typedef enum {
    GV_KILL_SLAM = 0,   /* the player rammed it while in overdrive */
    GV_KILL_WHIP,       /* a rock, usually tether-flung, hit it */
    GV_KILL_SPLIT,      /* removed while splitting into children */
    GV_KILL_CULL        /* despawned for being out of play */
} gv_kill_cause;

/* ------------------------------------------------------------ event queue */
void gv_emit(gv_world *w, int kind, gv_v2 pos, float magnitude, int payload);

/* ---------------------------------------------------------------- spawning */
gv_enemy *gv_spawn_enemy(gv_world *w, int kind, gv_v2 pos, int split_depth);
gv_rock *gv_spawn_rock(gv_world *w, gv_v2 pos, float radius);
gv_bullet *gv_spawn_bullet(gv_world *w, gv_v2 pos, gv_v2 vel, float radius, float life);
gv_pickup *gv_spawn_pickup(gv_world *w, int kind, gv_v2 pos, gv_v2 vel);

/* ------------------------------------------------------------------ effects
 * All effect helpers draw from the cosmetic RNG stream and must never alter
 * gameplay state, so that visual settings cannot change a run's outcome. */
void gv_fx_burst(gv_world *w, gv_v2 pos, gv_v2 dir, int count, float speed,
                 uint8_t r, uint8_t g, uint8_t b, int kind);
void gv_fx_shockwave(gv_world *w, gv_v2 pos, float max_radius, float life,
                     float thickness, uint8_t r, uint8_t g, uint8_t b);
void gv_fx_floater(gv_world *w, gv_v2 pos, int value, uint8_t r, uint8_t g, uint8_t b);
void gv_fx_update(gv_world *w, float dt);
void gv_add_shake(gv_world *w, float trauma);
void gv_add_hitstop(gv_world *w, float seconds);

/* ------------------------------------------------------------------- death */
void gv_kill_enemy(gv_world *w, gv_enemy *e, int cause, gv_v2 impact_dir, float impact_speed);
void gv_damage_player(gv_world *w, gv_v2 from);

/* ------------------------------------------------------------------ scoring */
void gv_score_register_kill(gv_world *w, const gv_enemy *e, int cause);
void gv_score_update(gv_world *w, float dt);

/* --------------------------------------------------------------- subsystems */
void gv_player_update(gv_world *w, const gv_input *in, float dt);
void gv_player_integrate(gv_world *w, float dt);
void gv_tether_update(gv_world *w, const gv_input *in, float dt);
void gv_tether_solve(gv_world *w, float dt);
void gv_tether_detach(gv_world *w, bool player_released);
void gv_enemies_update(gv_world *w, float dt);
void gv_enemies_integrate(gv_world *w, float dt);
void gv_wave_update(gv_world *w, float dt);
void gv_collide_all(gv_world *w, float dt);

/* ---------------------------------------------------------------- utilities */
/* Resolve the tether's anchor to a live entity, or NULL if it has gone away.
 * Writes the anchor's centre and velocity when non-NULL pointers are supplied. */
bool gv_anchor_resolve(gv_world *w, const gv_tether *t, gv_v2 *out_pos, gv_v2 *out_vel,
                       float *out_inv_mass);

/* Apply an impulse to whatever the tether is anchored to (a no-op for static
 * anchors). Used so that swinging on a rock actually moves the rock. */
void gv_anchor_apply_impulse(gv_world *w, const gv_tether *t, gv_v2 impulse);

/* Clear the `tethered` flag on every anchor candidate. */
void gv_anchor_clear_flags(gv_world *w);

/* Keep an entity inside the arena, bouncing it off the walls.
 * Returns true when a wall was hit. `restitution` scales the reflected speed. */
bool gv_confine(gv_v2 *pos, gv_v2 *vel, float radius, float arena_w, float arena_h,
                float restitution);

/* Pick a spawn position at least `min_dist` from `avoid`, biased to the arena
 * edges. Always returns a position inside the arena. */
gv_v2 gv_pick_spawn_pos(gv_world *w, gv_v2 avoid, float min_dist, float radius);

/* Enemy archetype constants, shared by the spawner and the AI. */
float gv_enemy_base_radius(int kind);
float gv_enemy_base_hp(int kind);
float gv_enemy_base_score(int kind);
float gv_enemy_armour(int kind);
float gv_enemy_cost(int kind);

#endif /* GV_SIM_H */
