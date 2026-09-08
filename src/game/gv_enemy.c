/* gv_enemy.c — enemy behaviour.
 *
 * Every archetype is built to be *readable at speed*: the player is often
 * moving at 1000 px/s, so threats telegraph clearly, commit visibly, and
 * recover slowly enough to be punished.
 */
#include <string.h>

#include "gv_sim.h"
#include "gv_world.h"

#define GV_LANCER_TRIGGER_DIST 540.0f
#define GV_LANCER_WINDUP 0.82f
#define GV_LANCER_DASH_TIME 0.62f
#define GV_LANCER_DASH_SPEED 1080.0f
#define GV_LANCER_RECOVER 0.66f

#define GV_SENTINEL_RANGE 430.0f
#define GV_SENTINEL_FIRE_PERIOD 2.05f
#define GV_SENTINEL_BULLET_SPEED 430.0f

#define GV_WARDEN_FIRE_PERIOD 3.4f
#define GV_WARDEN_BULLET_SPEED 300.0f
#define GV_WARDEN_RING 7

#define GV_ENEMY_SPAWN_TIME 0.55f

/* Lead the target so aimed shots are a real threat without being unavoidable.
 * The prediction is deliberately imperfect at long range: it converges on the
 * true intercept only when the shot is already close to unavoidable. */
static gv_v2 gv_aim_lead(gv_v2 from, gv_v2 target_pos, gv_v2 target_vel, float bullet_speed)
{
    gv_v2 to_target = gv_v2_sub(target_pos, from);
    float dist = gv_v2_len(to_target);
    if (bullet_speed <= GV_EPS) return gv_v2_norm_or(to_target, gv_v2_make(1.0f, 0.0f));

    float travel = dist / bullet_speed;
    travel = gv_minf(travel, 0.9f); /* cap the lead so it cannot aim absurdly far ahead */
    gv_v2 predicted = gv_v2_madd(target_pos, target_vel, travel * 0.72f);
    return gv_v2_norm_or(gv_v2_sub(predicted, from), gv_v2_make(1.0f, 0.0f));
}

/* Keep enemies from collapsing into a single overlapping blob. Separation runs
 * on positions only; impulses would fight the AI's steering. */
static void gv_enemy_separation(gv_world *w)
{
    for (int i = 0; i < GV_MAX_ENEMIES; ++i) {
        gv_enemy *a = &w->enemies[i];
        if (!a->alive || a->spawn_anim < 1.0f) continue;

        for (int j = i + 1; j < GV_MAX_ENEMIES; ++j) {
            gv_enemy *b = &w->enemies[j];
            if (!b->alive || b->spawn_anim < 1.0f) continue;

            gv_v2 d = gv_v2_sub(b->pos, a->pos);
            float min_d = (a->radius + b->radius) * 1.02f;
            float dist_sq = gv_v2_len_sq(d);
            if (dist_sq >= min_d * min_d) continue;

            float dist = sqrtf(dist_sq);
            gv_v2 n = (dist > GV_EPS) ? gv_v2_mul(d, 1.0f / dist)
                                      : gv_v2_from_angle(gv_rng_angle(&w->fxrng));
            /* Wardens shoulder others aside instead of being pushed. */
            float a_share = (a->kind == GV_ENEMY_WARDEN) ? 0.12f : 0.5f;
            float b_share = (b->kind == GV_ENEMY_WARDEN) ? 0.12f : 0.5f;
            float total = a_share + b_share;
            if (total <= GV_EPS) continue;
            a_share /= total;
            b_share /= total;

            float push = (min_d - dist) * 0.55f;
            a->pos = gv_v2_madd(a->pos, n, -push * a_share);
            b->pos = gv_v2_madd(b->pos, n, push * b_share);
        }
    }
}

static void gv_enemy_fire(gv_world *w, gv_enemy *e, gv_v2 dir, float speed, float radius,
                          float life)
{
    gv_v2 muzzle = gv_v2_madd(e->pos, dir, e->radius + 6.0f);
    gv_v2 vel = gv_v2_mul(dir, speed * w->tuning.bullet_speed);
    if (gv_spawn_bullet(w, muzzle, vel, radius, life)) {
        gv_emit(w, GV_EV_ENEMY_FIRE, muzzle, 0.4f, e->kind);
        gv_fx_burst(w, muzzle, dir, 3, 90.0f, 255, 160, 90, GV_PART_SPARK);
    }
}

