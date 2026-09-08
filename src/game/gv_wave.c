/* gv_wave.c — the wave director.
 *
 * Waves are budget-driven rather than scripted: each wave gets a pool of
 * points and buys a randomised mix of archetypes with it. That keeps the
 * difficulty curve smooth while making no two runs identical.
 */
#include "gv_sim.h"
#include "gv_world.h"

/* Never let more than this many enemies exist at once, however large the
 * budget grows. Beyond roughly this count the screen stops being readable,
 * and unreadable is not the same as difficult. */
#define GV_SOFT_ENEMY_CAP 58

#define GV_WAVE_INTRO_TIME 2.1f
#define GV_WAVE_CLEAR_TIME 1.7f

/* Seconds of grace before stragglers start escalating, and how long the ramp
 * takes to reach full aggression. */
#define GV_HUNT_GRACE 8.0f
#define GV_HUNT_RAMP 16.0f

/* Wave at which each archetype joins the pool. */
#define GV_UNLOCK_LANCER 3
#define GV_UNLOCK_SENTINEL 5
#define GV_UNLOCK_SPLITTER 7
#define GV_WARDEN_EVERY 10

static float gv_wave_budget(const gv_world *w, int wave)
{
    /* Linear early, gently super-linear later: the ramp has to stay ahead of a
     * player who is getting better at the tether, without becoming a wall. */
    float base = 3.0f + 1.75f * (float)(wave - 1) + 0.045f * (float)((wave - 1) * (wave - 1));
    return base * w->tuning.spawn_budget;
}

static float gv_wave_spawn_interval(const gv_world *w, int wave)
{
    float interval = 0.62f - 0.012f * (float)wave;
    interval = gv_maxf(interval, 0.17f);
    return interval / gv_maxf(0.25f, w->tuning.spawn_rate);
}

/* Weighted archetype selection for the current wave. Returns -1 when nothing
 * affordable is unlocked yet. */
static int gv_wave_pick_kind(gv_world *w, int wave, float budget)
{
    int kinds[GV_ENEMY_KIND_COUNT];
    float weights[GV_ENEMY_KIND_COUNT];
    int n = 0;
    float total = 0.0f;

    /* Drones thin out as the roster fills in, so late waves are not just a
     * bigger pile of the same thing. */
    if (gv_enemy_cost(GV_ENEMY_DRONE) <= budget) {
        kinds[n] = GV_ENEMY_DRONE;
        weights[n] = gv_maxf(1.2f, 7.0f - 0.30f * (float)wave);
        total += weights[n++];
    }
    if (wave >= GV_UNLOCK_LANCER && gv_enemy_cost(GV_ENEMY_LANCER) <= budget) {
        kinds[n] = GV_ENEMY_LANCER;
        weights[n] = gv_minf(4.5f, 1.4f + 0.22f * (float)(wave - GV_UNLOCK_LANCER));
        total += weights[n++];
    }
    if (wave >= GV_UNLOCK_SENTINEL && gv_enemy_cost(GV_ENEMY_SENTINEL) <= budget) {
        kinds[n] = GV_ENEMY_SENTINEL;
        weights[n] = gv_minf(3.2f, 1.0f + 0.18f * (float)(wave - GV_UNLOCK_SENTINEL));
        total += weights[n++];
    }
    if (wave >= GV_UNLOCK_SPLITTER && gv_enemy_cost(GV_ENEMY_SPLITTER) <= budget) {
        kinds[n] = GV_ENEMY_SPLITTER;
        weights[n] = gv_minf(2.6f, 0.9f + 0.15f * (float)(wave - GV_UNLOCK_SPLITTER));
        total += weights[n++];
    }

    if (n == 0 || total <= 0.0f) return -1;

    float roll = gv_rng_range_f(&w->rng, 0.0f, total);
    for (int i = 0; i < n; ++i) {
        roll -= weights[i];
        if (roll <= 0.0f) return kinds[i];
    }
    return kinds[n - 1];
}

