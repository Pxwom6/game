/* gv_collide.c — contact resolution.
 *
 * The central rule of GRAVITON lives here: above the overdrive threshold the
 * ship is a weapon, below it the ship is a target. Everything else in the
 * design exists to make that one comparison legible and fair.
 *
 * Player-vs-enemy and rock-vs-enemy use swept tests, because both can cross
 * more than their own radius in a single 120 Hz step.
 */
#include <string.h>

#include "gv_sim.h"
#include "gv_world.h"

#define GV_ROCK_LETHAL_SPEED 380.0f
#define GV_PICKUP_RADIUS 26.0f

/* Speed the ship must carry to break this enemy rather than bounce off it. */
static float gv_slam_requirement(const gv_enemy *e)
{
    return GV_OVERDRIVE_SPEED + e->armour;
}

/* Fraction of speed the ship keeps after destroying `e`. */
static float gv_slam_bleed(const gv_enemy *e)
{
    float keep = 1.0f - (0.06f + e->armour / 2600.0f);
    return gv_clampf(keep, GV_SLAM_BLEED_MIN, GV_SLAM_BLEED_MAX);
}

static void gv_hurt_enemy(gv_world *w, gv_enemy *e, float damage, int cause, gv_v2 dir,
                          float impact_speed)
{
    e->hp -= damage;
    e->hit_flash = 1.0f;
    if (e->hp <= 0.0f) {
        gv_kill_enemy(w, e, cause, dir, impact_speed);
    } else {
        e->vel = gv_v2_madd(e->vel, dir, 620.0f);
        e->stagger = gv_maxf(e->stagger, 0.4f);
        gv_add_hitstop(w, 0.03f);
        gv_add_shake(w, 0.10f);
        gv_fx_burst(w, e->pos, dir, 10, 240.0f, 255, 220, 160, GV_PART_SPARK);
        gv_emit(w, GV_EV_ENEMY_HIT, e->pos, 0.5f, e->kind);
    }
}

/* ------------------------------------------------------- player vs enemies */

static void gv_collide_player_enemies(gv_world *w)
{
    gv_player *p = &w->player;
    if (!p->alive) return;

    gv_v2 motion = gv_v2_sub(p->pos, p->prev_pos);

    for (int i = 0; i < GV_MAX_ENEMIES; ++i) {
        gv_enemy *e = &w->enemies[i];
        if (!e->alive || e->spawn_anim < 1.0f) continue;

        float toi;
        /* Sweep the ship's path this step; the enemy is treated as static over
         * the step, which is accurate enough at 120 Hz and never misses the
         * fast cases that a discrete test would tunnel straight through. */
        if (!gv_sweep_circle_circle(p->prev_pos, p->radius, motion, e->pos, e->radius, &toi)) {
            continue;
        }

        gv_v2 contact = gv_v2_madd(p->prev_pos, motion, toi);
        gv_v2 normal = gv_v2_norm_or(gv_v2_sub(e->pos, contact), gv_v2_norm_or(p->vel,
                                     gv_v2_make(1.0f, 0.0f)));

        if (p->speed >= gv_slam_requirement(e)) {
            /* Overdrive: the ship wins. Excess speed converts into extra
             * damage so a huge swing can one-shot an armoured target. */
            float excess = p->speed - gv_slam_requirement(e);
            float damage = 1.0f + floorf(excess / 520.0f);
            float keep = gv_slam_bleed(e);

            gv_hurt_enemy(w, e, damage, GV_KILL_SLAM, normal, p->speed);

            if (!e->alive) {
                p->vel = gv_v2_mul(p->vel, keep);
            } else {
                /* Bounced off something that survived: lose most of the run-up
                 * and get pushed back, so failed slams have a real cost. */
                p->vel = gv_v2_mul(p->vel, 0.5f);
                p->vel = gv_v2_madd(p->vel, normal, -260.0f);
                p->pos = gv_v2_madd(contact, normal, -2.0f);
            }
            p->speed = gv_v2_len(p->vel);
        } else if (p->invuln <= 0.0f) {
            gv_damage_player(w, e->pos);
            /* One hit per step at most: the player has been pushed clear and
             * is now invulnerable, so stop scanning. */
            return;
        } else {
            /* Invulnerable contact still separates the two, otherwise an enemy
             * can sit inside the ship and connect the instant i-frames lapse. */
            p->pos = gv_v2_madd(contact, normal, -2.0f);
            e->vel = gv_v2_madd(e->vel, normal, 220.0f);
        }
    }
}

