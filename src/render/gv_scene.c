#include "gv_scene.h"

#include <stdlib.h>
#include <string.h>

#include "../core/gv_rng.h"

#define GV_STAR_COUNT 520
#define GV_STAR_LAYERS 3

/* The camera leads the ship by up to this many pixels at full speed, so the
 * player sees where they are going rather than where they have been. */
#define GV_CAM_LEAD 220.0f
#define GV_CAM_SMOOTH 6.5f
/* Framing: pulled back by default so most of the arena is on screen, and
 * further at speed so there is time to read what is coming. */
#define GV_CAM_ZOOM_BASE 0.84f
#define GV_CAM_ZOOM_MIN 0.74f

typedef struct {
    gv_v2 pos;
    float size;
    float bright;
    uint8_t layer;
} gv_star;

struct gv_scene {
    gv_star stars[GV_STAR_COUNT];
    gv_camera camera;
    gv_v2 cam_target;
    float shake_time;
    float zoom_current;
    float flash;        /* full-screen flash, driven by big events */
    gv_scene_options opt;
    gv_rng rng;
};

/* ------------------------------------------------------------------ palette */

static const gv_color k_col_player = { 100, 230, 255, 255 };
static const gv_color k_col_overdrive = { 255, 176, 70, 255 };
static const gv_color k_col_tether = { 124, 255, 196, 255 };
static const gv_color k_col_tether_taut = { 255, 120, 120, 255 };
static const gv_color k_col_pylon = { 122, 132, 255, 255 };
static const gv_color k_col_rock = { 138, 160, 184, 255 };
static const gv_color k_col_bullet = { 255, 200, 100, 255 };
static const gv_color k_col_wall = { 90, 130, 220, 255 };
static const gv_color k_col_grid = { 26, 40, 74, 255 };

static gv_color gv_enemy_colour(int kind)
{
    switch (kind) {
    case GV_ENEMY_DRONE: return gv_rgb(255, 96, 144);
    case GV_ENEMY_LANCER: return gv_rgb(255, 170, 70);
    case GV_ENEMY_SENTINEL: return gv_rgb(120, 200, 255);
    case GV_ENEMY_SPLITTER: return gv_rgb(180, 255, 120);
    case GV_ENEMY_WARDEN: return gv_rgb(255, 70, 220);
    default: return gv_rgb(255, 255, 255);
    }
}

/* ------------------------------------------------------------------ lifetime */

gv_scene *gv_scene_create(uint64_t seed)
{
    gv_scene *s = (gv_scene *)calloc(1, sizeof(gv_scene));
    if (!s) return NULL;

    gv_rng_seed(&s->rng, seed, 77u);
    for (int i = 0; i < GV_STAR_COUNT; ++i) {
        gv_star *st = &s->stars[i];
        st->layer = (uint8_t)gv_rng_below(&s->rng, GV_STAR_LAYERS);
        /* Spread stars over more than the arena so parallax never runs out of
         * sky at the edges. */
        st->pos = gv_v2_make(gv_rng_range_f(&s->rng, -400.0f, GV_ARENA_W + 400.0f),
                             gv_rng_range_f(&s->rng, -400.0f, GV_ARENA_H + 400.0f));
        st->size = gv_rng_range_f(&s->rng, 0.7f, 2.3f);
        st->bright = gv_rng_range_f(&s->rng, 0.25f, 1.0f);
    }

    s->camera.zoom = GV_CAM_ZOOM_BASE;
    s->zoom_current = GV_CAM_ZOOM_BASE;
    s->opt.screen_shake = true;
    s->opt.motion_blur_trails = true;
    s->opt.particle_density = 1.0f;
    s->opt.show_offscreen_markers = true;
    return s;
}

void gv_scene_destroy(gv_scene *s)
{
    free(s);
}

void gv_scene_set_options(gv_scene *s, const gv_scene_options *opt)
{
    if (!s || !opt) return;
    s->opt = *opt;
}

/* -------------------------------------------------------------------- camera */

/* Keep the view inside the arena so the player never sees dead space. */
static void gv_camera_clamp(gv_camera *cam, const gv_world *w, float view_w, float view_h)
{
    float half_w = view_w * 0.5f / cam->zoom;
    float half_h = view_h * 0.5f / cam->zoom;

    if (w->arena_w <= half_w * 2.0f) {
        cam->pos.x = w->arena_w * 0.5f;
    } else {
        cam->pos.x = gv_clampf(cam->pos.x, half_w, w->arena_w - half_w);
    }
    if (w->arena_h <= half_h * 2.0f) {
        cam->pos.y = w->arena_h * 0.5f;
    } else {
        cam->pos.y = gv_clampf(cam->pos.y, half_h, w->arena_h - half_h);
    }
}

