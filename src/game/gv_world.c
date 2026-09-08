#include "gv_world.h"

#include <string.h>

#include "gv_sim.h"

/* ------------------------------------------------------------------ tuning */

static const gv_tuning k_tuning[GV_DIFF_COUNT] = {
    /* enemy_speed, enemy_accel, spawn_budget, spawn_rate, bullet_speed, score_mult, lives */
    { 0.84f, 0.82f, 0.78f, 0.80f, 0.82f, 0.85f, 4 }, /* Cadet */
    { 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 3 }, /* Pilot */
    { 1.16f, 1.18f, 1.30f, 1.28f, 1.18f, 1.30f, 2 }, /* Ace   */
};

void gv_tuning_for(gv_tuning *t, int difficulty)
{
    if (!t) return;
    *t = k_tuning[gv_clampi(difficulty, 0, GV_DIFF_COUNT - 1)];
}

const char *gv_difficulty_name(int difficulty)
{
    switch (gv_clampi(difficulty, 0, GV_DIFF_COUNT - 1)) {
    case GV_DIFF_CADET: return "CADET";
    case GV_DIFF_PILOT: return "PILOT";
    default: return "ACE";
    }
}

/* ------------------------------------------------------- enemy archetypes */

float gv_enemy_base_radius(int kind)
{
    switch (kind) {
    /* Sized to stay legible once the camera pulls back: an enemy that reads
     * as a few pixels is an unfair enemy, whatever its behaviour. */
    case GV_ENEMY_DRONE: return 14.0f;
    case GV_ENEMY_LANCER: return 17.0f;
    case GV_ENEMY_SENTINEL: return 21.0f;
    case GV_ENEMY_SPLITTER: return 24.0f;
    case GV_ENEMY_WARDEN: return 38.0f;
    default: return 14.0f;
    }
}

float gv_enemy_base_hp(int kind)
{
    switch (kind) {
    case GV_ENEMY_SENTINEL: return 2.0f;
    case GV_ENEMY_WARDEN: return 3.0f;
    default: return 1.0f;
    }
}

float gv_enemy_base_score(int kind)
{
    switch (kind) {
    case GV_ENEMY_DRONE: return 100.0f;
    case GV_ENEMY_LANCER: return 160.0f;
    case GV_ENEMY_SENTINEL: return 240.0f;
    case GV_ENEMY_SPLITTER: return 190.0f;
    case GV_ENEMY_WARDEN: return 1500.0f;
    default: return 100.0f;
    }
}

/* Extra speed, above the overdrive threshold, needed to slam this archetype.
 * Rendered as a ring around the enemy so the requirement is always legible. */
float gv_enemy_armour(int kind)
{
    switch (kind) {
    case GV_ENEMY_DRONE: return 0.0f;
    case GV_ENEMY_LANCER: return 60.0f;
    case GV_ENEMY_SENTINEL: return 140.0f;
    case GV_ENEMY_SPLITTER: return 40.0f;
    case GV_ENEMY_WARDEN: return 360.0f;
    default: return 0.0f;
    }
}

float gv_enemy_cost(int kind)
{
    switch (kind) {
    case GV_ENEMY_DRONE: return 1.0f;
    case GV_ENEMY_LANCER: return 1.6f;
    case GV_ENEMY_SENTINEL: return 2.2f;
    case GV_ENEMY_SPLITTER: return 2.6f;
    case GV_ENEMY_WARDEN: return 8.0f;
    default: return 1.0f;
    }
}

/* -------------------------------------------------------------- generations */

static uint16_t gv_next_gen(gv_world *w)
{
    /* Generation 0 is reserved to mean "no anchor", so skip it on wrap. */
    w->next_gen = (uint16_t)(w->next_gen + 1u);
    if (w->next_gen == 0u) w->next_gen = 1u;
    return w->next_gen;
}

/* ------------------------------------------------------------ event queue */