/* -------------------------------------------------------- player vs bullets */

static void gv_collide_player_bullets(gv_world *w)
{
    gv_player *p = &w->player;
    if (!p->alive) return;

    for (int i = 0; i < GV_MAX_BULLETS; ++i) {
        gv_bullet *b = &w->bullets[i];
        if (!b->alive) continue;
        if (!gv_circles_overlap(b->pos, b->radius, p->pos, p->radius)) continue;

        b->alive = false;
        gv_fx_burst(w, b->pos, gv_v2_norm(b->vel), 6, 160.0f, 255, 190, 120, GV_PART_SPARK);
        if (p->invuln <= 0.0f) {
            gv_damage_player(w, b->pos);
            return;
        }
    }
}

/* ---------------------------------------------------------- player vs rocks */

static void gv_collide_player_rocks(gv_world *w)
{
    gv_player *p = &w->player;
    if (!p->alive) return;

    for (int i = 0; i < GV_MAX_ROCKS; ++i) {
        gv_rock *r = &w->rocks[i];
        if (!r->alive) continue;

        gv_v2 d = gv_v2_sub(p->pos, r->pos);
        float min_d = p->radius + r->radius;
        float dist_sq = gv_v2_len_sq(d);
        if (dist_sq >= min_d * min_d) continue;

        float dist = sqrtf(dist_sq);
        gv_v2 n = (dist > GV_EPS) ? gv_v2_mul(d, 1.0f / dist)
                                  : gv_v2_from_angle(gv_rng_angle(&w->fxrng));

        /* Rocks are neutral terrain: they never damage the ship. Making them
         * harmless is what lets the player fling them around fearlessly. */
        float inv_sum = 1.0f + r->inv_mass;
        float overlap = min_d - dist;
        p->pos = gv_v2_madd(p->pos, n, overlap * (1.0f / inv_sum));
        r->pos = gv_v2_madd(r->pos, n, -overlap * (r->inv_mass / inv_sum));

        float rel = gv_v2_dot(gv_v2_sub(p->vel, r->vel), n);
        if (rel < 0.0f) {
            float impulse = -(1.0f + 0.5f) * rel / inv_sum;
            p->vel = gv_v2_madd(p->vel, n, impulse);
            r->vel = gv_v2_madd(r->vel, n, -impulse * r->inv_mass);
            p->vel = gv_v2_clamp_len(p->vel, GV_PLAYER_MAX_SPEED);
            p->speed = gv_v2_len(p->vel);

            /* Any collision that costs a noticeable amount of speed has to be
             * seen and heard. Momentum is the player's whole resource, so
             * losing a chunk of it silently reads as the game cheating. */
            if (-rel > 170.0f) {
                r->hit_flash = 1.0f;
                gv_add_shake(w, gv_clampf(-rel / 1400.0f, 0.03f, 0.12f));
                gv_fx_burst(w, gv_v2_madd(p->pos, n, -p->radius), n, 8, 200.0f, 200, 220, 255,
                            GV_PART_SPARK);
                gv_emit(w, GV_EV_ROCK_HIT, p->pos, gv_clampf(-rel / 1400.0f, 0.2f, 1.0f), 0);
            }
        }
    }
}

/* --------------------------------------------------------- player vs pylons */

