/* gv_tether.c — the grapple.
 *
 * GRAVITON has no gun. Every point of damage comes from momentum, and the
 * tether is how momentum is made: anchor to something, swing, reel in to trade
 * radius for speed, then release and let inertia do the rest.
 *
 * The rope is modelled as an inextensible distance constraint that only acts
 * when taut (it pulls, never pushes), solved positionally after integration
 * with a matching velocity correction.
 */
#include <string.h>

#include "gv_sim.h"
#include "gv_world.h"

/* Effective inverse mass of each anchor class. Static pylons are immovable;
 * enemies are light enough that swinging on one mostly flings *them*. */
#define GV_PLAYER_INV_MASS 1.0f
#define GV_ENEMY_INV_MASS 1.55f

bool gv_anchor_resolve(gv_world *w, const gv_tether *t, gv_v2 *out_pos, gv_v2 *out_vel,
                       float *out_inv_mass)
{
    switch (t->anchor_kind) {
    case GV_ANCHOR_PYLON: {
        if (t->anchor_index < 0 || t->anchor_index >= w->pylon_count) return false;
        gv_pylon *p = &w->pylons[t->anchor_index];
        if (!p->alive) return false;
        if (out_pos) *out_pos = p->pos;
        if (out_vel) *out_vel = gv_v2_zero();
        if (out_inv_mass) *out_inv_mass = 0.0f;
        return true;
    }
    case GV_ANCHOR_ROCK: {
        if (t->anchor_index < 0 || t->anchor_index >= GV_MAX_ROCKS) return false;
        gv_rock *r = &w->rocks[t->anchor_index];
        if (!r->alive || r->gen != t->anchor_gen) return false;
        if (out_pos) *out_pos = r->pos;
        if (out_vel) *out_vel = r->vel;
        if (out_inv_mass) *out_inv_mass = r->inv_mass;
        return true;
    }
    case GV_ANCHOR_ENEMY: {
        if (t->anchor_index < 0 || t->anchor_index >= GV_MAX_ENEMIES) return false;
        gv_enemy *e = &w->enemies[t->anchor_index];
        if (!e->alive || e->gen != t->anchor_gen) return false;
        if (out_pos) *out_pos = e->pos;
        if (out_vel) *out_vel = e->vel;
        if (out_inv_mass) *out_inv_mass = GV_ENEMY_INV_MASS;
        return true;
    }
    default:
        return false;
    }
}

void gv_anchor_apply_impulse(gv_world *w, const gv_tether *t, gv_v2 impulse)
{
    switch (t->anchor_kind) {
    case GV_ANCHOR_ROCK:
        if (t->anchor_index >= 0 && t->anchor_index < GV_MAX_ROCKS) {
            gv_rock *r = &w->rocks[t->anchor_index];
            if (r->alive && r->gen == t->anchor_gen) {
                r->vel = gv_v2_madd(r->vel, impulse, r->inv_mass);
            }
        }
        break;
    case GV_ANCHOR_ENEMY:
        if (t->anchor_index >= 0 && t->anchor_index < GV_MAX_ENEMIES) {
            gv_enemy *e = &w->enemies[t->anchor_index];
            if (e->alive && e->gen == t->anchor_gen) {
                e->vel = gv_v2_madd(e->vel, impulse, GV_ENEMY_INV_MASS);
            }
        }
        break;
    default:
        break; /* pylons absorb everything */
    }
}

void gv_anchor_clear_flags(gv_world *w)
{
    for (int i = 0; i < GV_MAX_ROCKS; ++i) w->rocks[i].tethered = false;
    for (int i = 0; i < GV_MAX_ENEMIES; ++i) w->enemies[i].tethered = false;
    for (int i = 0; i < w->pylon_count; ++i) w->pylons[i].tethered = false;
}

static void gv_mark_anchor(gv_world *w, const gv_tether *t)
{
    switch (t->anchor_kind) {
    case GV_ANCHOR_PYLON:
        if (t->anchor_index >= 0 && t->anchor_index < w->pylon_count) {
            w->pylons[t->anchor_index].tethered = true;
        }
        break;
    case GV_ANCHOR_ROCK:
        if (t->anchor_index >= 0 && t->anchor_index < GV_MAX_ROCKS) {
            w->rocks[t->anchor_index].tethered = true;
        }
        break;
    case GV_ANCHOR_ENEMY:
        if (t->anchor_index >= 0 && t->anchor_index < GV_MAX_ENEMIES) {
            w->enemies[t->anchor_index].tethered = true;
        }
        break;
    default:
        break;
    }
}

