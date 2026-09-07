/* gv_player.c — the ship: thrust, drag, overdrive, focus and the pulse. */
#include <string.h>

#include "gv_sim.h"
#include "gv_world.h"

/* Emit an omnidirectional shove that clears bullets and buys breathing room.
 * It never kills, so it stays a movement tool rather than a second weapon. */
static void gv_player_fire_pulse(gv_world *w)
{
    gv_player *p = &w->player;

    p->pulse_charge -= 1.0f;
    p->pulse_cooldown = GV_PULSE_COOLDOWN;

    int cleared = 0;
    for (int i = 0; i < GV_MAX_BULLETS; ++i) {
        gv_bullet *b = &w->bullets[i];
        if (!b->alive) continue;
        if (gv_v2_dist_sq(b->pos, p->pos) > GV_PULSE_RADIUS * GV_PULSE_RADIUS) continue;
        b->alive = false;
        cleared++;
        gv_fx_burst(w, b->pos, gv_v2_zero(), 4, 130.0f, 255, 240, 160, GV_PART_SPARK);
    }
    if (cleared > 0) w->score.pulse_saves += cleared;

    for (int i = 0; i < GV_MAX_ENEMIES; ++i) {
        gv_enemy *e = &w->enemies[i];
        if (!e->alive) continue;
        gv_v2 d = gv_v2_sub(e->pos, p->pos);
        float dist = gv_v2_len(d);
        if (dist > GV_PULSE_RADIUS) continue;
        gv_v2 n = gv_v2_norm_or(d, gv_v2_make(1.0f, 0.0f));
        /* Falloff so point-blank pulses shove hard and the edge barely nudges. */
        float falloff = 1.0f - dist / GV_PULSE_RADIUS;
        float power = GV_PULSE_IMPULSE * (0.35f + 0.65f * falloff * falloff);
        /* Wardens are heavy: they resist the shove rather than ignoring it. */
        if (e->kind == GV_ENEMY_WARDEN) power *= 0.45f;
        e->vel = gv_v2_madd(e->vel, n, power);
        e->stagger = gv_maxf(e->stagger, 0.35f + 0.35f * falloff);
        e->hit_flash = 1.0f;
    }

    for (int i = 0; i < GV_MAX_ROCKS; ++i) {
        gv_rock *r = &w->rocks[i];
        if (!r->alive) continue;
        gv_v2 d = gv_v2_sub(r->pos, p->pos);
        float dist = gv_v2_len(d);
        if (dist > GV_PULSE_RADIUS * 1.25f) continue;
        gv_v2 n = gv_v2_norm_or(d, gv_v2_make(1.0f, 0.0f));
        float falloff = 1.0f - gv_minf(1.0f, dist / (GV_PULSE_RADIUS * 1.25f));
        r->vel = gv_v2_madd(r->vel, n, GV_PULSE_IMPULSE * falloff * r->inv_mass * 0.9f);
    }

    /* A small self-kick makes the pulse double as an emergency dodge. */
    gv_v2 back = gv_v2_neg(p->aim);
    p->vel = gv_v2_madd(p->vel, back, GV_PULSE_SELF_KICK);

    gv_fx_shockwave(w, p->pos, GV_PULSE_RADIUS, 0.42f, 5.0f, 120, 240, 255);
    gv_fx_burst(w, p->pos, gv_v2_zero(), 26, 300.0f, 140, 240, 255, GV_PART_SPARK);
    gv_add_shake(w, 0.28f);
    gv_emit(w, GV_EV_PULSE, p->pos, 1.0f, cleared);
}