static void gv_collide_player_pylons(gv_world *w)
{
    gv_player *p = &w->player;
    if (!p->alive) return;

    for (int i = 0; i < w->pylon_count; ++i) {
        gv_pylon *py = &w->pylons[i];
        if (!py->alive) continue;

        gv_v2 d = gv_v2_sub(p->pos, py->pos);
        float min_d = p->radius + py->radius;
        float dist_sq = gv_v2_len_sq(d);
        if (dist_sq >= min_d * min_d) continue;

        float dist = sqrtf(dist_sq);
        gv_v2 n = (dist > GV_EPS) ? gv_v2_mul(d, 1.0f / dist) : gv_v2_make(0.0f, -1.0f);
        p->pos = gv_v2_madd(py->pos, n, min_d);

        float vn = gv_v2_dot(p->vel, n);
        if (vn < 0.0f) {
            p->vel = gv_v2_madd(p->vel, n, -vn * 1.55f);
            p->speed = gv_v2_len(p->vel);
            if (-vn > 400.0f) {
                gv_add_shake(w, 0.08f);
                gv_emit(w, GV_EV_WALL_BOUNCE, p->pos, gv_clampf(-vn / 1200.0f, 0.2f, 1.0f), 2);
            }
        }
    }
}

/* --------------------------------------------------------- rocks vs enemies */

static void gv_collide_rocks_enemies(gv_world *w)
{
    for (int i = 0; i < GV_MAX_ROCKS; ++i) {
        gv_rock *r = &w->rocks[i];
        if (!r->alive) continue;

        float rock_speed = gv_v2_len(r->vel);
        gv_v2 motion = gv_v2_sub(r->pos, r->prev_pos);

        for (int j = 0; j < GV_MAX_ENEMIES; ++j) {
            gv_enemy *e = &w->enemies[j];
            if (!e->alive || e->spawn_anim < 1.0f) continue;

            float toi;
            if (!gv_sweep_circle_circle(r->prev_pos, r->radius, motion, e->pos, e->radius, &toi)) {
                continue;
            }

            gv_v2 contact = gv_v2_madd(r->prev_pos, motion, toi);
            gv_v2 n = gv_v2_norm_or(gv_v2_sub(e->pos, contact),
                                    gv_v2_norm_or(r->vel, gv_v2_make(1.0f, 0.0f)));

            /* A rock only kills when it is genuinely moving — which, in
             * practice, means the player put it there. Armour still counts,
             * but rocks punch well above the ship's own threshold. */
            float required = GV_ROCK_LETHAL_SPEED + e->armour * 0.6f;
            if (rock_speed >= required) {
                gv_hurt_enemy(w, e, 2.0f, GV_KILL_WHIP, n, rock_speed);
                /* The rock keeps most of its speed and ploughs on through. */
                r->vel = gv_v2_mul(r->vel, 0.9f);
                r->hit_flash = 1.0f;
            } else {
                float inv_sum = r->inv_mass + GV_EPS + 1.0f;
                float rel = gv_v2_dot(gv_v2_sub(e->vel, r->vel), n);
                if (rel < 0.0f) {
                    float impulse = -(1.0f + 0.35f) * rel / inv_sum;
                    e->vel = gv_v2_madd(e->vel, n, impulse);
                    r->vel = gv_v2_madd(r->vel, n, -impulse * r->inv_mass);
                }
                /* Separate so the enemy does not grind along inside the rock. */
                float overlap = (r->radius + e->radius) - gv_v2_dist(r->pos, e->pos);
                if (overlap > 0.0f) e->pos = gv_v2_madd(e->pos, n, overlap);
            }
        }
    }
}

/* ------------------------------------------------- bullets vs solid geometry */

static void gv_collide_bullets_solids(gv_world *w)
{
    for (int i = 0; i < GV_MAX_BULLETS; ++i) {
        gv_bullet *b = &w->bullets[i];
        if (!b->alive) continue;

        /* Rocks and pylons block shots, which makes the arena's furniture
         * genuinely useful as cover rather than pure decoration. */
        for (int j = 0; j < GV_MAX_ROCKS && b->alive; ++j) {
            gv_rock *r = &w->rocks[j];
            if (!r->alive) continue;
            if (!gv_circles_overlap(b->pos, b->radius, r->pos, r->radius)) continue;
            b->alive = false;
            gv_fx_burst(w, b->pos, gv_v2_neg(gv_v2_norm(b->vel)), 5, 140.0f, 255, 190, 120,
                        GV_PART_SPARK);
        }
        for (int j = 0; j < w->pylon_count && b->alive; ++j) {
            gv_pylon *py = &w->pylons[j];
            if (!py->alive) continue;
            if (!gv_circles_overlap(b->pos, b->radius, py->pos, py->radius)) continue;
            b->alive = false;
            gv_fx_burst(w, b->pos, gv_v2_neg(gv_v2_norm(b->vel)), 5, 140.0f, 160, 240, 255,
                        GV_PART_SPARK);
        }
    }
}