void gv_emit(gv_world *w, int kind, gv_v2 pos, float magnitude, int payload)
{
    if (w->event_count >= GV_MAX_EVENTS) {
        /* Dropping is preferable to stalling the simulation; the flag lets the
         * tests assert that this never happens under normal play. */
        w->events_dropped = true;
        return;
    }
    gv_event *e = &w->events[w->event_count++];
    e->kind = (uint8_t)kind;
    e->pos = pos;
    e->magnitude = magnitude;
    e->a = payload;
}

void gv_world_clear_events(gv_world *w)
{
    w->event_count = 0;
    w->events_dropped = false;
}

/* ---------------------------------------------------------------- spawning */

gv_enemy *gv_spawn_enemy(gv_world *w, int kind, gv_v2 pos, int split_depth)
{
    if (kind < 0 || kind >= GV_ENEMY_KIND_COUNT) return NULL;
    for (int i = 0; i < GV_MAX_ENEMIES; ++i) {
        gv_enemy *e = &w->enemies[i];
        if (e->alive) continue;

        memset(e, 0, sizeof(*e));
        e->alive = true;
        e->kind = (uint8_t)kind;
        e->gen = gv_next_gen(w);
        e->pos = pos;
        e->vel = gv_v2_zero();
        e->radius = gv_enemy_base_radius(kind);
        e->max_hp = gv_enemy_base_hp(kind);
        e->hp = e->max_hp;
        e->score = gv_enemy_base_score(kind);
        e->armour = gv_enemy_armour(kind);
        e->split_depth = (uint8_t)gv_clampi(split_depth, 0, 4);
        e->angle = gv_rng_angle(&w->rng);
        e->spin = gv_rng_range_f(&w->rng, -1.2f, 1.2f);
        e->ai = GV_EAI_SEEK;
        e->spawn_anim = 0.0f;
        e->fire_timer = gv_rng_range_f(&w->rng, 0.7f, 2.1f);

        /* Children of a splitter are smaller and quicker than a fresh drone. */
        float shrink = 1.0f - 0.22f * (float)e->split_depth;
        e->radius *= shrink;

        switch (kind) {
        case GV_ENEMY_DRONE:
            e->accel = 560.0f / shrink;
            e->max_speed = 285.0f / shrink;
            break;
        case GV_ENEMY_LANCER:
            e->accel = 400.0f;
            e->max_speed = 250.0f;
            e->ai = GV_EAI_SEEK;
            e->timer = gv_rng_range_f(&w->rng, 0.4f, 1.1f);
            break;
        case GV_ENEMY_SENTINEL:
            e->accel = 180.0f;
            e->max_speed = 115.0f;
            e->ai = GV_EAI_ORBIT;
            break;
        case GV_ENEMY_SPLITTER:
            e->accel = 340.0f;
            e->max_speed = 205.0f;
            break;
        case GV_ENEMY_WARDEN:
            e->accel = 300.0f;
            e->max_speed = 235.0f;
            break;
        default:
            break;
        }

        e->accel *= w->tuning.enemy_accel;
        e->max_speed *= w->tuning.enemy_speed;

        w->enemy_count++;
        gv_emit(w, GV_EV_ENEMY_SPAWN, pos, 0.4f, kind);
        gv_fx_shockwave(w, pos, e->radius * 3.4f, 0.42f, 2.0f, 190, 90, 255);
        return e;
    }
    return NULL; /* pool exhausted: drop the spawn */
}

gv_rock *gv_spawn_rock(gv_world *w, gv_v2 pos, float radius)
{
    for (int i = 0; i < GV_MAX_ROCKS; ++i) {
        gv_rock *r = &w->rocks[i];
        if (r->alive) continue;

        memset(r, 0, sizeof(*r));
        r->alive = true;
        r->gen = gv_next_gen(w);
        r->pos = pos;
        r->radius = gv_clampf(radius, 14.0f, 64.0f);
        /* Mass scales with area so big rocks feel genuinely heavy on the rope. */
        float area = r->radius * r->radius;
        r->inv_mass = 900.0f / (area * 2.4f);
        r->inv_mass = gv_clampf(r->inv_mass, 0.05f, 1.4f);
        r->vel = gv_v2_polar(gv_rng_angle(&w->rng), gv_rng_range_f(&w->rng, 8.0f, 46.0f));
        r->angle = gv_rng_angle(&w->rng);
        r->spin = gv_rng_range_f(&w->rng, -0.7f, 0.7f);
        for (int v = 0; v < GV_ROCK_VERTS; ++v) {
            r->verts[v] = gv_rng_range_f(&w->rng, 0.74f, 1.16f);
        }
        w->rock_count++;
        return r;
    }
    return NULL;
}

