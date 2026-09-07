#include "gv_autopilot.h"

#include <string.h>

#include "gv_sim.h"

/* Speed the bot tries to reach before committing to an attack run. */
#define GV_AUTO_ATTACK_SPEED 900.0f
#define GV_AUTO_ANCHOR_MIN 140.0f
#define GV_AUTO_ANCHOR_MAX 480.0f

static const gv_enemy *gv_auto_nearest_enemy(const gv_world *w, float *out_dist)
{
    const gv_enemy *best = NULL;
    float best_d = 1e30f;
    for (int i = 0; i < GV_MAX_ENEMIES; ++i) {
        const gv_enemy *e = &w->enemies[i];
        if (!e->alive || e->spawn_anim < 1.0f) continue;
        float d = gv_v2_dist(e->pos, w->player.pos);
        if (d < best_d) {
            best_d = d;
            best = e;
        }
    }
    if (out_dist) *out_dist = best ? best_d : 1e30f;
    return best;
}

/* Pick an anchor to swing on: something at a comfortable rope length that is
 * roughly to the side of where we want to go, not directly behind us. */
static bool gv_auto_pick_anchor(const gv_world *w, gv_v2 desired_dir, gv_v2 *out_pos)
{
    const gv_player *p = &w->player;
    float best_score = -1e30f;
    gv_v2 best_pos = gv_v2_zero();
    bool found = false;

    for (int i = 0; i < w->pylon_count; ++i) {
        const gv_pylon *py = &w->pylons[i];
        if (!py->alive) continue;
        float d = gv_v2_dist(py->pos, p->pos);
        if (d < GV_AUTO_ANCHOR_MIN || d > GV_AUTO_ANCHOR_MAX) continue;

        /* Prefer anchors perpendicular to the direction of travel: those give
         * the widest swing arc. */
        gv_v2 to_anchor = gv_v2_norm_or(gv_v2_sub(py->pos, p->pos), gv_v2_make(1.0f, 0.0f));
        float alignment = gv_absf(gv_v2_cross(to_anchor, desired_dir));
        float score = alignment * 100.0f - gv_absf(d - 320.0f);
        if (score > best_score) {
            best_score = score;
            best_pos = py->pos;
            found = true;
        }
    }

    for (int i = 0; i < GV_MAX_ROCKS; ++i) {
        const gv_rock *r = &w->rocks[i];
        if (!r->alive) continue;
        float d = gv_v2_dist(r->pos, p->pos);
        if (d < GV_AUTO_ANCHOR_MIN || d > GV_AUTO_ANCHOR_MAX) continue;

        gv_v2 to_anchor = gv_v2_norm_or(gv_v2_sub(r->pos, p->pos), gv_v2_make(1.0f, 0.0f));
        float alignment = gv_absf(gv_v2_cross(to_anchor, desired_dir));
        /* Rocks move under load, so they are worth slightly less than a pylon. */
        float score = alignment * 100.0f - gv_absf(d - 320.0f) - 30.0f;
        if (score > best_score) {
            best_score = score;
            best_pos = r->pos;
            found = true;
        }
    }

    if (found && out_pos) *out_pos = best_pos;
    return found;
}

/* Is anything dangerous close enough to justify spending a pulse charge? */
static bool gv_auto_should_pulse(const gv_world *w)
{
    const gv_player *p = &w->player;
    if (p->pulse_charge < 1.0f || p->pulse_cooldown > 0.0f) return false;
    if (p->invuln > 0.0f) return false;

    for (int i = 0; i < GV_MAX_BULLETS; ++i) {
        const gv_bullet *b = &w->bullets[i];
        if (!b->alive) continue;
        if (gv_v2_dist_sq(b->pos, p->pos) < 110.0f * 110.0f) return true;
    }
    for (int i = 0; i < GV_MAX_ENEMIES; ++i) {
        const gv_enemy *e = &w->enemies[i];
        if (!e->alive || e->spawn_anim < 1.0f) continue;
        /* Only panic about enemies we cannot currently break. */
        if (p->speed >= GV_OVERDRIVE_SPEED + e->armour) continue;
        if (gv_v2_dist_sq(e->pos, p->pos) < 105.0f * 105.0f) return true;
    }
    return false;
}