void gv_tether_detach(gv_world *w, bool player_released)
{
    gv_player *p = &w->player;
    gv_tether *t = &p->tether;
    if (t->state == GV_TETHER_IDLE) return;

    bool was_attached = (t->state == GV_TETHER_ATTACHED);

    if (was_attached && player_released) {
        /* Release boost. Scaled by how long the swing lasted so that tapping
         * the button repeatedly cannot farm free speed, and capped in absolute
         * terms so it stays a flourish rather than the main accelerator. */
        float swing_quality = gv_clampf(t->attach_time / 0.45f, 0.0f, 1.0f);
        float speed = gv_v2_len(p->vel);
        if (speed > 40.0f) {
            float boosted = speed * gv_lerpf(1.0f, GV_TETHER_WHIP_BONUS, swing_quality);
            float capped = gv_minf(boosted, speed + GV_TETHER_RELEASE_MAX_BOOST);
            p->vel = gv_v2_mul(p->vel, capped / speed);
            p->vel = gv_v2_clamp_len(p->vel, GV_PLAYER_MAX_SPEED);
        }
        gv_emit(w, GV_EV_TETHER_RELEASE, p->pos, swing_quality, 0);
    }

    t->state = GV_TETHER_RETRACTING;
    t->anchor_kind = GV_ANCHOR_NONE;
    t->anchor_index = -1;
    t->anchor_gen = 0;
    t->taut = 0.0f;
    t->attach_time = 0.0f;
}

/* Sweep the hook from `from` to `to` and attach to the first thing it meets. */
static bool gv_tether_try_attach(gv_world *w, gv_v2 from, gv_v2 to)
{
    gv_player *p = &w->player;
    gv_tether *t = &p->tether;

    gv_v2 delta = gv_v2_sub(to, from);
    float travel = gv_v2_len(delta);
    if (travel < GV_EPS) return false;
    gv_v2 dir = gv_v2_mul(delta, 1.0f / travel);

    float best_t = travel + 1.0f;
    int best_kind = GV_ANCHOR_NONE;
    int best_index = -1;
    uint16_t best_gen = 0;

    /* The hook is given a small radius of its own so that grazing shots latch;
     * a pixel-perfect hook would feel unresponsive at these speeds. */
    const float hook_grace = 9.0f;

    for (int i = 0; i < w->pylon_count; ++i) {
        gv_pylon *py = &w->pylons[i];
        if (!py->alive) continue;
        float hit;
        if (gv_ray_circle(from, dir, py->pos, py->radius + hook_grace, travel, &hit) &&
            hit < best_t) {
            best_t = hit;
            best_kind = GV_ANCHOR_PYLON;
            best_index = i;
            best_gen = 0;
        }
    }

    for (int i = 0; i < GV_MAX_ROCKS; ++i) {
        gv_rock *r = &w->rocks[i];
        if (!r->alive) continue;
        float hit;
        if (gv_ray_circle(from, dir, r->pos, r->radius + hook_grace, travel, &hit) &&
            hit < best_t) {
            best_t = hit;
            best_kind = GV_ANCHOR_ROCK;
            best_index = i;
            best_gen = r->gen;
        }
    }

    for (int i = 0; i < GV_MAX_ENEMIES; ++i) {
        gv_enemy *e = &w->enemies[i];
        if (!e->alive) continue;
        /* Materialising enemies are intangible, so the hook passes through them
         * exactly as the ship does. */
        if (e->spawn_anim < 1.0f) continue;
        float hit;
        if (gv_ray_circle(from, dir, e->pos, e->radius + hook_grace, travel, &hit) &&
            hit < best_t) {
            best_t = hit;
            best_kind = GV_ANCHOR_ENEMY;
            best_index = i;
            best_gen = e->gen;
        }
    }

    if (best_kind == GV_ANCHOR_NONE) return false;

    t->anchor_kind = (uint8_t)best_kind;
    t->anchor_index = (int16_t)best_index;
    t->anchor_gen = best_gen;
    t->tip = gv_v2_madd(from, dir, best_t);
    t->state = GV_TETHER_ATTACHED;
    t->attach_time = 0.0f;

    gv_v2 anchor_pos;
    if (!gv_anchor_resolve(w, t, &anchor_pos, NULL, NULL)) {
        /* Resolution can only fail if the anchor vanished this very step. */
        t->state = GV_TETHER_RETRACTING;
        t->anchor_kind = GV_ANCHOR_NONE;
        t->anchor_index = -1;
        return false;
    }

    t->length = gv_clampf(gv_v2_dist(p->pos, anchor_pos), GV_TETHER_MIN_LEN, GV_TETHER_MAX_LEN);
    gv_emit(w, GV_EV_TETHER_ATTACH, t->tip, 1.0f, best_kind);
    gv_fx_burst(w, t->tip, gv_v2_neg(dir), 8, 190.0f, 150, 255, 220, GV_PART_SPARK);
    return true;
}