gv_bullet *gv_spawn_bullet(gv_world *w, gv_v2 pos, gv_v2 vel, float radius, float life)
{
    for (int i = 0; i < GV_MAX_BULLETS; ++i) {
        gv_bullet *b = &w->bullets[i];
        if (b->alive) continue;
        b->alive = true;
        b->pos = pos;
        b->vel = vel;
        b->radius = radius;
        b->life = life;
        return b;
    }
    return NULL;
}

gv_pickup *gv_spawn_pickup(gv_world *w, int kind, gv_v2 pos, gv_v2 vel)
{
    if (kind < 0 || kind >= GV_PICKUP_KIND_COUNT) return NULL;
    for (int i = 0; i < GV_MAX_PICKUPS; ++i) {
        gv_pickup *p = &w->pickups[i];
        if (p->alive) continue;
        p->alive = true;
        p->kind = (uint8_t)kind;
        p->pos = pos;
        p->vel = vel;
        p->life = 12.0f;
        p->bob = gv_rng_f01(&w->fxrng) * GV_TAU;
        return p;
    }
    return NULL;
}

/* ------------------------------------------------------------------ arena */

bool gv_confine(gv_v2 *pos, gv_v2 *vel, float radius, float arena_w, float arena_h,
                float restitution)
{
    bool hit = false;

    /* A radius larger than the arena would make the min and max bounds cross;
     * pin to the centre instead of oscillating. */
    if (arena_w <= radius * 2.0f) {
        pos->x = arena_w * 0.5f;
        vel->x = 0.0f;
        hit = true;
    } else if (pos->x < radius) {
        pos->x = radius;
        if (vel->x < 0.0f) vel->x = -vel->x * restitution;
        hit = true;
    } else if (pos->x > arena_w - radius) {
        pos->x = arena_w - radius;
        if (vel->x > 0.0f) vel->x = -vel->x * restitution;
        hit = true;
    }

    if (arena_h <= radius * 2.0f) {
        pos->y = arena_h * 0.5f;
        vel->y = 0.0f;
        hit = true;
    } else if (pos->y < radius) {
        pos->y = radius;
        if (vel->y < 0.0f) vel->y = -vel->y * restitution;
        hit = true;
    } else if (pos->y > arena_h - radius) {
        pos->y = arena_h - radius;
        if (vel->y > 0.0f) vel->y = -vel->y * restitution;
        hit = true;
    }
    return hit;
}

gv_v2 gv_pick_spawn_pos(gv_world *w, gv_v2 avoid, float min_dist, float radius)
{
    float margin = radius + 26.0f;
    float max_x = gv_maxf(margin, w->arena_w - margin);
    float max_y = gv_maxf(margin, w->arena_h - margin);

    gv_v2 best = gv_v2_make(w->arena_w * 0.5f, margin);
    float best_dist = -1.0f;

    for (int attempt = 0; attempt < 24; ++attempt) {
        /* Bias toward the perimeter: enemies arriving from the edges read more
         * clearly than ones appearing next to the player. */
        gv_v2 p;
        int side = (int)gv_rng_below(&w->rng, 4u);
        float t = gv_rng_f01(&w->rng);
        float inset = gv_rng_range_f(&w->rng, 0.0f, 190.0f);
        switch (side) {
        case 0: p = gv_v2_make(gv_lerpf(margin, max_x, t), margin + inset); break;
        case 1: p = gv_v2_make(gv_lerpf(margin, max_x, t), max_y - inset); break;
        case 2: p = gv_v2_make(margin + inset, gv_lerpf(margin, max_y, t)); break;
        default: p = gv_v2_make(max_x - inset, gv_lerpf(margin, max_y, t)); break;
        }
        p.x = gv_clampf(p.x, margin, max_x);
        p.y = gv_clampf(p.y, margin, max_y);

        float d = gv_v2_dist(p, avoid);
        if (d >= min_dist) return p;
        if (d > best_dist) {
            best_dist = d;
            best = p;
        }
    }
    /* Every candidate was too close (a cramped arena): use the farthest one. */
    return best;
}