static void gv_enemy_ai_step(gv_world *w, gv_enemy *e, float dt)
{
    gv_player *p = &w->player;
    gv_v2 to_player = gv_v2_sub(p->pos, e->pos);
    float dist = gv_v2_len(to_player);
    gv_v2 dir = gv_v2_norm_or(to_player, gv_v2_from_angle(e->angle));

    /* Straggler escalation. The wave director raises `hunt` when a wave drags
     * on; enemies get faster and stop keeping their distance, so a fight can
     * never stall into a chase around an empty arena. */
    float hunt = w->wave.hunt;
    float accel = e->accel * (1.0f + 0.85f * hunt);
    float max_speed = e->max_speed * (1.0f + 0.60f * hunt);

    /* A staggered enemy has been shoved and is briefly not steering. */
    if (e->stagger > 0.0f) {
        e->stagger -= dt;
        return;
    }

    /* With the player dead, enemies mill around instead of piling onto a
     * corpse — it keeps the death beat calm and readable. */
    if (!p->alive) {
        e->vel = gv_v2_mul(e->vel, expf(-0.9f * dt));
        return;
    }

    switch (e->kind) {
    case GV_ENEMY_DRONE:
    case GV_ENEMY_SPLITTER: {
        e->vel = gv_v2_madd(e->vel, dir, accel * dt);
        e->vel = gv_v2_clamp_len(e->vel, max_speed);
        e->angle = gv_v2_angle(gv_v2_norm_or(e->vel, dir));
        break;
    }

    case GV_ENEMY_LANCER: {
        switch (e->ai) {
        case GV_EAI_SEEK:
            e->vel = gv_v2_madd(e->vel, dir, accel * dt);
            e->vel = gv_v2_clamp_len(e->vel, max_speed);
            e->angle = gv_angle_approach(e->angle, gv_v2_angle(dir), 4.5f * dt);
            e->timer -= dt;
            if (dist < GV_LANCER_TRIGGER_DIST && e->timer <= 0.0f) {
                e->ai = GV_EAI_WINDUP;
                e->timer = GV_LANCER_WINDUP;
            }
            break;

        case GV_EAI_WINDUP:
            /* Brake and track: the pause is the tell, the slow turn is the
             * counter-play — you can bait the commit and slip aside. */
            e->vel = gv_v2_mul(e->vel, expf(-4.2f * dt));
            e->angle = gv_angle_approach(e->angle, gv_v2_angle(dir), 3.0f * dt);
            e->timer -= dt;
            if (e->timer <= 0.0f) {
                e->ai = GV_EAI_DASH;
                e->timer = GV_LANCER_DASH_TIME;
                e->dash_dir = gv_v2_from_angle(e->angle);
                e->vel = gv_v2_mul(e->dash_dir, GV_LANCER_DASH_SPEED * w->tuning.enemy_speed);
                gv_fx_burst(w, e->pos, e->dash_dir, 10, 260.0f, 255, 170, 70, GV_PART_SPARK);
            }
            break;

        case GV_EAI_DASH:
            /* Committed: no steering at all during the dash. */
            e->timer -= dt;
            if (e->timer <= 0.0f) {
                e->ai = GV_EAI_RECOVER;
                e->timer = GV_LANCER_RECOVER;
            }
            break;

        case GV_EAI_RECOVER:
        default:
            e->vel = gv_v2_mul(e->vel, expf(-3.0f * dt));
            e->angle = gv_angle_approach(e->angle, gv_v2_angle(dir), 2.0f * dt);
            e->timer -= dt;
            if (e->timer <= 0.0f) {
                e->ai = GV_EAI_SEEK;
                e->timer = gv_rng_range_f(&w->rng, 0.25f, 0.7f);
            }
            break;
        }
        break;
    }

    case GV_ENEMY_SENTINEL: {
        /* Hold a ring at firing range: close in when far, back off when near. */
        /* Under escalation the sentinel abandons its standoff and closes in. */
        float standoff = GV_SENTINEL_RANGE * (1.0f - 0.72f * hunt);
        float error = dist - standoff;
        gv_v2 radial = gv_v2_mul(dir, gv_clampf(error * 0.012f, -1.0f, 1.0f));
        gv_v2 tangent = gv_v2_mul(gv_v2_perp(dir), 0.65f);
        gv_v2 steer = gv_v2_norm_or(gv_v2_add(radial, tangent), dir);

        e->vel = gv_v2_madd(e->vel, steer, accel * dt);
        e->vel = gv_v2_clamp_len(e->vel, max_speed);
        e->angle = gv_angle_approach(e->angle, gv_v2_angle(dir), 2.6f * dt);

        e->fire_timer -= dt;
        if (e->fire_timer <= 0.0f) {
            e->fire_timer = GV_SENTINEL_FIRE_PERIOD / gv_maxf(0.2f, w->tuning.spawn_rate);
            if (dist < standoff * 1.9f + 200.0f) {
                gv_v2 shot = gv_aim_lead(e->pos, p->pos, p->vel, GV_SENTINEL_BULLET_SPEED);
                gv_enemy_fire(w, e, shot, GV_SENTINEL_BULLET_SPEED, 5.0f, 4.0f);
            }
        }
        break;
    }

    case GV_ENEMY_WARDEN: {
        e->vel = gv_v2_madd(e->vel, dir, accel * dt);
        e->vel = gv_v2_clamp_len(e->vel, max_speed);
        e->angle = gv_wrap_angle(e->angle + 0.9f * dt);

        e->fire_timer -= dt;
        if (e->fire_timer <= 0.0f) {
            e->fire_timer = GV_WARDEN_FIRE_PERIOD;
            /* A slow radial curtain: it does not track, so it rewards reading
             * the gaps rather than out-running the shot. */
            float base = gv_rng_angle(&w->rng);
            for (int i = 0; i < GV_WARDEN_RING; ++i) {
                float a = base + (float)i * (GV_TAU / (float)GV_WARDEN_RING);
                gv_enemy_fire(w, e, gv_v2_from_angle(a), GV_WARDEN_BULLET_SPEED, 6.0f, 5.0f);
            }
            gv_fx_shockwave(w, e->pos, e->radius * 3.0f, 0.4f, 3.0f, 255, 70, 220);
        }
        break;
    }

    default:
        break;
    }
}