void gv_scene_reset(gv_scene *s, const gv_world *w)
{
    if (!s || !w) return;
    s->camera.pos = w->player.pos;
    s->cam_target = w->player.pos;
    s->camera.zoom = GV_CAM_ZOOM_BASE;
    s->zoom_current = GV_CAM_ZOOM_BASE;
    s->camera.shake_x = s->camera.shake_y = 0.0f;
    s->flash = 0.0f;
    gv_camera_clamp(&s->camera, w, (float)GV_VIEW_W, (float)GV_VIEW_H);
}

void gv_scene_update(gv_scene *s, const gv_world *w, float dt)
{
    if (!s || !w || !(dt > 0.0f)) return;

    const gv_player *p = &w->player;

    /* Lead the ship in the direction of travel. Using velocity rather than
     * aim keeps the framing stable while the player sweeps the cursor around. */
    gv_v2 lead = gv_v2_clamp_len(gv_v2_mul(p->vel, 0.22f), GV_CAM_LEAD);
    s->cam_target = gv_v2_add(p->pos, lead);

    s->camera.pos = gv_v2_approach_exp(s->camera.pos, s->cam_target, GV_CAM_SMOOTH, dt);

    /* Pull back as speed rises so there is time to read what is ahead. */
    float target_zoom = gv_lerpf(GV_CAM_ZOOM_BASE, GV_CAM_ZOOM_MIN,
                                 gv_clampf(p->speed / GV_PLAYER_MAX_SPEED, 0.0f, 1.0f));
    s->zoom_current = gv_approach_exp(s->zoom_current, target_zoom, 2.2f, dt);
    s->camera.zoom = s->zoom_current;

    /* Trauma-squared shake, so small hits are subtle and big ones are not. */
    s->shake_time += dt;
    if (s->opt.screen_shake) {
        float trauma = w->shake * w->shake;
        float amp = trauma * 26.0f;
        float t = s->shake_time * 34.0f;
        s->camera.shake_x = sinf(t * 1.7f) * amp + sinf(t * 0.9f + 1.3f) * amp * 0.5f;
        s->camera.shake_y = cosf(t * 1.3f) * amp + cosf(t * 2.1f + 0.7f) * amp * 0.5f;
    } else {
        s->camera.shake_x = s->camera.shake_y = 0.0f;
    }

    s->flash = gv_maxf(0.0f, s->flash - dt * 3.4f);
    gv_camera_clamp(&s->camera, w, (float)GV_VIEW_W, (float)GV_VIEW_H);
}

gv_v2 gv_scene_screen_to_world(const gv_scene *s, gv_gfx *g, gv_v2 view_point)
{
    if (!s || !g) return gv_v2_zero();
    return gv_gfx_view_to_world(g, view_point);
}

/* ---------------------------------------------------------------- background */

static void gv_draw_starfield(gv_scene *s, gv_gfx *g)
{
    const gv_camera *cam = gv_gfx_camera(g);
    /* Nearer layers move more; the furthest barely moves at all. */
    static const float parallax[GV_STAR_LAYERS] = { 0.25f, 0.5f, 0.8f };

    for (int i = 0; i < GV_STAR_COUNT; ++i) {
        const gv_star *st = &s->stars[i];
        float k = parallax[st->layer];

        gv_v2 view = gv_v2_make(
            (st->pos.x - cam->pos.x) * k * cam->zoom + (float)GV_VIEW_W * 0.5f + cam->shake_x * k,
            (st->pos.y - cam->pos.y) * k * cam->zoom + (float)GV_VIEW_H * 0.5f + cam->shake_y * k);

        if (view.x < -10.0f || view.x > (float)GV_VIEW_W + 10.0f || view.y < -10.0f ||
            view.y > (float)GV_VIEW_H + 10.0f) {
            continue;
        }

        float b = st->bright * (0.35f + 0.65f * k);
        if (s->opt.high_contrast) b *= 0.45f;
        gv_color c = gv_rgba(180, 210, 255, (uint8_t)(b * 190.0f));
        gv_draw_glow(g, view, st->size * (1.2f + k), c);
    }
}