/* ------------------------------------------------------------------- death */

void gv_kill_enemy(gv_world *w, gv_enemy *e, int cause, gv_v2 impact_dir, float impact_speed)
{
    if (!e || !e->alive) return;

    gv_v2 pos = e->pos;
    int kind = e->kind;
    int depth = e->split_depth;
    float radius = e->radius;

    e->alive = false;
    e->tethered = false;
    w->enemy_count--;
    if (w->enemy_count < 0) w->enemy_count = 0;

    /* If the player was swinging on this enemy, the rope has to let go. */
    if (w->player.tether.anchor_kind == GV_ANCHOR_ENEMY &&
        w->player.tether.anchor_index >= 0 &&
        w->player.tether.anchor_index < GV_MAX_ENEMIES &&
        &w->enemies[w->player.tether.anchor_index] == e) {
        gv_tether_detach(w, false);
    }

    if (cause != GV_KILL_CULL) {
        gv_score_register_kill(w, e, cause);

        uint8_t cr = 255, cg = 90, cb = 120;
        switch (kind) {
        case GV_ENEMY_DRONE: cr = 255; cg = 96; cb = 140; break;
        case GV_ENEMY_LANCER: cr = 255; cg = 170; cb = 70; break;
        case GV_ENEMY_SENTINEL: cr = 120; cg = 200; cb = 255; break;
        case GV_ENEMY_SPLITTER: cr = 180; cg = 255; cb = 120; break;
        case GV_ENEMY_WARDEN: cr = 255; cg = 70; cb = 220; break;
        default: break;
        }

        int count = (kind == GV_ENEMY_WARDEN) ? 54 : 22;
        gv_fx_burst(w, pos, impact_dir, count, 260.0f + impact_speed * 0.35f, cr, cg, cb,
                    GV_PART_SHARD);
        gv_fx_shockwave(w, pos, radius * (kind == GV_ENEMY_WARDEN ? 7.0f : 3.6f),
                        kind == GV_ENEMY_WARDEN ? 0.7f : 0.4f, 3.0f, cr, cg, cb);
        gv_add_shake(w, kind == GV_ENEMY_WARDEN ? 0.55f : 0.16f);
        gv_add_hitstop(w, kind == GV_ENEMY_WARDEN ? 0.10f : 0.035f);
        gv_emit(w, GV_EV_ENEMY_KILLED, pos, kind == GV_ENEMY_WARDEN ? 1.0f : 0.55f, kind);

        /* Drops. Rolled on the gameplay stream so a seed reproduces exactly. */
        if (gv_rng_chance(&w->rng, 0.22f)) {
            gv_spawn_pickup(w, GV_PICKUP_CORE, pos,
                            gv_v2_polar(gv_rng_angle(&w->rng), gv_rng_range_f(&w->rng, 30.0f, 90.0f)));
        }
        if (gv_rng_chance(&w->rng, 0.05f)) {
            gv_spawn_pickup(w, GV_PICKUP_PULSE, pos,
                            gv_v2_polar(gv_rng_angle(&w->rng), gv_rng_range_f(&w->rng, 20.0f, 60.0f)));
        }
        if (w->player.lives < GV_PLAYER_MAX_LIVES && gv_rng_chance(&w->rng, 0.012f)) {
            gv_spawn_pickup(w, GV_PICKUP_LIFE, pos,
                            gv_v2_polar(gv_rng_angle(&w->rng), gv_rng_range_f(&w->rng, 10.0f, 40.0f)));
        }

        /* Splitters seed a ring of drones, bounded by split_depth so a chain of
         * splits can never grow without limit. */
        if (kind == GV_ENEMY_SPLITTER && depth < 1) {
            int children = 3;
            for (int i = 0; i < children; ++i) {
                float a = (float)i / (float)children * GV_TAU + gv_rng_angle(&w->rng);
                gv_v2 cp = gv_v2_madd(pos, gv_v2_from_angle(a), radius + 6.0f);
                cp.x = gv_clampf(cp.x, 10.0f, w->arena_w - 10.0f);
                cp.y = gv_clampf(cp.y, 10.0f, w->arena_h - 10.0f);
                gv_enemy *child = gv_spawn_enemy(w, GV_ENEMY_DRONE, cp, depth + 1);
                if (child) {
                    child->vel = gv_v2_polar(a, 210.0f);
                    child->spawn_anim = 0.55f; /* children materialise faster */
                }
            }
        }
    }
}

