/* gv_fx.c — particles, shockwaves and score floaters.
 *
 * Effects live inside the world so a replay looks identical, but they draw
 * exclusively from the cosmetic RNG stream and never write back to gameplay
 * state. That separation is what allows the particle-density setting to exist
 * without turning a low-spec run into a different game.
 */
#include "gv_sim.h"
#include "gv_world.h"

void gv_fx_burst(gv_world *w, gv_v2 pos, gv_v2 dir, int count, float speed,
                 uint8_t r, uint8_t g, uint8_t b, int kind)
{
    if (count <= 0) return;

    /* fx_quality of 0 still keeps a single particle for the smallest bursts,
     * so events remain visible at the lowest setting. */
    float quality = (w->fx_quality > 0.0f) ? gv_clampf(w->fx_quality, 0.0f, 1.0f) : 1.0f;
    int wanted = (int)(((float)count * quality) + 0.5f);
    if (wanted < 1) wanted = 1;

    bool directed = gv_v2_len_sq(dir) > GV_EPS;
    gv_v2 base_dir = directed ? gv_v2_norm(dir) : gv_v2_make(1.0f, 0.0f);
    float base_angle = gv_v2_angle(base_dir);

    for (int n = 0; n < wanted; ++n) {
        gv_particle *p = NULL;
        for (int i = 0; i < GV_MAX_PARTICLES; ++i) {
            if (!w->particles[i].alive) {
                p = &w->particles[i];
                break;
            }
        }
        if (!p) return; /* pool exhausted: drop the rest of the burst */

        /* A directed burst forms a cone; an undirected one is a full ring. */
        float spread = directed ? 0.85f : GV_PI;
        float a = base_angle + gv_rng_range_f(&w->fxrng, -spread, spread);
        float s = speed * gv_rng_range_f(&w->fxrng, 0.35f, 1.15f);

        p->alive = true;
        p->kind = (uint8_t)kind;
        p->pos = pos;
        p->angle = a;
        p->r = r;
        p->g = g;
        p->b = b;

        switch (kind) {
        case GV_PART_TRAIL:
            p->vel = gv_v2_polar(a, s * 0.25f);
            p->max_life = gv_rng_range_f(&w->fxrng, 0.22f, 0.36f);
            p->size = gv_rng_range_f(&w->fxrng, 2.6f, 4.4f);
            p->drag = 3.4f;
            p->spin = 0.0f;
            break;
        case GV_PART_SMOKE:
            p->vel = gv_v2_polar(a, s * 0.4f);
            p->max_life = gv_rng_range_f(&w->fxrng, 0.6f, 1.1f);
            p->size = gv_rng_range_f(&w->fxrng, 5.0f, 11.0f);
            p->drag = 1.6f;
            p->spin = gv_rng_range_f(&w->fxrng, -1.5f, 1.5f);
            break;
        case GV_PART_SHARD:
            p->vel = gv_v2_polar(a, s);
            p->max_life = gv_rng_range_f(&w->fxrng, 0.36f, 0.85f);
            p->size = gv_rng_range_f(&w->fxrng, 3.5f, 8.0f);
            p->drag = 1.9f;
            p->spin = gv_rng_range_f(&w->fxrng, -9.0f, 9.0f);
            break;
        case GV_PART_SPARK:
        default:
            p->vel = gv_v2_polar(a, s);
            p->max_life = gv_rng_range_f(&w->fxrng, 0.18f, 0.46f);
            p->size = gv_rng_range_f(&w->fxrng, 1.8f, 3.6f);
            p->drag = 2.6f;
            p->spin = 0.0f;
            break;
        }
        p->life = p->max_life;
    }
}

void gv_fx_shockwave(gv_world *w, gv_v2 pos, float max_radius, float life, float thickness,
                     uint8_t r, uint8_t g, uint8_t b)
{
    if (life <= 0.0f || max_radius <= 0.0f) return;

    for (int i = 0; i < GV_MAX_SHOCKWAVES; ++i) {
        gv_shockwave *s = &w->shockwaves[i];
        if (s->alive) continue;
        s->alive = true;
        s->pos = pos;
        s->radius = 0.0f;
        s->max_radius = max_radius;
        s->life = life;
        s->max_life = life;
        s->thickness = thickness;
        s->r = r;
        s->g = g;
        s->b = b;
        return;
    }
}

void gv_fx_floater(gv_world *w, gv_v2 pos, int value, uint8_t r, uint8_t g, uint8_t b)
{
    for (int i = 0; i < GV_MAX_FLOATERS; ++i) {
        gv_floater *f = &w->floaters[i];
        if (f->alive) continue;
        f->alive = true;
        f->pos = pos;
        f->vel = gv_v2_make(gv_rng_range_f(&w->fxrng, -18.0f, 18.0f), -62.0f);
        f->max_life = 0.95f;
        f->life = f->max_life;
        f->value = value;
        f->scale = 1.0f;
        f->r = r;
        f->g = g;
        f->b = b;
        return;
    }

    /* All slots busy: recycle the one closest to expiring so that the most
     * recent — and therefore most relevant — score always gets shown. */
    int oldest = 0;
    for (int i = 1; i < GV_MAX_FLOATERS; ++i) {
        if (w->floaters[i].life < w->floaters[oldest].life) oldest = i;
    }
    gv_floater *f = &w->floaters[oldest];
    f->alive = true;
    f->pos = pos;
    f->vel = gv_v2_make(0.0f, -62.0f);
    f->max_life = 0.95f;
    f->life = f->max_life;
    f->value = value;
    f->scale = 1.0f;
    f->r = r;
    f->g = g;
    f->b = b;
}

void gv_fx_update(gv_world *w, float dt)
{
    if (dt <= 0.0f) return;

    for (int i = 0; i < GV_MAX_PARTICLES; ++i) {
        gv_particle *p = &w->particles[i];
        if (!p->alive) continue;

        p->life -= dt;
        if (p->life <= 0.0f) {
            p->alive = false;
            continue;
        }
        p->pos = gv_v2_madd(p->pos, p->vel, dt);
        p->vel = gv_v2_mul(p->vel, expf(-p->drag * dt));
        p->angle = gv_wrap_angle(p->angle + p->spin * dt);

        if (p->kind == GV_PART_SMOKE) p->size += 8.0f * dt;
    }

    for (int i = 0; i < GV_MAX_SHOCKWAVES; ++i) {
        gv_shockwave *s = &w->shockwaves[i];
        if (!s->alive) continue;

        s->life -= dt;
        if (s->life <= 0.0f) {
            s->alive = false;
            continue;
        }
        /* Ease-out expansion: fast at the front, settling at the edge. */
        float t = 1.0f - (s->life / s->max_life);
        s->radius = s->max_radius * (1.0f - (1.0f - t) * (1.0f - t));
    }

    for (int i = 0; i < GV_MAX_FLOATERS; ++i) {
        gv_floater *f = &w->floaters[i];
        if (!f->alive) continue;

        f->life -= dt;
        if (f->life <= 0.0f) {
            f->alive = false;
            continue;
        }
        f->pos = gv_v2_madd(f->pos, f->vel, dt);
        f->vel = gv_v2_mul(f->vel, expf(-1.8f * dt));
        /* A quick pop on birth, then a slow settle. */
        float age = 1.0f - (f->life / f->max_life);
        f->scale = 1.0f + 0.35f * expf(-age * 12.0f);
    }

    for (int i = 0; i < w->pylon_count; ++i) {
        w->pylons[i].pulse = gv_wrap_angle(w->pylons[i].pulse + dt * 1.6f);
    }
}
