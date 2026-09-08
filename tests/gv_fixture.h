/* gv_fixture.h — shared helpers for the simulation tests.
 *
 * `static inline` so that including this from several test translation units
 * costs nothing and produces no unused-function warnings.
 */
#ifndef GV_FIXTURE_H
#define GV_FIXTURE_H

#include <stdlib.h>
#include <string.h>

#include "../src/game/gv_sim.h"
#include "../src/game/gv_world.h"

/* The world is a few hundred KB; keep it off the stack. */
static inline gv_world *gv_new_world(uint64_t seed, int difficulty)
{
    gv_world *w = (gv_world *)malloc(sizeof(gv_world));
    if (w) gv_world_init(w, seed, difficulty);
    return w;
}

static inline gv_input gv_idle_input(const gv_world *w)
{
    gv_input in;
    memset(&in, 0, sizeof(in));
    in.aim_point = gv_v2_add(w->player.pos, gv_v2_make(100.0f, 0.0f));
    return in;
}

static inline void gv_run(gv_world *w, const gv_input *in, int steps)
{
    for (int i = 0; i < steps; ++i) {
        gv_world_step(w, in, GV_FIXED_DT);
        gv_world_clear_events(w);
    }
}

/* Stop the wave director from spawning, so a test can set up an exact
 * scenario without interference. Parking it in INTRO with a huge timer is
 * enough: no other state can be entered from there. */
static inline void gv_park_waves(gv_world *w)
{
    w->wave.state = GV_WAVE_INTRO;
    w->wave.timer = 1.0e9f;
    w->wave.budget = 0.0f;
    w->wave.hunt = 0.0f;
}

/* Strip the arena down to nothing the ship can touch.
 *
 * The seeded arena scatters rocks and pylons, and both are solid. A test that
 * stages an exact encounter — charge this enemy at this speed, swing on this
 * anchor — is measuring the rule under test, not the furniture, so anything
 * else the ship could hit has to go. Leaving it in produced three very
 * convincing false failures: a graze against a rock read as the rope solver
 * leaking energy, a wall bounce read as it bleeding a swing dry, and a pylon
 * sitting 30px short of a warden read as the armour gate being broken.
 *
 * A test that is about the furniture obviously does not call this. */
static inline void gv_clear_arena(gv_world *w)
{
    for (int i = 0; i < GV_MAX_ROCKS; ++i) w->rocks[i].alive = false;
    w->rock_count = 0;
    for (int i = 0; i < w->pylon_count; ++i) w->pylons[i].alive = false;
}

/* Place a fully materialised, motionless enemy — the state a test usually
 * wants, rather than the intangible spawning state. */
static inline gv_enemy *gv_place_enemy(gv_world *w, int kind, gv_v2 pos)
{
    gv_enemy *e = gv_spawn_enemy(w, kind, pos, 0);
    if (e) {
        e->spawn_anim = 1.0f;
        e->vel = gv_v2_zero();
        e->fire_timer = 1.0e9f; /* silence it unless the test wants shooting */
    }
    return e;
}

static inline bool gv_saw_event(const gv_world *w, int kind)
{
    for (int i = 0; i < w->event_count; ++i) {
        if (w->events[i].kind == (uint8_t)kind) return true;
    }
    return false;
}

/* Step until `kind` is observed or the budget runs out. */
static inline bool gv_run_until_event(gv_world *w, const gv_input *in, int kind, int max_steps)
{
    for (int i = 0; i < max_steps; ++i) {
        gv_world_step(w, in, GV_FIXED_DT);
        bool hit = gv_saw_event(w, kind);
        gv_world_clear_events(w);
        if (hit) return true;
    }
    return false;
}

static inline int gv_count_live_bullets(const gv_world *w)
{
    int n = 0;
    for (int i = 0; i < GV_MAX_BULLETS; ++i) {
        if (w->bullets[i].alive) n++;
    }
    return n;
}

static inline int gv_count_live_particles(const gv_world *w)
{
    int n = 0;
    for (int i = 0; i < GV_MAX_PARTICLES; ++i) {
        if (w->particles[i].alive) n++;
    }
    return n;
}

static inline int gv_count_live_pickups(const gv_world *w)
{
    int n = 0;
    for (int i = 0; i < GV_MAX_PICKUPS; ++i) {
        if (w->pickups[i].alive) n++;
    }
    return n;
}

/* Nearest pylon to the player; every seeded arena has at least one. */
static inline const gv_pylon *gv_nearest_pylon(const gv_world *w)
{
    const gv_pylon *best = NULL;
    float best_d = 1e30f;
    for (int i = 0; i < w->pylon_count; ++i) {
        if (!w->pylons[i].alive) continue;
        float d = gv_v2_dist(w->pylons[i].pos, w->player.pos);
        if (d < best_d) {
            best_d = d;
            best = &w->pylons[i];
        }
    }
    return best;
}

static inline bool gv_world_all_finite(const gv_world *w)
{
    if (!gv_v2_finite(w->player.pos) || !gv_v2_finite(w->player.vel)) return false;
    if (!isfinite(w->player.speed) || !isfinite(w->player.focus)) return false;
    if (!isfinite(w->player.tether.length) || !gv_v2_finite(w->player.tether.tip)) return false;
    if (!isfinite(w->shake) || !isfinite(w->time)) return false;
    for (int i = 0; i < GV_MAX_ENEMIES; ++i) {
        if (!w->enemies[i].alive) continue;
        if (!gv_v2_finite(w->enemies[i].pos) || !gv_v2_finite(w->enemies[i].vel)) return false;
        if (!isfinite(w->enemies[i].hp)) return false;
    }
    for (int i = 0; i < GV_MAX_ROCKS; ++i) {
        if (!w->rocks[i].alive) continue;
        if (!gv_v2_finite(w->rocks[i].pos) || !gv_v2_finite(w->rocks[i].vel)) return false;
    }
    for (int i = 0; i < GV_MAX_BULLETS; ++i) {
        if (!w->bullets[i].alive) continue;
        if (!gv_v2_finite(w->bullets[i].pos) || !gv_v2_finite(w->bullets[i].vel)) return false;
    }
    for (int i = 0; i < GV_MAX_PICKUPS; ++i) {
        if (!w->pickups[i].alive) continue;
        if (!gv_v2_finite(w->pickups[i].pos)) return false;
    }
    for (int i = 0; i < GV_MAX_PARTICLES; ++i) {
        if (!w->particles[i].alive) continue;
        if (!gv_v2_finite(w->particles[i].pos) || !isfinite(w->particles[i].life)) return false;
    }
    for (int i = 0; i < GV_MAX_SHOCKWAVES; ++i) {
        if (!w->shockwaves[i].alive) continue;
        if (!isfinite(w->shockwaves[i].radius)) return false;
    }
    return true;
}

#endif /* GV_FIXTURE_H */