void gv_damage_player(gv_world *w, gv_v2 from)
{
    gv_player *p = &w->player;
    if (!p->alive || p->invuln > 0.0f) return;

    p->lives--;
    p->invuln = GV_PLAYER_INVULN;
    p->hit_flash = 1.0f;
    gv_add_shake(w, 0.75f);
    gv_add_hitstop(w, 0.12f);

    gv_v2 away = gv_v2_norm_or(gv_v2_sub(p->pos, from), gv_v2_make(0.0f, -1.0f));
    p->vel = gv_v2_madd(gv_v2_mul(p->vel, 0.25f), away, 320.0f);

    /* Losing a life snaps the rope: you cannot keep a swing through a hit. */
    if (p->tether.state != GV_TETHER_IDLE) gv_tether_detach(w, false);

    /* Reset the chain — the multiplier is the reward for staying untouched. */
    w->score.multiplier = 1;
    w->score.mult_timer = 0.0f;
    w->score.combo = 0;

    gv_fx_burst(w, p->pos, away, 34, 420.0f, 255, 220, 120, GV_PART_SPARK);
    gv_fx_shockwave(w, p->pos, 220.0f, 0.55f, 4.0f, 255, 210, 120);
    gv_emit(w, GV_EV_PLAYER_HIT, p->pos, 1.0f, p->lives);

    if (p->lives <= 0) {
        p->lives = 0;
        p->alive = false;
        w->run_state = GV_RUN_DYING;
        w->death_timer = 2.1f;
        gv_add_shake(w, 1.0f);
        gv_fx_burst(w, p->pos, gv_v2_zero(), 90, 620.0f, 120, 230, 255, GV_PART_SHARD);
        gv_fx_shockwave(w, p->pos, 520.0f, 1.1f, 6.0f, 150, 240, 255);
        gv_emit(w, GV_EV_PLAYER_DIED, p->pos, 1.0f, 0);
    }
}

/* ------------------------------------------------------------------- shake */

void gv_add_shake(gv_world *w, float trauma)
{
    w->shake = gv_clampf(w->shake + trauma, 0.0f, 1.0f);
}

void gv_add_hitstop(gv_world *w, float seconds)
{
    if (seconds > w->hitstop) w->hitstop = seconds;
    if (w->hitstop > 0.22f) w->hitstop = 0.22f; /* never let it feel like a freeze */
}

/* -------------------------------------------------------------------- init */