static void gv_wave_begin(gv_world *w)
{
    gv_wave *v = &w->wave;
    v->index++;
    v->state = GV_WAVE_SPAWNING;
    v->budget = gv_wave_budget(w, v->index);
    v->spawn_timer = 0.0f;
    v->warden_wave = (v->index % GV_WARDEN_EVERY) == 0;
    v->timer = 0.0f;
    v->fight_time = 0.0f;
    v->hunt = 0.0f;

    gv_emit(w, GV_EV_WAVE_START, w->player.pos, v->warden_wave ? 1.0f : 0.5f, v->index);

    /* The warden arrives first and alone, so the entrance lands. */
    if (v->warden_wave) {
        gv_v2 pos = gv_pick_spawn_pos(w, w->player.pos, 620.0f, 34.0f);
        if (gv_spawn_enemy(w, GV_ENEMY_WARDEN, pos, 0)) {
            v->budget -= gv_enemy_cost(GV_ENEMY_WARDEN);
            gv_add_shake(w, 0.4f);
        }
    }
}

void gv_wave_update(gv_world *w, float dt)
{
    gv_wave *v = &w->wave;

    switch (v->state) {
    case GV_WAVE_INTRO:
        v->timer -= dt;
        if (v->timer <= 0.0f) gv_wave_begin(w);
        break;

    case GV_WAVE_SPAWNING: {
        v->spawn_timer -= dt;
        if (v->spawn_timer > 0.0f) break;

        if (v->budget <= 0.0f) {
            v->state = GV_WAVE_FIGHTING;
            break;
        }

        /* Hold spawning while the arena is already saturated. The budget is
         * preserved, so the wave still costs what it should — it just arrives
         * at a rate the player can actually read. */
        if (w->enemy_count >= GV_SOFT_ENEMY_CAP) {
            v->spawn_timer = 0.35f;
            break;
        }

        int kind = gv_wave_pick_kind(w, v->index, v->budget);
        if (kind < 0) {
            /* Nothing affordable remains: the wave is fully committed. */
            v->budget = 0.0f;
            v->state = GV_WAVE_FIGHTING;
            break;
        }

        gv_v2 pos = gv_pick_spawn_pos(w, w->player.pos, 460.0f, gv_enemy_base_radius(kind));
        if (gv_spawn_enemy(w, kind, pos, 0)) {
            v->budget -= gv_enemy_cost(kind);
            v->spawn_timer = gv_wave_spawn_interval(w, v->index);
        } else {
            /* Pool exhausted; try again shortly rather than losing the budget. */
            v->spawn_timer = 0.4f;
        }
        break;
    }

    case GV_WAVE_FIGHTING:
        /* Escalate against stragglers. Ramping rather than snapping keeps the
         * change readable: the player feels the pressure build. */
        v->fight_time += dt;
        v->hunt = gv_clampf((v->fight_time - GV_HUNT_GRACE) / GV_HUNT_RAMP, 0.0f, 1.0f);

        if (w->enemy_count <= 0) {
            v->state = GV_WAVE_CLEARED;
            v->timer = GV_WAVE_CLEAR_TIME;

            int64_t bonus = (int64_t)(200.0f * (float)v->index * w->tuning.score_mult);
            w->score.score += bonus;
            gv_fx_floater(w, w->player.pos, (int)bonus, 255, 240, 140);
            gv_emit(w, GV_EV_WAVE_CLEAR, w->player.pos, 1.0f, v->index);

            /* A clear tops the pulse back up — the reward for finishing a wave
             * is being ready for the next one. */
            w->player.pulse_charge = gv_maxf(w->player.pulse_charge,
                                             gv_minf(GV_PULSE_MAX, w->player.pulse_charge + 1.0f));
        }
        break;

    case GV_WAVE_CLEARED:
    default:
        v->timer -= dt;
        if (v->timer <= 0.0f) {
            v->state = GV_WAVE_INTRO;
            v->timer = GV_WAVE_INTRO_TIME;
        }
        break;
    }
}