void gv_enemies_update(gv_world *w, float dt)
{
    for (int i = 0; i < GV_MAX_ENEMIES; ++i) {
        gv_enemy *e = &w->enemies[i];
        if (!e->alive) continue;

        e->hit_flash = gv_maxf(0.0f, e->hit_flash - dt * 4.0f);

        if (e->spawn_anim < 1.0f) {
            e->spawn_anim = gv_minf(1.0f, e->spawn_anim + dt / GV_ENEMY_SPAWN_TIME);
            /* Intangible while materialising: no steering, no collisions. */
            e->vel = gv_v2_mul(e->vel, expf(-3.0f * dt));
            continue;
        }

        gv_enemy_ai_step(w, e, dt);
    }
}

void gv_enemies_integrate(gv_world *w, float dt)
{
    for (int i = 0; i < GV_MAX_ENEMIES; ++i) {
        gv_enemy *e = &w->enemies[i];
        if (!e->alive) continue;

        e->pos = gv_v2_madd(e->pos, e->vel, dt);

        /* Lancers keep their dash speed through a wall bounce, which turns a
         * missed dash into a second threat rather than a dead one. */
        float restitution = (e->kind == GV_ENEMY_LANCER && e->ai == GV_EAI_DASH) ? 0.95f : 0.7f;
        if (gv_confine(&e->pos, &e->vel, e->radius, w->arena_w, w->arena_h, restitution)) {
            if (gv_v2_len_sq(e->vel) > 400.0f * 400.0f) {
                gv_emit(w, GV_EV_WALL_BOUNCE, e->pos, 0.3f, 0);
            }
        }

        if (!gv_v2_finite(e->pos) || !gv_v2_finite(e->vel)) {
            /* Defensive: a non-finite entity would poison every later query.
             * This has never triggered in testing, but the cost of the check is
             * nil and the cost of a NaN reaching the renderer is a hang. */
            gv_kill_enemy(w, e, GV_KILL_CULL, gv_v2_zero(), 0.0f);
        }
    }

    gv_enemy_separation(w);
}