static void gv_place_pylons(gv_world *w)
{
    /* A jittered 3x3 lattice. Regular enough that players learn the spacing,
     * irregular enough that arenas do not feel like graph paper.
     *
     * Count is a design choice, not a decoration budget: anchors should be a
     * decision, not scenery. At this spacing nowhere in the arena is further
     * than a rope length from one, so the tether is always available, but the
     * player still has to pick which anchor sets up the shot they want. */
    const int cols = 3, rows = 3;
    float mx = w->arena_w / (float)(cols + 1);
    float my = w->arena_h / (float)(rows + 1);
    w->pylon_count = 0;

    for (int cy = 1; cy <= rows; ++cy) {
        for (int cx = 1; cx <= cols; ++cx) {
            if (w->pylon_count >= GV_MAX_PYLONS) return;
            gv_pylon *p = &w->pylons[w->pylon_count++];
            p->alive = true;
            p->tethered = false;
            p->radius = 20.0f;
            p->pulse = gv_rng_f01(&w->fxrng) * GV_TAU;
            p->pos = gv_v2_make(mx * (float)cx + gv_rng_range_f(&w->rng, -74.0f, 74.0f),
                                my * (float)cy + gv_rng_range_f(&w->rng, -62.0f, 62.0f));
            p->pos.x = gv_clampf(p->pos.x, 90.0f, w->arena_w - 90.0f);
            p->pos.y = gv_clampf(p->pos.y, 90.0f, w->arena_h - 90.0f);

            /* An odd column and row count puts one cell exactly on the player's
             * spawn point. Push any pylon out of that pocket so the run never
             * starts with the ship wedged against an anchor. */
            gv_v2 centre = gv_v2_make(w->arena_w * 0.5f, w->arena_h * 0.5f);
            gv_v2 out = gv_v2_sub(p->pos, centre);
            const float keep_clear = 230.0f;
            if (gv_v2_len(out) < keep_clear) {
                gv_v2 dir = gv_v2_norm_or(out, gv_v2_from_angle(gv_rng_angle(&w->rng)));
                p->pos = gv_v2_madd(centre, dir, keep_clear);
            }
        }
    }
}

static void gv_place_rocks(gv_world *w, int count)
{
    gv_v2 centre = gv_v2_make(w->arena_w * 0.5f, w->arena_h * 0.5f);
    for (int i = 0; i < count; ++i) {
        float radius = gv_rng_range_f(&w->rng, 20.0f, 46.0f);
        gv_v2 pos = gv_v2_zero();
        /* Reject positions that overlap the player's start or an existing rock. */
        for (int attempt = 0; attempt < 40; ++attempt) {
            pos = gv_v2_make(gv_rng_range_f(&w->rng, radius + 40.0f, w->arena_w - radius - 40.0f),
                             gv_rng_range_f(&w->rng, radius + 40.0f, w->arena_h - radius - 40.0f));
            if (gv_v2_dist(pos, centre) < 260.0f) continue;
            bool clash = false;
            for (int j = 0; j < GV_MAX_ROCKS && !clash; ++j) {
                if (!w->rocks[j].alive) continue;
                if (gv_v2_dist(pos, w->rocks[j].pos) < radius + w->rocks[j].radius + 30.0f) {
                    clash = true;
                }
            }
            for (int j = 0; j < w->pylon_count && !clash; ++j) {
                if (gv_v2_dist(pos, w->pylons[j].pos) < radius + 70.0f) clash = true;
            }
            if (!clash) break;
        }
        gv_spawn_rock(w, pos, radius);
    }
}

void gv_world_init(gv_world *w, uint64_t seed, int difficulty)
{
    memset(w, 0, sizeof(*w));

    w->seed = seed;
    w->difficulty = gv_clampi(difficulty, 0, GV_DIFF_COUNT - 1);
    gv_tuning_for(&w->tuning, w->difficulty);
    /* Two streams from the same seed: gameplay and cosmetics never interleave. */
    gv_rng_seed(&w->rng, seed, 1u);
    gv_rng_seed(&w->fxrng, seed ^ 0x9E3779B97F4A7C15ull, 2u);

    w->next_gen = 1u;
    w->arena_w = GV_ARENA_W;
    w->arena_h = GV_ARENA_H;
    w->time_scale = 1.0f;
    w->fx_quality = 1.0f;
    w->run_state = GV_RUN_PLAYING;

    gv_place_pylons(w);
    gv_place_rocks(w, 10);

    gv_player *p = &w->player;
    p->alive = true;
    p->pos = gv_v2_make(w->arena_w * 0.5f, w->arena_h * 0.5f);
    p->vel = gv_v2_zero();
    p->aim = gv_v2_make(1.0f, 0.0f);
    p->angle = 0.0f;
    p->radius = GV_PLAYER_RADIUS;
    p->lives = w->tuning.start_lives;
    p->invuln = GV_PLAYER_SPAWN_GRACE;
    p->pulse_charge = GV_PULSE_MAX;
    p->focus = GV_FOCUS_MAX;
    p->tether.state = GV_TETHER_IDLE;
    p->tether.anchor_kind = GV_ANCHOR_NONE;
    p->tether.anchor_index = -1;

    w->score.multiplier = 1;
    w->wave.index = 0;
    w->wave.state = GV_WAVE_INTRO;
    w->wave.timer = 2.4f;
}