void gv_player_update(gv_world *w, const gv_input *in, float dt)
{
    gv_player *p = &w->player;
    if (!p->alive) return;

    /* ---- aim ---- */
    gv_v2 to_aim = gv_v2_sub(in->aim_point, p->pos);
    /* Keep the previous facing when the cursor sits exactly on the ship,
     * rather than snapping to an arbitrary angle. */
    p->aim = gv_v2_norm_or(to_aim, p->aim);
    if (gv_v2_len_sq(p->aim) < GV_EPS) p->aim = gv_v2_make(1.0f, 0.0f);
    p->angle = gv_v2_angle(p->aim);

    /* ---- timers ---- */
    p->invuln = gv_maxf(0.0f, p->invuln - dt);
    p->hit_flash = gv_maxf(0.0f, p->hit_flash - dt * 3.0f);
    p->pulse_cooldown = gv_maxf(0.0f, p->pulse_cooldown - dt);
    p->pulse_charge = gv_minf(GV_PULSE_MAX, p->pulse_charge + GV_PULSE_RECHARGE * dt);

    /* ---- focus ---- */
    bool wants_focus = in->focus_held && p->focus > 0.0f;
    if (!p->focus_active && wants_focus && p->focus < GV_FOCUS_MIN_TO_START) {
        /* Require a minimum reserve to re-engage, so a drained meter cannot be
         * chattered on and off for a permanent partial slowdown. */
        wants_focus = false;
    }
    p->focus_active = wants_focus;
    if (p->focus_active) {
        p->focus = gv_maxf(0.0f, p->focus - GV_FOCUS_DRAIN * dt);
        if (p->focus <= 0.0f) p->focus_active = false;
    } else {
        p->focus = gv_minf(GV_FOCUS_MAX, p->focus + GV_FOCUS_REGEN * dt);
    }

    /* ---- pulse ---- */
    if (in->pulse_pressed) {
        if (p->pulse_charge >= 1.0f && p->pulse_cooldown <= 0.0f) {
            gv_player_fire_pulse(w);
        } else {
            gv_emit(w, GV_EV_PULSE_EMPTY, p->pos, 0.3f, 0);
        }
    }

    /* ---- thrust ---- */
    gv_v2 move = gv_v2_clamp_len(in->move, 1.0f);
    if (!gv_v2_finite(move)) move = gv_v2_zero();
    p->thrust_amount = gv_v2_len(move);

    /* Thrust is weaker while swinging: the rope, not the engine, is meant to
     * be the primary source of speed. */
    float authority = (p->tether.state == GV_TETHER_ATTACHED) ? 0.72f : 1.0f;
    p->vel = gv_v2_madd(p->vel, move, GV_PLAYER_THRUST * authority * dt);

    /* Exponential drag: frame-rate independent and asymptotic, so there is no
     * hard speed wall — just diminishing returns. */
    p->vel = gv_v2_mul(p->vel, expf(-GV_PLAYER_DRAG * dt));
    p->vel = gv_v2_clamp_len(p->vel, GV_PLAYER_MAX_SPEED);

    p->speed = gv_v2_len(p->vel);
    p->overdrive = gv_clampf(gv_remap(p->speed, GV_OVERDRIVE_SPEED, GV_OVERDRIVE_FULL, 0.0f, 1.0f),
                             0.0f, 1.0f);
}

void gv_player_integrate(gv_world *w, float dt)
{
    gv_player *p = &w->player;
    if (!p->alive) return;

    p->prev_pos = p->pos;
    p->pos = gv_v2_madd(p->pos, p->vel, dt);

    /* Walls are springy rather than sticky — bouncing preserves the momentum
     * the player worked for instead of dumping it. */
    /* A wall bounce costs 45% of the ship's speed, so the threshold for
     * showing it has to be low: momentum is the player's resource, and losing
     * a chunk of it with no spark and no sound reads as the game cheating. */
    if (gv_confine(&p->pos, &p->vel, p->radius, w->arena_w, w->arena_h, 0.55f)) {
        if (p->speed > 150.0f) {
            gv_add_shake(w, gv_clampf(p->speed / 2600.0f, 0.03f, 0.16f));
            gv_emit(w, GV_EV_WALL_BOUNCE, p->pos, gv_clampf(p->speed / 1200.0f, 0.15f, 1.0f), 1);
            gv_fx_burst(w, p->pos, gv_v2_neg(gv_v2_norm(p->vel)), 8, 200.0f, 120, 220, 255,
                        GV_PART_SPARK);
        }
    }

    p->speed = gv_v2_len(p->vel);

    /* Motion trail. Emitted on a distance-based timer so the ribbon stays even
     * whether the ship is drifting or at full tilt. */
    p->trail_timer -= dt * (1.0f + p->speed * 0.006f);
    if (p->trail_timer <= 0.0f) {
        p->trail_timer = 0.045f;
        if (p->speed > 60.0f) {
            uint8_t r = (uint8_t)gv_lerpf(90.0f, 255.0f, p->overdrive);
            uint8_t g = (uint8_t)gv_lerpf(220.0f, 190.0f, p->overdrive);
            uint8_t b = (uint8_t)gv_lerpf(255.0f, 80.0f, p->overdrive);
            gv_fx_burst(w, p->pos, gv_v2_neg(gv_v2_norm(p->vel)), 1, 30.0f, r, g, b, GV_PART_TRAIL);
        }
    }
}