static void gv_draw_arena(gv_scene *s, gv_gfx *g, const gv_world *w)
{
    /* A sparse grid gives the empty space a sense of scale and speed without
     * competing with the entities for attention. */
    const float step = 320.0f;
    gv_color grid = k_col_grid;
    if (s->opt.high_contrast) grid = gv_fade(grid, 0.4f);

    for (float x = 0.0f; x <= w->arena_w + 1.0f; x += step) {
        gv_v2 a = gv_gfx_world_to_view(g, gv_v2_make(x, 0.0f));
        gv_v2 b = gv_gfx_world_to_view(g, gv_v2_make(x, w->arena_h));
        if (a.x < -20.0f || a.x > (float)GV_VIEW_W + 20.0f) continue;
        gv_draw_line(g, a, b, 1.0f, grid, 0.0f);
    }
    for (float y = 0.0f; y <= w->arena_h + 1.0f; y += step) {
        gv_v2 a = gv_gfx_world_to_view(g, gv_v2_make(0.0f, y));
        gv_v2 b = gv_gfx_world_to_view(g, gv_v2_make(w->arena_w, y));
        if (a.y < -20.0f || a.y > (float)GV_VIEW_H + 20.0f) continue;
        gv_draw_line(g, a, b, 1.0f, grid, 0.0f);
    }

    /* The boundary brightens as the ship approaches, which reads as a warning
     * without needing a separate UI element. */
    const gv_player *p = &w->player;
    float near_edge = gv_minf(gv_minf(p->pos.x, w->arena_w - p->pos.x),
                              gv_minf(p->pos.y, w->arena_h - p->pos.y));
    float proximity = 1.0f - gv_clampf(near_edge / 420.0f, 0.0f, 1.0f);
    gv_color wall = gv_fade(k_col_wall, 0.45f + 0.55f * proximity);

    gv_v2 corners[4] = {
        gv_gfx_world_to_view(g, gv_v2_make(0.0f, 0.0f)),
        gv_gfx_world_to_view(g, gv_v2_make(w->arena_w, 0.0f)),
        gv_gfx_world_to_view(g, gv_v2_make(w->arena_w, w->arena_h)),
        gv_gfx_world_to_view(g, gv_v2_make(0.0f, w->arena_h)),
    };
    gv_draw_poly(g, corners, 4, 2.6f, wall, 1.0f + proximity);
}

/* ------------------------------------------------------------------ entities */

static void gv_draw_pylons(gv_gfx *g, const gv_world *w)
{
    for (int i = 0; i < w->pylon_count; ++i) {
        const gv_pylon *py = &w->pylons[i];
        if (!py->alive || !gv_gfx_visible(g, py->pos, py->radius + 40.0f)) continue;

        float pulse = 0.5f + 0.5f * sinf(py->pulse);
        gv_color c = k_col_pylon;

        /* Anchors within rope reach light up. This is the affordance that
         * turns the tether from "aim and hope" into a read of the arena, and
         * it keeps out-of-range pylons from competing for attention. */
        float reach = gv_v2_dist(py->pos, w->player.pos);
        float in_range = 1.0f - gv_smoothstep(GV_TETHER_MAX_LEN * 0.82f, GV_TETHER_MAX_LEN,
                                              reach);
        float presence = 0.40f + 0.60f * in_range;
        if (py->tethered) {
            c = k_col_tether;
            presence = 1.0f;
        }

        gv_v2 centre = gv_gfx_world_to_view(g, py->pos);
        float r = py->radius * gv_gfx_camera(g)->zoom;

        gv_draw_glow(g, centre, r * (2.4f + pulse * 0.5f),
                     gv_fade(c, (0.22f + pulse * 0.12f) * presence));

        /* A ring with radial ticks — deliberately NOT a polygon.
         *
         * Every enemy archetype is a polygon (triangle, arrow, hexagon,
         * square, octagon), so anything the player must react to has straight
         * edges and anything they can grab onto is round. An earlier hexagon
         * here was almost indistinguishable from a sentinel at a glance, which
         * is a bad thing to get wrong in a game about ramming the right
         * targets at speed. */
        float glow = in_range * 0.8f + 0.3f;
        gv_draw_circle(g, centre, r, 20, 2.0f, gv_fade(c, presence), glow);
        for (int k = 0; k < 4; ++k) {
            float a = (float)k / 4.0f * GV_TAU + py->pulse * 0.25f;
            gv_draw_line(g, gv_v2_add(centre, gv_v2_polar(a, r * 1.05f)),
                         gv_v2_add(centre, gv_v2_polar(a, r * 1.5f)), 1.8f,
                         gv_fade(c, presence * 0.9f), glow);
        }
        gv_draw_glow(g, centre, r * 0.5f, gv_fade(c, 0.85f * presence));
    }
}