int gv_world_alive_enemies(const gv_world *w)
{
    int n = 0;
    for (int i = 0; i < GV_MAX_ENEMIES; ++i) {
        if (w->enemies[i].alive) n++;
    }
    return n;
}

bool gv_world_is_over(const gv_world *w)
{
    return w->run_state == GV_RUN_GAME_OVER;
}

int64_t gv_world_final_score(const gv_world *w)
{
    return w->score.score;
}

/* -------------------------------------------------------------------- step */

static void gv_pickups_update(gv_world *w, float dt)
{
    gv_player *p = &w->player;
    for (int i = 0; i < GV_MAX_PICKUPS; ++i) {
        gv_pickup *k = &w->pickups[i];
        if (!k->alive) continue;

        k->life -= dt;
        k->bob += dt * 3.4f;
        if (k->life <= 0.0f) {
            k->alive = false;
            continue;
        }

        /* Magnetise toward the player once close, so a fast pass still collects. */
        if (p->alive) {
            gv_v2 to_player = gv_v2_sub(p->pos, k->pos);
            float d = gv_v2_len(to_player);
            if (d < 260.0f && d > GV_EPS) {
                float pull = gv_remap(d, 260.0f, 40.0f, 260.0f, 1500.0f);
                k->vel = gv_v2_madd(k->vel, gv_v2_mul(to_player, 1.0f / d), pull * dt);
            }
        }

        k->vel = gv_v2_mul(k->vel, 1.0f - gv_minf(1.0f, 0.9f * dt));
        k->pos = gv_v2_madd(k->pos, k->vel, dt);
        gv_confine(&k->pos, &k->vel, 8.0f, w->arena_w, w->arena_h, 0.5f);
    }
}

static void gv_bullets_update(gv_world *w, float dt)
{
    for (int i = 0; i < GV_MAX_BULLETS; ++i) {
        gv_bullet *b = &w->bullets[i];
        if (!b->alive) continue;
        b->life -= dt;
        if (b->life <= 0.0f) {
            b->alive = false;
            continue;
        }
        b->pos = gv_v2_madd(b->pos, b->vel, dt);
        /* Bullets die at the walls rather than bouncing: ricochets off-screen
         * are the classic source of unfair, unreadable hits. */
        if (b->pos.x < -20.0f || b->pos.y < -20.0f ||
            b->pos.x > w->arena_w + 20.0f || b->pos.y > w->arena_h + 20.0f) {
            b->alive = false;
        }
    }
}