gv_input gv_autopilot_think(gv_world *w, gv_rng *jitter)
{
    gv_input in;
    memset(&in, 0, sizeof(in));

    gv_player *p = &w->player;
    in.aim_point = gv_v2_add(p->pos, gv_v2_make(100.0f, 0.0f));
    if (!p->alive) return in;

    float target_dist = 0.0f;
    const gv_enemy *target = gv_auto_nearest_enemy(w, &target_dist);

    gv_v2 to_target = target ? gv_v2_norm_or(gv_v2_sub(target->pos, p->pos),
                                             gv_v2_make(1.0f, 0.0f))
                             : gv_v2_make(1.0f, 0.0f);
    if (target) in.aim_point = target->pos;

    if (gv_auto_should_pulse(w)) in.pulse_pressed = true;

    /* Enough speed to break the nearest target? Then go through it. */
    float needed = target ? GV_OVERDRIVE_SPEED + target->armour : GV_AUTO_ATTACK_SPEED;
    bool can_kill = target && p->speed >= needed * 1.05f;

    switch (p->tether.state) {
    case GV_TETHER_ATTACHED: {
        in.tether_held = true;

        gv_v2 anchor_pos;
        if (!gv_anchor_resolve(w, &p->tether, &anchor_pos, NULL, NULL)) {
            in.tether_held = false;
            break;
        }

        gv_v2 radial = gv_v2_norm_or(gv_v2_sub(p->pos, anchor_pos), gv_v2_make(1.0f, 0.0f));
        gv_v2 tangent = gv_v2_perp(radial);
        /* Swing the way we are already going, so thrust adds to the arc
         * instead of fighting it. */
        if (gv_v2_dot(p->vel, tangent) < 0.0f) tangent = gv_v2_neg(tangent);

        in.move = tangent;
        in.reel = 1.0f; /* shortening the rope is where the speed comes from */

        /* Let go once we are fast and pointed at something worth hitting. */
        if (can_kill) {
            gv_v2 heading = gv_v2_norm_or(p->vel, tangent);
            if (gv_v2_dot(heading, to_target) > 0.82f) in.tether_held = false;
        }
        /* Bail out of a swing that has stopped paying: the rope is short and
         * we are still slow. */
        if (p->tether.length <= GV_TETHER_MIN_LEN + 6.0f && p->speed < 420.0f) {
            in.tether_held = false;
        }
        break;
    }

    case GV_TETHER_FLYING:
        in.tether_held = true;
        in.aim_point = gv_v2_add(p->pos, gv_v2_mul(p->aim, 200.0f));
        break;

    case GV_TETHER_RETRACTING:
    case GV_TETHER_IDLE:
    default: {
        if (can_kill) {
            /* Attack run: steer straight at the target. */
            in.move = to_target;
        } else {
            gv_v2 anchor_pos;
            if (gv_auto_pick_anchor(w, to_target, &anchor_pos)) {
                in.tether_pressed = true;
                in.tether_held = true;
                in.aim_point = anchor_pos;
                /* Thrust across the anchor line to enter the swing with some
                 * tangential speed already. */
                gv_v2 to_anchor = gv_v2_norm_or(gv_v2_sub(anchor_pos, p->pos),
                                                gv_v2_make(1.0f, 0.0f));
                in.move = gv_v2_perp(to_anchor);
                if (gv_v2_dot(in.move, p->vel) < 0.0f) in.move = gv_v2_neg(in.move);
            } else if (target) {
                in.move = to_target;
            } else {
                in.move = gv_v2_polar(gv_rng_angle(jitter), 1.0f);
            }
        }
        break;
    }
    }

    /* A little noise breaks up any limit cycle the policy might settle into. */
    if (gv_rng_chance(jitter, 0.02f)) {
        in.move = gv_v2_norm_or(gv_v2_add(in.move, gv_v2_polar(gv_rng_angle(jitter), 0.6f)),
                                in.move);
    }
    return in;
}