static void gv_draw_rocks(gv_gfx *g, const gv_world *w, float lag)
{
    for (int i = 0; i < GV_MAX_ROCKS; ++i) {
        const gv_rock *r = &w->rocks[i];
        if (!r->alive) continue;

        gv_v2 pos = gv_v2_madd(r->pos, r->vel, lag);
        if (!gv_gfx_visible(g, pos, r->radius + 30.0f)) continue;

        gv_v2 centre = gv_gfx_world_to_view(g, pos);
        float rad = r->radius * gv_gfx_camera(g)->zoom;

        /* A fast rock is a weapon; make that unmistakable. */
        float speed = gv_v2_len(r->vel);
        float hot = gv_clampf((speed - 260.0f) / 500.0f, 0.0f, 1.0f);
        gv_color c = gv_color_lerp(k_col_rock, gv_rgb(255, 150, 90), hot);
        if (r->tethered) c = gv_color_lerp(c, k_col_tether, 0.5f);
        c = gv_color_lerp(c, gv_rgb(255, 255, 255), r->hit_flash * 0.8f);

        gv_v2 pts[GV_ROCK_VERTS];
        for (int v = 0; v < GV_ROCK_VERTS; ++v) {
            float a = (float)v / (float)GV_ROCK_VERTS * GV_TAU + r->angle;
            pts[v] = gv_v2_add(centre, gv_v2_polar(a, rad * r->verts[v]));
        }

        if (hot > 0.05f) {
            gv_draw_glow(g, centre, rad * 2.6f, gv_fade(gv_rgb(255, 150, 90), 0.30f * hot));
        }
        /* Rocks are anchors and weapons, not scenery, so they get a real
         * outline. The fill only has to occlude the starfield enough to read
         * as solid mass; earlier it was so dark the shape disappeared. */
        gv_draw_poly_filled(g, pts, GV_ROCK_VERTS, gv_rgba(20, 30, 46, 255));
        gv_draw_poly(g, pts, GV_ROCK_VERTS, 2.4f, c, 1.0f + hot * 0.6f);

        /* A short interior facet line gives the silhouette some volume. */
        gv_draw_line(g, pts[0], pts[GV_ROCK_VERTS / 2], 1.2f, gv_fade(c, 0.30f), 0.0f);
    }
}

static void gv_draw_enemy_shape(gv_gfx *g, gv_v2 centre, float radius, float angle, int kind,
                                gv_color c, float glow)
{
    gv_v2 pts[8];
    switch (kind) {
    case GV_ENEMY_DRONE:
        for (int i = 0; i < 3; ++i) {
            pts[i] = gv_v2_add(centre, gv_v2_polar(angle + (float)i / 3.0f * GV_TAU, radius));
        }
        gv_draw_poly(g, pts, 3, 2.2f, c, glow);
        break;

    case GV_ENEMY_LANCER: {
        /* An arrowhead: the silhouette itself tells you which way it will go. */
        pts[0] = gv_v2_add(centre, gv_v2_polar(angle, radius * 1.7f));
        pts[1] = gv_v2_add(centre, gv_v2_polar(angle + 2.4f, radius));
        pts[2] = gv_v2_add(centre, gv_v2_polar(angle + GV_PI, radius * 0.42f));
        pts[3] = gv_v2_add(centre, gv_v2_polar(angle - 2.4f, radius));
        gv_draw_poly(g, pts, 4, 2.2f, c, glow);
        break;
    }

    case GV_ENEMY_SENTINEL:
        for (int i = 0; i < 6; ++i) {
            pts[i] = gv_v2_add(centre, gv_v2_polar(angle + (float)i / 6.0f * GV_TAU, radius));
        }
        gv_draw_poly(g, pts, 6, 2.0f, c, glow);
        gv_draw_circle(g, centre, radius * 0.45f, 8, 1.6f, c, glow * 0.8f);
        break;

    case GV_ENEMY_SPLITTER:
        for (int i = 0; i < 4; ++i) {
            pts[i] = gv_v2_add(centre, gv_v2_polar(angle + (float)i / 4.0f * GV_TAU, radius));
        }
        gv_draw_poly(g, pts, 4, 2.2f, c, glow);
        /* An inner diamond hints that there is something inside to break out. */
        for (int i = 0; i < 4; ++i) {
            pts[i] = gv_v2_add(centre,
                               gv_v2_polar(angle + (float)i / 4.0f * GV_TAU + 0.78f, radius * 0.5f));
        }
        gv_draw_poly(g, pts, 4, 1.6f, c, glow * 0.7f);
        break;

    case GV_ENEMY_WARDEN:
    default:
        for (int i = 0; i < 8; ++i) {
            pts[i] = gv_v2_add(centre, gv_v2_polar(angle + (float)i / 8.0f * GV_TAU, radius));
        }
        gv_draw_poly(g, pts, 8, 3.0f, c, glow);
        for (int i = 0; i < 4; ++i) {
            pts[i] = gv_v2_add(centre,
                               gv_v2_polar(-angle * 1.6f + (float)i / 4.0f * GV_TAU, radius * 0.5f));
        }
        gv_draw_poly(g, pts, 4, 2.2f, c, glow);
        break;
    }
}