static void gv_rocks_update(gv_world *w, float dt)
{
    for (int i = 0; i < GV_MAX_ROCKS; ++i) {
        gv_rock *r = &w->rocks[i];
        if (!r->alive) continue;

        r->hit_flash = gv_maxf(0.0f, r->hit_flash - dt * 3.5f);
        /* Rocks bleed speed slowly so a whipped rock stays dangerous for a
         * while but the arena eventually settles back down. */
        float damp = r->tethered ? 0.02f : 0.30f;
        r->vel = gv_v2_mul(r->vel, expf(-damp * dt));
        r->vel = gv_v2_clamp_len(r->vel, 2200.0f);
        r->prev_pos = r->pos;
        r->pos = gv_v2_madd(r->pos, r->vel, dt);
        r->angle = gv_wrap_angle(r->angle + r->spin * dt);

        if (gv_confine(&r->pos, &r->vel, r->radius, w->arena_w, w->arena_h, 0.62f)) {
            if (gv_v2_len_sq(r->vel) > 300.0f * 300.0f) {
                gv_emit(w, GV_EV_WALL_BOUNCE, r->pos, 0.4f, 0);
            }
        }
    }

    /* Rock-vs-rock separation. O(n^2) over a 48-slot pool is a few hundred
     * checks per step — cheaper than maintaining a spatial index. */
    for (int i = 0; i < GV_MAX_ROCKS; ++i) {
        gv_rock *a = &w->rocks[i];
        if (!a->alive) continue;
        for (int j = i + 1; j < GV_MAX_ROCKS; ++j) {
            gv_rock *b = &w->rocks[j];
            if (!b->alive) continue;

            gv_v2 d = gv_v2_sub(b->pos, a->pos);
            float min_d = a->radius + b->radius;
            float dist_sq = gv_v2_len_sq(d);
            if (dist_sq >= min_d * min_d) continue;

            float dist = sqrtf(dist_sq);
            gv_v2 n = (dist > GV_EPS) ? gv_v2_mul(d, 1.0f / dist)
                                      : gv_v2_from_angle(gv_rng_angle(&w->fxrng));
            float overlap = min_d - dist;
            float inv_sum = a->inv_mass + b->inv_mass;
            if (inv_sum <= GV_EPS) continue;

            a->pos = gv_v2_madd(a->pos, n, -overlap * (a->inv_mass / inv_sum));
            b->pos = gv_v2_madd(b->pos, n, overlap * (b->inv_mass / inv_sum));

            float rel = gv_v2_dot(gv_v2_sub(b->vel, a->vel), n);
            if (rel >= 0.0f) continue;
            float impulse = -(1.0f + 0.45f) * rel / inv_sum;
            a->vel = gv_v2_madd(a->vel, n, -impulse * a->inv_mass);
            b->vel = gv_v2_madd(b->vel, n, impulse * b->inv_mass);
        }
    }
}

void gv_world_step(gv_world *w, const gv_input *in, float dt)
{
    gv_input fallback;
    if (!in) {
        memset(&fallback, 0, sizeof(fallback));
        fallback.aim_point = gv_v2_add(w->player.pos, gv_v2_make(1.0f, 0.0f));
        in = &fallback;
    }

    /* Reject non-finite or non-positive steps outright, and cap absurd ones:
     * a bad dt must never be able to teleport entities through the world. */
    if (!isfinite(dt) || dt <= 0.0f) return;
    dt = gv_minf(dt, 0.05f);

    /* Hit-stop: a few frames of frozen simulation on heavy impacts. Effects
     * keep running at a reduced rate so the freeze reads as impact, not a hang. */
    if (w->hitstop > 0.0f) {
        w->hitstop -= dt;
        gv_fx_update(w, dt * 0.35f);
        w->shake = gv_maxf(0.0f, w->shake - dt * 0.6f);
        return;
    }

    w->time += dt;
    w->shake = gv_maxf(0.0f, w->shake - dt * 1.55f);

    if (w->run_state == GV_RUN_DYING) {
        w->death_timer -= dt;
        /* The arena keeps moving during the death beat, which reads far better
         * than an abrupt cut to the game-over screen. */
        gv_enemies_update(w, dt);
        gv_enemies_integrate(w, dt);
        gv_rocks_update(w, dt);
        gv_bullets_update(w, dt);
        gv_fx_update(w, dt);
        if (w->death_timer <= 0.0f) w->run_state = GV_RUN_GAME_OVER;
        return;
    }

    if (w->run_state == GV_RUN_GAME_OVER) {
        gv_fx_update(w, dt);
        return;
    }

    w->score.time_alive += dt;

    gv_anchor_clear_flags(w);
    gv_player_update(w, in, dt);
    gv_tether_update(w, in, dt);
    gv_enemies_update(w, dt);

    gv_player_integrate(w, dt);
    gv_enemies_integrate(w, dt);
    gv_rocks_update(w, dt);
    gv_bullets_update(w, dt);
    gv_pickups_update(w, dt);

    /* Constraints run after integration so the rope corrects this step's
     * motion rather than lagging a frame behind it. */
    gv_tether_solve(w, dt);

    gv_collide_all(w, dt);
    gv_wave_update(w, dt);
    gv_score_update(w, dt);
    gv_fx_update(w, dt);

    /* The player's focus meter drives the app's time scale. Keeping it here
     * means a replay slows down in exactly the same places. */
    w->time_scale = w->player.focus_active ? GV_FOCUS_TIMESCALE : 1.0f;
}