/* --------------------------------------------------------- enemies vs pylons */

static void gv_collide_enemies_pylons(gv_world *w)
{
    for (int i = 0; i < GV_MAX_ENEMIES; ++i) {
        gv_enemy *e = &w->enemies[i];
        if (!e->alive || e->spawn_anim < 1.0f) continue;

        for (int j = 0; j < w->pylon_count; ++j) {
            gv_pylon *py = &w->pylons[j];
            if (!py->alive) continue;

            gv_v2 d = gv_v2_sub(e->pos, py->pos);
            float min_d = e->radius + py->radius;
            float dist_sq = gv_v2_len_sq(d);
            if (dist_sq >= min_d * min_d) continue;

            float dist = sqrtf(dist_sq);
            gv_v2 n = (dist > GV_EPS) ? gv_v2_mul(d, 1.0f / dist) : gv_v2_make(0.0f, -1.0f);
            e->pos = gv_v2_madd(py->pos, n, min_d);

            float vn = gv_v2_dot(e->vel, n);
            if (vn < 0.0f) e->vel = gv_v2_madd(e->vel, n, -vn * 1.6f);
        }
    }
}

/* -------------------------------------------------------- player vs pickups */

static void gv_collide_pickups(gv_world *w)
{
    gv_player *p = &w->player;
    if (!p->alive) return;

    for (int i = 0; i < GV_MAX_PICKUPS; ++i) {
        gv_pickup *k = &w->pickups[i];
        if (!k->alive) continue;
        if (gv_v2_dist_sq(k->pos, p->pos) > (GV_PICKUP_RADIUS + p->radius) *
                                            (GV_PICKUP_RADIUS + p->radius)) {
            continue;
        }

        k->alive = false;
        gv_emit(w, GV_EV_PICKUP, k->pos, 0.6f, k->kind);

        switch (k->kind) {
        case GV_PICKUP_CORE: {
            int value = 50 * w->score.multiplier;
            w->score.score += value;
            /* Refreshing the window is the real prize: cores are how a chain
             * survives a lull between kills. */
            w->score.mult_timer = GV_MULT_WINDOW;
            gv_fx_floater(w, k->pos, value, 130, 255, 200);
            gv_fx_burst(w, k->pos, gv_v2_zero(), 12, 200.0f, 130, 255, 200, GV_PART_SPARK);
            break;
        }
        case GV_PICKUP_LIFE:
            if (p->lives < GV_PLAYER_MAX_LIVES) {
                p->lives++;
                gv_emit(w, GV_EV_EXTRA_LIFE, k->pos, 1.0f, p->lives);
            } else {
                w->score.score += 2000;
                gv_fx_floater(w, k->pos, 2000, 255, 120, 180);
            }
            gv_fx_shockwave(w, k->pos, 180.0f, 0.6f, 4.0f, 255, 120, 180);
            gv_fx_burst(w, k->pos, gv_v2_zero(), 24, 260.0f, 255, 120, 180, GV_PART_SPARK);
            break;
        case GV_PICKUP_PULSE:
            p->pulse_charge = gv_minf(GV_PULSE_MAX, p->pulse_charge + 1.0f);
            gv_fx_burst(w, k->pos, gv_v2_zero(), 16, 220.0f, 120, 240, 255, GV_PART_SPARK);
            break;
        default:
            break;
        }
    }
}

void gv_collide_all(gv_world *w, float dt)
{
    (void)dt;
    gv_collide_player_enemies(w);
    gv_collide_player_bullets(w);
    gv_collide_player_rocks(w);
    gv_collide_player_pylons(w);
    gv_collide_rocks_enemies(w);
    gv_collide_enemies_pylons(w);
    gv_collide_bullets_solids(w);
    gv_collide_pickups(w);
}