static void gv_draw_enemies(gv_gfx *g, const gv_world *w, float lag)
{
    const gv_player *p = &w->player;

    for (int i = 0; i < GV_MAX_ENEMIES; ++i) {
        const gv_enemy *e = &w->enemies[i];
        if (!e->alive) continue;

        gv_v2 pos = gv_v2_madd(e->pos, e->vel, lag);
        if (!gv_gfx_visible(g, pos, e->radius + 60.0f)) continue;

        gv_v2 centre = gv_gfx_world_to_view(g, pos);
        float zoom = gv_gfx_camera(g)->zoom;
        float radius = e->radius * zoom;
        gv_color c = gv_enemy_colour(e->kind);

        /* Materialising: fade and scale in, with a converging ring, so the
         * player can see something is arriving before it can hurt them. */
        if (e->spawn_anim < 1.0f) {
            float t = e->spawn_anim;
            float ring = radius * (3.4f - 2.4f * t);
            gv_draw_circle(g, centre, ring, 16, 1.6f, gv_fade(c, 0.35f + 0.4f * t), 0.8f);
            gv_draw_enemy_shape(g, centre, radius * (0.35f + 0.65f * t), e->angle, e->kind,
                                gv_fade(c, 0.25f + 0.6f * t), 0.6f);
            continue;
        }

        c = gv_color_lerp(c, gv_rgb(255, 255, 255), e->hit_flash * 0.85f);

        /* Armour ring: the single most important readability element in the
         * game. It turns solid when the ship is currently fast enough to break
         * this target, so the overdrive rule never has to be guessed at. */
        if (e->armour > 0.0f) {
            float needed = GV_OVERDRIVE_SPEED + e->armour;
            bool can_break = p->alive && p->speed >= needed;
            gv_color ring = can_break ? gv_rgb(255, 240, 160) : gv_rgb(150, 160, 190);
            float ring_r = radius * 1.55f;
            if (can_break) {
                gv_draw_circle(g, centre, ring_r, 20, 2.2f, ring, 1.2f);
                gv_draw_glow(g, centre, ring_r * 1.5f, gv_fade(ring, 0.2f));
            } else {
                /* Dashed while out of reach: visibly incomplete. */
                for (int k = 0; k < 8; ++k) {
                    float a0 = (float)k / 8.0f * GV_TAU;
                    gv_draw_arc(g, centre, ring_r, a0, a0 + 0.28f, 3, 1.8f, gv_fade(ring, 0.75f),
                                0.5f);
                }
            }
        }

        /* Lancer wind-up: a sight line down the committed path. */
        if (e->kind == GV_ENEMY_LANCER && e->ai == GV_EAI_WINDUP) {
            float charge = 1.0f - gv_clampf(e->timer / 0.82f, 0.0f, 1.0f);
            gv_v2 tip = gv_v2_add(centre, gv_v2_polar(e->angle, 900.0f * zoom));
            gv_draw_line(g, centre, tip, 1.2f + 2.0f * charge,
                         gv_fade(gv_rgb(255, 170, 70), 0.18f + 0.42f * charge), 0.6f);
            gv_draw_circle(g, centre, radius * (2.2f - charge * 0.9f), 14, 1.8f,
                           gv_fade(gv_rgb(255, 200, 120), 0.4f + 0.5f * charge), 1.0f);
        }

        float glow_amount = (e->kind == GV_ENEMY_WARDEN) ? 1.4f : 1.0f;
        gv_draw_glow(g, centre, radius * 2.2f, gv_fade(c, 0.18f + e->hit_flash * 0.4f));
        gv_draw_enemy_shape(g, centre, radius, e->angle, e->kind, c, glow_amount);

        /* Warden health pips, so a three-hit fight has visible progress. */
        if (e->kind == GV_ENEMY_WARDEN && e->max_hp > 1.0f) {
            int pips = (int)e->max_hp;
            for (int k = 0; k < pips; ++k) {
                float a = -GV_PI * 0.5f + ((float)k - (float)(pips - 1) * 0.5f) * 0.32f;
                gv_v2 at = gv_v2_add(centre, gv_v2_polar(a, radius * 2.1f));
                bool filled = ((float)k < e->hp);
                gv_draw_glow(g, at, filled ? 4.5f : 2.5f,
                             filled ? c : gv_fade(gv_rgb(120, 120, 140), 0.7f));
            }
        }
    }
}

static void gv_draw_bullets(gv_gfx *g, const gv_world *w, float lag)
{
    for (int i = 0; i < GV_MAX_BULLETS; ++i) {
        const gv_bullet *b = &w->bullets[i];
        if (!b->alive) continue;

        gv_v2 pos = gv_v2_madd(b->pos, b->vel, lag);
        if (!gv_gfx_visible(g, pos, 40.0f)) continue;

        gv_v2 centre = gv_gfx_world_to_view(g, pos);
        /* A short tail along the direction of travel makes the shot's heading
         * readable at a glance, which matters more than the dot itself. */
        gv_v2 tail = gv_gfx_world_to_view(g, gv_v2_madd(pos, b->vel, -0.045f));
        float r = b->radius * gv_gfx_camera(g)->zoom;

        gv_draw_line(g, tail, centre, r * 0.9f, gv_fade(k_col_bullet, 0.6f), 1.0f);
        gv_draw_glow(g, centre, r * 3.2f, gv_fade(k_col_bullet, 0.5f));
        gv_draw_glow(g, centre, r * 1.1f, gv_rgba(255, 255, 240, 255));
    }
}