void gv_tether_update(gv_world *w, const gv_input *in, float dt)
{
    gv_player *p = &w->player;
    gv_tether *t = &p->tether;

    if (!p->alive) {
        t->state = GV_TETHER_IDLE;
        t->anchor_kind = GV_ANCHOR_NONE;
        t->anchor_index = -1;
        return;
    }

    /* Firing. A fresh press always re-aims, cancelling any existing rope. */
    if (in->tether_pressed) {
        if (t->state == GV_TETHER_ATTACHED || t->state == GV_TETHER_FLYING) {
            gv_tether_detach(w, t->state == GV_TETHER_ATTACHED);
        }
        t->state = GV_TETHER_FLYING;
        t->origin = p->pos;
        t->tip = gv_v2_madd(p->pos, p->aim, p->radius + 2.0f);
        /* Inherit the ship's velocity so shots fired while moving fast lead
         * correctly instead of trailing behind. */
        t->tip_vel = gv_v2_madd(gv_v2_mul(p->aim, GV_TETHER_SPEED), p->vel, 0.55f);
        t->anchor_kind = GV_ANCHOR_NONE;
        t->anchor_index = -1;
        t->taut = 0.0f;
        gv_emit(w, GV_EV_TETHER_FIRE, p->pos, 1.0f, 0);
    }

    /* Letting go of the button drops the rope. */
    if (!in->tether_held && (t->state == GV_TETHER_ATTACHED || t->state == GV_TETHER_FLYING)) {
        gv_tether_detach(w, t->state == GV_TETHER_ATTACHED);
    }

    switch (t->state) {
    case GV_TETHER_FLYING: {
        gv_v2 prev = t->tip;
        gv_v2 next = gv_v2_madd(prev, t->tip_vel, dt);

        if (!gv_tether_try_attach(w, prev, next)) {
            t->tip = next;
            /* Out of rope, or the hook left the arena: reel it back in. */
            bool too_far = gv_v2_dist(p->pos, t->tip) > GV_TETHER_MAX_LEN;
            bool outside = t->tip.x < 0.0f || t->tip.y < 0.0f ||
                           t->tip.x > w->arena_w || t->tip.y > w->arena_h;
            if (too_far || outside) {
                t->state = GV_TETHER_RETRACTING;
                gv_emit(w, GV_EV_TETHER_MISS, t->tip, 0.5f, 0);
            }
        }
        break;
    }

    case GV_TETHER_ATTACHED: {
        gv_v2 anchor_pos;
        if (!gv_anchor_resolve(w, t, &anchor_pos, NULL, NULL)) {
            /* Anchor destroyed underneath us. */
            gv_tether_detach(w, false);
            break;
        }
        t->attach_time += dt;
        t->tip = anchor_pos;
        gv_mark_anchor(w, t);

        /* Reeling. Shortening the rope conserves angular momentum below, which
         * is the main way a skilled player builds speed. */
        if (in->reel != 0.0f) {
            float old_len = t->length;
            float target = t->length - in->reel * GV_TETHER_REEL_SPEED * dt;
            t->length = gv_clampf(target, GV_TETHER_MIN_LEN, GV_TETHER_MAX_LEN);

            if (t->length < old_len - GV_EPS && t->length > GV_EPS) {
                gv_v2 anchor_vel = gv_v2_zero();
                (void)gv_anchor_resolve(w, t, NULL, &anchor_vel, NULL);
                gv_v2 radial = gv_v2_norm_or(gv_v2_sub(p->pos, anchor_pos), gv_v2_make(1.0f, 0.0f));
                gv_v2 tangent = gv_v2_perp(radial);
                gv_v2 rel = gv_v2_sub(p->vel, anchor_vel);

                float vr = gv_v2_dot(rel, radial);
                float vt = gv_v2_dot(rel, tangent);
                /* L = m * v_t * r is conserved as r shrinks. Bound the gain per
                 * step so a single frame cannot multiply speed without limit. */
                float ratio = gv_clampf(old_len / t->length, 1.0f, 1.06f);
                vt *= ratio;

                rel = gv_v2_add(gv_v2_mul(radial, vr), gv_v2_mul(tangent, vt));
                p->vel = gv_v2_clamp_len(gv_v2_add(anchor_vel, rel), GV_PLAYER_MAX_SPEED);
            }
        }
        break;
    }

    case GV_TETHER_RETRACTING: {
        gv_v2 to_ship = gv_v2_sub(p->pos, t->tip);
        float d = gv_v2_len(to_ship);
        float step = GV_TETHER_RETRACT_SPEED * dt;
        if (d <= step + p->radius) {
            t->state = GV_TETHER_IDLE;
            t->tip = p->pos;
            t->taut = 0.0f;
        } else {
            t->tip = gv_v2_madd(t->tip, gv_v2_mul(to_ship, 1.0f / d), step);
        }
        break;
    }

    default:
        t->tip = p->pos;
        t->taut = 0.0f;
        break;
    }
}