static void gv_draw_pickups(gv_gfx *g, const gv_world *w, float lag)
{
    for (int i = 0; i < GV_MAX_PICKUPS; ++i) {
        const gv_pickup *k = &w->pickups[i];
        if (!k->alive) continue;

        gv_v2 pos = gv_v2_madd(k->pos, k->vel, lag);
        if (!gv_gfx_visible(g, pos, 50.0f)) continue;

        gv_color c;
        switch (k->kind) {
        case GV_PICKUP_LIFE: c = gv_rgb(255, 120, 180); break;
        case GV_PICKUP_PULSE: c = gv_rgb(120, 240, 255); break;
        default: c = gv_rgb(130, 255, 200); break;
        }

        /* Blink out as the pickup nears expiry, so nobody is surprised. */
        float alpha = (k->life < 2.5f) ? (0.35f + 0.65f * (0.5f + 0.5f * sinf(k->life * 18.0f)))
                                       : 1.0f;
        gv_v2 centre = gv_gfx_world_to_view(g, pos);
        float zoom = gv_gfx_camera(g)->zoom;
        float r = (10.0f + 2.0f * sinf(k->bob)) * zoom;

        gv_draw_glow(g, centre, r * 3.4f, gv_fade(c, 0.28f * alpha));

        gv_v2 pts[4];
        for (int v = 0; v < 4; ++v) {
            pts[v] = gv_v2_add(centre, gv_v2_polar(k->bob * 0.7f + (float)v / 4.0f * GV_TAU, r));
        }
        gv_draw_poly(g, pts, 4, 2.0f, gv_fade(c, alpha), 1.0f);

        if (k->kind == GV_PICKUP_LIFE) {
            gv_draw_line(g, gv_v2_sub(centre, gv_v2_make(r * 0.4f, 0.0f)),
                         gv_v2_add(centre, gv_v2_make(r * 0.4f, 0.0f)), 2.0f, gv_fade(c, alpha),
                         1.0f);
            gv_draw_line(g, gv_v2_sub(centre, gv_v2_make(0.0f, r * 0.4f)),
                         gv_v2_add(centre, gv_v2_make(0.0f, r * 0.4f)), 2.0f, gv_fade(c, alpha),
                         1.0f);
        }
    }
}

static void gv_draw_particles(gv_scene *s, gv_gfx *g, const gv_world *w)
{
    (void)s;
    for (int i = 0; i < GV_MAX_PARTICLES; ++i) {
        const gv_particle *p = &w->particles[i];
        if (!p->alive) continue;
        if (!gv_gfx_visible(g, p->pos, p->size + 12.0f)) continue;

        float t = (p->max_life > 0.0f) ? p->life / p->max_life : 0.0f;
        gv_color c = gv_rgba(p->r, p->g, p->b, (uint8_t)gv_clampf(t * 255.0f, 0.0f, 255.0f));
        gv_v2 centre = gv_gfx_world_to_view(g, p->pos);
        float zoom = gv_gfx_camera(g)->zoom;

        switch (p->kind) {
        case GV_PART_SHARD: {
            /* Shards are drawn as short streaks along their heading, which
             * reads far better in motion than a round dot. */
            gv_v2 tail = gv_gfx_world_to_view(g, gv_v2_madd(p->pos, p->vel, -0.03f));
            gv_draw_line(g, tail, centre, p->size * 0.5f * zoom, c, 0.8f);
            break;
        }
        case GV_PART_SMOKE:
            gv_draw_glow(g, centre, p->size * zoom * (2.0f - t), gv_fade(c, 0.28f * t));
            break;
        case GV_PART_TRAIL:
            gv_draw_glow(g, centre, p->size * zoom * (0.6f + t), gv_fade(c, 0.55f * t));
            break;
        case GV_PART_SPARK:
        default:
            gv_draw_glow(g, centre, p->size * zoom * (0.8f + t * 0.9f), c);
            break;
        }
    }
}

static void gv_draw_shockwaves(gv_gfx *g, const gv_world *w)
{
    for (int i = 0; i < GV_MAX_SHOCKWAVES; ++i) {
        const gv_shockwave *s = &w->shockwaves[i];
        if (!s->alive) continue;
        if (!gv_gfx_visible(g, s->pos, s->radius + 20.0f)) continue;

        float t = (s->max_life > 0.0f) ? s->life / s->max_life : 0.0f;
        gv_color c = gv_rgba(s->r, s->g, s->b, (uint8_t)gv_clampf(t * 210.0f, 0.0f, 255.0f));
        gv_draw_circle_world(g, s->pos, s->radius, 28, s->thickness * t, c, 1.0f);
    }
}

static void gv_draw_floaters(gv_gfx *g, const gv_world *w)
{
    for (int i = 0; i < GV_MAX_FLOATERS; ++i) {
        const gv_floater *f = &w->floaters[i];
        if (!f->alive) continue;
        if (!gv_gfx_visible(g, f->pos, 90.0f)) continue;

        float t = (f->max_life > 0.0f) ? f->life / f->max_life : 0.0f;
        gv_color c = gv_rgba(f->r, f->g, f->b, (uint8_t)gv_clampf(t * 255.0f, 0.0f, 255.0f));
        gv_v2 at = gv_gfx_world_to_view(g, f->pos);
        char buf[32];
        gv_format_number(buf, sizeof(buf), f->value);
        gv_draw_text_centred(g, at.x, at.y, 15.0f * f->scale * gv_gfx_camera(g)->zoom, c, 0.9f,
                             buf);
    }
}

/* -------------------------------------------------------------------- tether */

static void gv_draw_tether(gv_gfx *g, const gv_world *w)
{
    const gv_player *p = &w->player;
    const gv_tether *t = &p->tether;
    if (t->state == GV_TETHER_IDLE) return;

    gv_v2 from = gv_gfx_world_to_view(g, p->pos);
    gv_v2 to = gv_gfx_world_to_view(g, t->tip);

    gv_color c = gv_color_lerp(k_col_tether, k_col_tether_taut, t->taut);
    float width = 1.6f + t->taut * 1.8f;

    if (t->state == GV_TETHER_ATTACHED) {
        /* A taut rope bows less and vibrates more. The wobble is purely
         * cosmetic but sells the tension the constraint is under. */
        gv_v2 delta = gv_v2_sub(to, from);
        float len = gv_v2_len(delta);
        int segments = gv_clampi((int)(len / 22.0f), 2, 22);
        gv_v2 perp = gv_v2_perp(gv_v2_norm_or(delta, gv_v2_make(1.0f, 0.0f)));
        float slack = (1.0f - t->taut) * gv_minf(len * 0.06f, 16.0f);
        float buzz = t->taut * 2.2f;

        gv_v2 prev = from;
        for (int i = 1; i <= segments; ++i) {
            float u = (float)i / (float)segments;
            gv_v2 base = gv_v2_lerp(from, to, u);
            float sag = sinf(u * GV_PI) * slack;
            sag += sinf(u * 18.0f + t->attach_time * 40.0f) * buzz;
            gv_v2 pt = gv_v2_madd(base, perp, sag);
            gv_draw_line(g, prev, pt, width, c, 1.0f);
            prev = pt;
        }
        gv_draw_glow(g, to, 11.0f + t->taut * 7.0f, gv_fade(c, 0.85f));
        gv_draw_circle(g, to, 9.0f, 10, 1.8f, c, 1.0f);
    } else {
        gv_draw_line(g, from, to, width, gv_fade(c, 0.8f), 1.0f);
        gv_draw_glow(g, to, 7.0f, gv_fade(c, 0.9f));
    }
}

/* -------------------------------------------------------------------- player */