void gv_tether_solve(gv_world *w, float dt)
{
    gv_player *p = &w->player;
    gv_tether *t = &p->tether;

    if (t->state != GV_TETHER_ATTACHED) {
        t->taut = gv_approach_exp(t->taut, 0.0f, 12.0f, dt);
        return;
    }

    gv_v2 anchor_pos, anchor_vel;
    float anchor_inv_mass;
    if (!gv_anchor_resolve(w, t, &anchor_pos, &anchor_vel, &anchor_inv_mass)) {
        gv_tether_detach(w, false);
        return;
    }

    gv_v2 d = gv_v2_sub(p->pos, anchor_pos);
    float dist = gv_v2_len(d);

    /* A rope pushes nothing: slack means no constraint at all. */
    if (dist <= t->length || dist < GV_EPS) {
        t->taut = gv_approach_exp(t->taut, 0.0f, 9.0f, dt);
        return;
    }

    gv_v2 n = gv_v2_mul(d, 1.0f / dist);
    float excess = dist - t->length;

    float inv_sum = GV_PLAYER_INV_MASS + anchor_inv_mass;
    if (inv_sum <= GV_EPS) return; /* both immovable: nothing to solve */

    float player_share = GV_PLAYER_INV_MASS / inv_sum;
    float anchor_share = anchor_inv_mass / inv_sum;

    /* Positional correction. */
    gv_v2 correction = gv_v2_mul(n, excess * GV_TETHER_STIFFNESS);
    p->pos = gv_v2_madd(p->pos, correction, -player_share);
    gv_v2 anchor_push = gv_v2_mul(correction, anchor_share);

    /* Velocity correction: cancel the separating radial component. */
    gv_v2 rel = gv_v2_sub(p->vel, anchor_vel);
    float vn = gv_v2_dot(rel, n);
    if (vn > 0.0f) {
        float j = -vn * (1.0f + GV_TETHER_DAMP) / inv_sum;
        p->vel = gv_v2_madd(p->vel, n, j * GV_PLAYER_INV_MASS);
        gv_anchor_apply_impulse(w, t, gv_v2_mul(n, -j));
    }

    /* Move the anchor to satisfy the constraint (rocks and enemies only). */
    if (anchor_share > 0.0f) {
        switch (t->anchor_kind) {
        case GV_ANCHOR_ROCK: {
            gv_rock *r = &w->rocks[t->anchor_index];
            r->pos = gv_v2_add(r->pos, anchor_push);
            break;
        }
        case GV_ANCHOR_ENEMY: {
            gv_enemy *e = &w->enemies[t->anchor_index];
            e->pos = gv_v2_add(e->pos, anchor_push);
            break;
        }
        default:
            break;
        }
    }

    p->vel = gv_v2_clamp_len(p->vel, GV_PLAYER_MAX_SPEED);
    p->speed = gv_v2_len(p->vel);

    /* Tension for presentation: how hard the rope had to work this step. */
    float load = gv_clampf(excess / 26.0f, 0.0f, 1.0f);
    t->taut = gv_approach_exp(t->taut, load, 16.0f, dt);
}