static void gv_draw_player(gv_gfx *g, const gv_world *w, float lag)
{
    const gv_player *p = &w->player;
    if (!p->alive) return;

    gv_v2 pos = gv_v2_madd(p->pos, p->vel, lag);
    gv_v2 centre = gv_gfx_world_to_view(g, pos);
    float zoom = gv_gfx_camera(g)->zoom;
    float r = p->radius * zoom;

    /* Invulnerability blinks the ship, but never all the way out: losing sight
     * of yourself at speed is worse than the hit that caused it. */
    float alpha = 1.0f;
    if (p->invuln > 0.0f) alpha = 0.45f + 0.55f * (0.5f + 0.5f * sinf(p->invuln * 34.0f));

    gv_color c = gv_color_lerp(k_col_player, k_col_overdrive, p->overdrive);
    c = gv_color_lerp(c, gv_rgb(255, 255, 255), p->hit_flash * 0.7f);

    /* Overdrive halo: the visual contract for "you are currently a weapon". */
    if (p->overdrive > 0.0f) {
        gv_draw_glow(g, centre, r * (4.0f + 5.0f * p->overdrive),
                     gv_fade(k_col_overdrive, 0.30f * p->overdrive * alpha));
        gv_draw_circle(g, centre, r * (1.9f + 0.5f * p->overdrive), 18, 1.8f,
                       gv_fade(k_col_overdrive, (0.35f + 0.5f * p->overdrive) * alpha), 1.2f);
    }
    gv_draw_glow(g, centre, r * 3.0f, gv_fade(c, 0.30f * alpha));

    /* Engine plume, opposite the thrust and scaled by how hard it is running. */
    if (p->thrust_amount > 0.02f) {
        gv_v2 back = gv_v2_neg(gv_v2_norm_or(p->vel, gv_v2_from_angle(p->angle)));
        float plume = r * (1.4f + 2.6f * p->thrust_amount);
        gv_v2 tip = gv_v2_madd(centre, back, plume);
        gv_draw_line(g, centre, tip, r * 0.7f,
                     gv_fade(gv_rgb(150, 220, 255), 0.55f * p->thrust_amount * alpha), 1.0f);
    }

    /* The hull: a forward-swept dart, drawn nose-first so the aim direction is
     * unmistakable even in a crowd. */
    gv_v2 hull[4];
    hull[0] = gv_v2_add(centre, gv_v2_polar(p->angle, r * 1.9f));
    hull[1] = gv_v2_add(centre, gv_v2_polar(p->angle + 2.55f, r * 1.25f));
    hull[2] = gv_v2_add(centre, gv_v2_polar(p->angle + GV_PI, r * 0.55f));
    hull[3] = gv_v2_add(centre, gv_v2_polar(p->angle - 2.55f, r * 1.25f));

    gv_draw_poly_filled(g, hull, 4, gv_rgba(10, 18, 30, (uint8_t)(220.0f * alpha)));
    gv_draw_poly(g, hull, 4, 2.4f, gv_fade(c, alpha), 1.2f);
    gv_draw_glow(g, centre, r * 0.7f, gv_fade(gv_rgb(255, 255, 255), 0.9f * alpha));
}

/* -------------------------------------------------------- off-screen markers */

/* Enemies outside the view get an arrow pinned to the edge. In an arena twice
 * the size of the screen this is the difference between "tense" and "unfair". */
static void gv_draw_offscreen_markers(gv_gfx *g, const gv_world *w)
{
    const float margin = 34.0f;
    const float vw = (float)GV_VIEW_W, vh = (float)GV_VIEW_H;

    for (int i = 0; i < GV_MAX_ENEMIES; ++i) {
        const gv_enemy *e = &w->enemies[i];
        if (!e->alive || e->spawn_anim < 1.0f) continue;
        if (gv_gfx_visible(g, e->pos, e->radius)) continue;

        gv_v2 v = gv_gfx_world_to_view(g, e->pos);
        gv_v2 edge = gv_v2_make(gv_clampf(v.x, margin, vw - margin),
                                gv_clampf(v.y, margin, vh - margin));

        gv_v2 dir = gv_v2_norm_or(gv_v2_sub(v, gv_v2_make(vw * 0.5f, vh * 0.5f)),
                                  gv_v2_make(1.0f, 0.0f));
        float dist = gv_v2_dist(e->pos, w->player.pos);
        /* Fade with distance so the edge does not become a wall of arrows. */
        float alpha = gv_clampf(gv_remap(dist, 2200.0f, 700.0f, 0.15f, 0.85f), 0.1f, 0.85f);
        gv_color c = gv_fade(gv_enemy_colour(e->kind), alpha);

        gv_v2 tri[3];
        float a = gv_v2_angle(dir);
        tri[0] = gv_v2_add(edge, gv_v2_polar(a, 11.0f));
        tri[1] = gv_v2_add(edge, gv_v2_polar(a + 2.5f, 8.0f));
        tri[2] = gv_v2_add(edge, gv_v2_polar(a - 2.5f, 8.0f));
        gv_draw_poly(g, tri, 3, 1.8f, c, 0.9f);
    }
}

/* ---------------------------------------------------------------------- draw */

void gv_scene_draw(gv_scene *s, gv_gfx *g, const gv_world *w, float lag)
{
    if (!s || !g || !w) return;
    if (!isfinite(lag) || lag < 0.0f) lag = 0.0f;
    lag = gv_minf(lag, GV_FIXED_DT);

    gv_gfx_set_camera(g, &s->camera);

    gv_draw_starfield(s, g);
    gv_draw_arena(s, g, w);
    gv_draw_pylons(g, w);
    gv_draw_rocks(g, w, lag);
    gv_draw_pickups(g, w, lag);
    gv_draw_particles(s, g, w);
    gv_draw_shockwaves(g, w);
    gv_draw_bullets(g, w, lag);
    gv_draw_enemies(g, w, lag);
    gv_draw_tether(g, w);
    gv_draw_player(g, w, lag);
    gv_draw_floaters(g, w);

    if (s->opt.show_offscreen_markers) gv_draw_offscreen_markers(g, w);
}
