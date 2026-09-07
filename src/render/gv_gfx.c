#include "gv_gfx.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The atlas packs both procedural brushes into one texture so that every
 * shape in the game — lines, glows, particles, glyphs — is drawn from the same
 * texture with the same blend mode, and SDL can batch the entire frame into a
 * handful of draw calls. */
#define GV_ATLAS_W 256
#define GV_ATLAS_H 128
#define GV_GLOW_SIZE 128
#define GV_LINE_X0 130

/* UV windows into the atlas. The two-pixel gutter between the brushes keeps
 * bilinear filtering from bleeding one into the other. */
#define GV_UV_GLOW_U0 (0.0f / (float)GV_ATLAS_W)
#define GV_UV_GLOW_U1 (127.0f / (float)GV_ATLAS_W)
#define GV_UV_LINE_U (192.0f / (float)GV_ATLAS_W)

/* Neon is built from three concentric passes. */
#define GV_HALO_SCALE 3.4f
#define GV_HALO_ALPHA 0.26f
#define GV_BODY_SCALE 1.55f
#define GV_BODY_ALPHA 0.70f
#define GV_CORE_SCALE 0.55f

#define GV_MAX_POLY 64

struct gv_gfx {
    SDL_Renderer *renderer;
    int view_w, view_h;

    SDL_Texture *atlas;
    SDL_Texture *rt_scene;
    SDL_Texture *rt_bloom_a;
    SDL_Texture *rt_bloom_b;

    bool has_targets;
    bool bloom_enabled;
    float bloom_strength;

    gv_camera camera;
};

static char g_gfx_error[256];

static void gv_gfx_set_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_gfx_error, sizeof(g_gfx_error), fmt, ap);
    va_end(ap);
}

const char *gv_gfx_error(void)
{
    return g_gfx_error;
}

/* ------------------------------------------------------------- the atlas */

static SDL_Texture *gv_build_atlas(SDL_Renderer *renderer)
{
    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, GV_ATLAS_W, GV_ATLAS_H, 32,
                                                       SDL_PIXELFORMAT_ARGB8888);
    if (!surf) {
        gv_gfx_set_error("atlas surface: %s", SDL_GetError());
        return NULL;
    }

    SDL_LockSurface(surf);
    uint32_t *pixels = (uint32_t *)surf->pixels;
    int pitch = surf->pitch / 4;
    memset(surf->pixels, 0, (size_t)surf->h * (size_t)surf->pitch);

    /* Left half: a radial falloff used for point lights, particle sprites and
     * the rounded caps on every line. The exponent controls how tight the core
     * reads against the halo. */
    const float centre = (float)GV_GLOW_SIZE * 0.5f;
    for (int y = 0; y < GV_GLOW_SIZE; ++y) {
        for (int x = 0; x < GV_GLOW_SIZE; ++x) {
            float dx = ((float)x + 0.5f - centre) / centre;
            float dy = ((float)y + 0.5f - centre) / centre;
            float r = sqrtf(dx * dx + dy * dy);
            float a = (r >= 1.0f) ? 0.0f : powf(1.0f - r, 2.4f);
            /* A small hard core keeps thin elements from looking washed out. */
            a += (r < 0.10f) ? (0.10f - r) * 3.0f : 0.0f;
            uint32_t alpha = (uint32_t)gv_clampf(a * 255.0f, 0.0f, 255.0f);
            pixels[y * pitch + x] = (alpha << 24) | 0x00FFFFFFu;
        }
    }

    /* Right half: a one-dimensional gradient across the line's width, so a
     * stretched quad reads as a glowing stroke rather than a flat band. */
    for (int y = 0; y < GV_ATLAS_H; ++y) {
        float t = gv_absf(((float)y + 0.5f) - (float)GV_ATLAS_H * 0.5f) /
                  ((float)GV_ATLAS_H * 0.5f);
        float a = (t >= 1.0f) ? 0.0f : powf(1.0f - t, 2.2f);
        a += (t < 0.08f) ? (0.08f - t) * 3.5f : 0.0f;
        uint32_t alpha = (uint32_t)gv_clampf(a * 255.0f, 0.0f, 255.0f);
        uint32_t packed = (alpha << 24) | 0x00FFFFFFu;
        for (int x = GV_LINE_X0; x < GV_ATLAS_W; ++x) {
            pixels[y * pitch + x] = packed;
        }
    }
    SDL_UnlockSurface(surf);

    SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, surf);
    SDL_FreeSurface(surf);
    if (!tex) {
        gv_gfx_set_error("atlas texture: %s", SDL_GetError());
        return NULL;
    }
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_ADD);
    SDL_SetTextureScaleMode(tex, SDL_ScaleModeLinear);
    return tex;
}

/* ------------------------------------------------------------------ device */

gv_gfx *gv_gfx_create(SDL_Renderer *renderer, int view_w, int view_h)
{
    if (!renderer || view_w <= 0 || view_h <= 0) {
        gv_gfx_set_error("invalid arguments");
        return NULL;
    }

    gv_gfx *g = (gv_gfx *)calloc(1, sizeof(gv_gfx));
    if (!g) {
        gv_gfx_set_error("out of memory");
        return NULL;
    }

    g->renderer = renderer;
    g->view_w = view_w;
    g->view_h = view_h;
    g->bloom_strength = 0.85f;
    g->camera.zoom = 1.0f;
    g->camera.pos = gv_v2_make((float)view_w * 0.5f, (float)view_h * 0.5f);

    g->atlas = gv_build_atlas(renderer);
    if (!g->atlas) {
        free(g);
        return NULL;
    }

    /* Render targets are optional. Software and some remote drivers do not
     * provide them; the game then composes straight to the backbuffer and
     * simply goes without the bloom haze. */
    if (SDL_RenderTargetSupported(renderer) == SDL_TRUE) {
        g->rt_scene = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888,
                                        SDL_TEXTUREACCESS_TARGET, view_w, view_h);
        g->rt_bloom_a = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888,
                                          SDL_TEXTUREACCESS_TARGET, view_w / 4, view_h / 4);
        g->rt_bloom_b = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888,
                                          SDL_TEXTUREACCESS_TARGET, view_w / 8, view_h / 8);
        g->has_targets = (g->rt_scene && g->rt_bloom_a && g->rt_bloom_b);

        if (g->has_targets) {
            SDL_SetTextureScaleMode(g->rt_scene, SDL_ScaleModeLinear);
            SDL_SetTextureScaleMode(g->rt_bloom_a, SDL_ScaleModeLinear);
            SDL_SetTextureScaleMode(g->rt_bloom_b, SDL_ScaleModeLinear);
        } else {
            /* Partial success is worse than none: release the strays. */
            if (g->rt_scene) SDL_DestroyTexture(g->rt_scene);
            if (g->rt_bloom_a) SDL_DestroyTexture(g->rt_bloom_a);
            if (g->rt_bloom_b) SDL_DestroyTexture(g->rt_bloom_b);
            g->rt_scene = g->rt_bloom_a = g->rt_bloom_b = NULL;
        }
    }

    g->bloom_enabled = g->has_targets;
    return g;
}

void gv_gfx_destroy(gv_gfx *g)
{
    if (!g) return;
    if (g->atlas) SDL_DestroyTexture(g->atlas);
    if (g->rt_scene) SDL_DestroyTexture(g->rt_scene);
    if (g->rt_bloom_a) SDL_DestroyTexture(g->rt_bloom_a);
    if (g->rt_bloom_b) SDL_DestroyTexture(g->rt_bloom_b);
    free(g);
}

bool gv_gfx_has_bloom(const gv_gfx *g)
{
    return g && g->has_targets;
}

void gv_gfx_set_bloom(gv_gfx *g, bool enabled, float strength)
{
    if (!g) return;
    g->bloom_enabled = enabled && g->has_targets;
    g->bloom_strength = gv_clampf(strength, 0.0f, 1.5f);
}

int gv_gfx_view_w(const gv_gfx *g) { return g ? g->view_w : 0; }
int gv_gfx_view_h(const gv_gfx *g) { return g ? g->view_h : 0; }

void gv_gfx_begin_scene(gv_gfx *g)
{
    if (!g) return;
    if (g->has_targets) SDL_SetRenderTarget(g->renderer, g->rt_scene);
    SDL_SetRenderDrawBlendMode(g->renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(g->renderer, 0, 0, 0, 255);
    SDL_RenderClear(g->renderer);
}

void gv_gfx_end_scene(gv_gfx *g, const SDL_Rect *dest)
{
    if (!g) return;

    if (!g->has_targets) {
        /* Already drawn straight to the backbuffer. */
        return;
    }

    if (g->bloom_enabled && g->bloom_strength > 0.01f) {
        /* Bright pass without shaders: copy the scene, then multiply it by
         * itself. Squaring leaves the bright neon largely intact while
         * collapsing the dim background toward black, which is exactly the
         * separation a threshold would give. */
        SDL_SetRenderTarget(g->renderer, g->rt_bloom_a);
        SDL_SetRenderDrawColor(g->renderer, 0, 0, 0, 255);
        SDL_RenderClear(g->renderer);
        SDL_SetTextureBlendMode(g->rt_scene, SDL_BLENDMODE_NONE);
        SDL_SetTextureAlphaMod(g->rt_scene, 255);
        SDL_RenderCopy(g->renderer, g->rt_scene, NULL, NULL);
        SDL_SetTextureBlendMode(g->rt_scene, SDL_BLENDMODE_MOD);
        SDL_RenderCopy(g->renderer, g->rt_scene, NULL, NULL);

        /* A second, coarser downsample widens the halo. Linear filtering on
         * the way down is doing the blurring for us. */
        SDL_SetRenderTarget(g->renderer, g->rt_bloom_b);
        SDL_SetRenderDrawColor(g->renderer, 0, 0, 0, 255);
        SDL_RenderClear(g->renderer);
        SDL_SetTextureBlendMode(g->rt_bloom_a, SDL_BLENDMODE_NONE);
        SDL_SetTextureAlphaMod(g->rt_bloom_a, 255);
        SDL_RenderCopy(g->renderer, g->rt_bloom_a, NULL, NULL);

        /* Composite both levels back over the scene. */
        SDL_SetRenderTarget(g->renderer, g->rt_scene);
        uint8_t tight = (uint8_t)gv_clampf(g->bloom_strength * 190.0f, 0.0f, 255.0f);
        uint8_t wide = (uint8_t)gv_clampf(g->bloom_strength * 140.0f, 0.0f, 255.0f);
        SDL_SetTextureBlendMode(g->rt_bloom_a, SDL_BLENDMODE_ADD);
        SDL_SetTextureAlphaMod(g->rt_bloom_a, tight);
        SDL_RenderCopy(g->renderer, g->rt_bloom_a, NULL, NULL);
        SDL_SetTextureBlendMode(g->rt_bloom_b, SDL_BLENDMODE_ADD);
        SDL_SetTextureAlphaMod(g->rt_bloom_b, wide);
        SDL_RenderCopy(g->renderer, g->rt_bloom_b, NULL, NULL);
    }

    SDL_SetRenderTarget(g->renderer, NULL);
    SDL_SetTextureBlendMode(g->rt_scene, SDL_BLENDMODE_NONE);
    SDL_SetTextureAlphaMod(g->rt_scene, 255);
    SDL_RenderCopy(g->renderer, g->rt_scene, NULL, dest);
}

/* ------------------------------------------------------------------ camera */

void gv_gfx_set_camera(gv_gfx *g, const gv_camera *cam)
{
    if (!g || !cam) return;
    g->camera = *cam;
    if (!(g->camera.zoom > 0.01f)) g->camera.zoom = 1.0f;
}

const gv_camera *gv_gfx_camera(const gv_gfx *g)
{
    return g ? &g->camera : NULL;
}

gv_v2 gv_gfx_world_to_view(const gv_gfx *g, gv_v2 world)
{
    const gv_camera *c = &g->camera;
    float z = c->zoom;
    return gv_v2_make((world.x - c->pos.x) * z + (float)g->view_w * 0.5f + c->shake_x,
                      (world.y - c->pos.y) * z + (float)g->view_h * 0.5f + c->shake_y);
}

gv_v2 gv_gfx_view_to_world(const gv_gfx *g, gv_v2 view)
{
    const gv_camera *c = &g->camera;
    float z = (c->zoom > 0.01f) ? c->zoom : 1.0f;
    return gv_v2_make((view.x - (float)g->view_w * 0.5f - c->shake_x) / z + c->pos.x,
                      (view.y - (float)g->view_h * 0.5f - c->shake_y) / z + c->pos.y);
}

bool gv_gfx_visible(const gv_gfx *g, gv_v2 world, float radius)
{
    gv_v2 v = gv_gfx_world_to_view(g, world);
    float r = radius * g->camera.zoom + 8.0f;
    return v.x + r >= 0.0f && v.x - r <= (float)g->view_w && v.y + r >= 0.0f &&
           v.y - r <= (float)g->view_h;
}

/* -------------------------------------------------------------- geometry */

/* Emit one textured quad. All primitives funnel through here, which is what
 * keeps the whole frame batchable. */
static void gv_quad(gv_gfx *g, gv_v2 p0, gv_v2 p1, gv_v2 p2, gv_v2 p3,
                    float u0, float v0, float u1, float v1, gv_color c)
{
    if (c.a == 0) return;

    SDL_Vertex verts[4];
    SDL_Color col = { c.r, c.g, c.b, c.a };

    verts[0].position.x = p0.x; verts[0].position.y = p0.y;
    verts[1].position.x = p1.x; verts[1].position.y = p1.y;
    verts[2].position.x = p2.x; verts[2].position.y = p2.y;
    verts[3].position.x = p3.x; verts[3].position.y = p3.y;

    verts[0].tex_coord.x = u0; verts[0].tex_coord.y = v0;
    verts[1].tex_coord.x = u1; verts[1].tex_coord.y = v0;
    verts[2].tex_coord.x = u1; verts[2].tex_coord.y = v1;
    verts[3].tex_coord.x = u0; verts[3].tex_coord.y = v1;

    for (int i = 0; i < 4; ++i) verts[i].color = col;

    static const int indices[6] = { 0, 1, 2, 0, 2, 3 };
    SDL_RenderGeometry(g->renderer, g->atlas, verts, 4, indices, 6);
}

/* One stroke pass of a line: a quad along the segment plus round caps. */
static void gv_stroke(gv_gfx *g, gv_v2 a, gv_v2 b, float width, gv_color c)
{
    if (width <= 0.0f || c.a == 0) return;

    gv_v2 d = gv_v2_sub(b, a);
    float len = gv_v2_len(d);
    gv_v2 dir = (len > GV_EPS) ? gv_v2_mul(d, 1.0f / len) : gv_v2_make(1.0f, 0.0f);
    gv_v2 perp = gv_v2_mul(gv_v2_perp(dir), width * 0.5f);

    if (len > GV_EPS) {
        gv_quad(g, gv_v2_add(a, perp), gv_v2_add(b, perp), gv_v2_sub(b, perp), gv_v2_sub(a, perp),
                GV_UV_LINE_U, 0.0f, GV_UV_LINE_U, 1.0f, c);
    }

    /* Round caps, drawn from the radial brush so joints and endpoints do not
     * show the flat edge of the quad. */
    float r = width * 0.5f;
    gv_v2 ex = gv_v2_make(r, 0.0f), ey = gv_v2_make(0.0f, r);
    gv_quad(g, gv_v2_sub(gv_v2_sub(a, ex), ey), gv_v2_sub(gv_v2_add(a, ex), ey),
            gv_v2_add(gv_v2_add(a, ex), ey), gv_v2_add(gv_v2_sub(a, ex), ey),
            GV_UV_GLOW_U0, 0.0f, GV_UV_GLOW_U1, 1.0f, c);
    if (len > GV_EPS) {
        gv_quad(g, gv_v2_sub(gv_v2_sub(b, ex), ey), gv_v2_sub(gv_v2_add(b, ex), ey),
                gv_v2_add(gv_v2_add(b, ex), ey), gv_v2_add(gv_v2_sub(b, ex), ey),
                GV_UV_GLOW_U0, 0.0f, GV_UV_GLOW_U1, 1.0f, c);
    }
}

void gv_draw_line(gv_gfx *g, gv_v2 a, gv_v2 b, float width, gv_color c, float glow)
{
    if (!g || !gv_v2_finite(a) || !gv_v2_finite(b) || !isfinite(width)) return;
    width = gv_clampf(width, 0.4f, 400.0f);
    glow = gv_clampf(glow, 0.0f, 3.0f);

    if (glow > 0.0f) {
        gv_stroke(g, a, b, width * GV_HALO_SCALE, gv_fade(c, GV_HALO_ALPHA * glow));
        gv_stroke(g, a, b, width * GV_BODY_SCALE, gv_fade(c, GV_BODY_ALPHA));
    }

    /* The core is pushed toward white so that saturated neon still reads as a
     * light source rather than a flat coloured stripe. */
    gv_color core = gv_color_lerp(c, gv_rgba(255, 255, 255, c.a), 0.55f * gv_minf(glow, 1.0f));
    gv_stroke(g, a, b, width * (glow > 0.0f ? GV_CORE_SCALE : 1.0f), core);
}

void gv_draw_line_world(gv_gfx *g, gv_v2 a, gv_v2 b, float width, gv_color c, float glow)
{
    if (!g) return;
    gv_draw_line(g, gv_gfx_world_to_view(g, a), gv_gfx_world_to_view(g, b),
                 width * g->camera.zoom, c, glow);
}

void gv_draw_glow(gv_gfx *g, gv_v2 centre, float radius, gv_color c)
{
    if (!g || !gv_v2_finite(centre) || !isfinite(radius) || radius <= 0.0f) return;
    radius = gv_minf(radius, 4000.0f);
    gv_v2 ex = gv_v2_make(radius, 0.0f), ey = gv_v2_make(0.0f, radius);
    gv_quad(g, gv_v2_sub(gv_v2_sub(centre, ex), ey), gv_v2_sub(gv_v2_add(centre, ex), ey),
            gv_v2_add(gv_v2_add(centre, ex), ey), gv_v2_add(gv_v2_sub(centre, ex), ey),
            GV_UV_GLOW_U0, 0.0f, GV_UV_GLOW_U1, 1.0f, c);
}

void gv_draw_glow_world(gv_gfx *g, gv_v2 centre, float radius, gv_color c)
{
    if (!g) return;
    gv_draw_glow(g, gv_gfx_world_to_view(g, centre), radius * g->camera.zoom, c);
}

void gv_draw_poly(gv_gfx *g, const gv_v2 *pts, int count, float width, gv_color c, float glow)
{
    if (!g || !pts || count < 2) return;
    for (int i = 0; i < count; ++i) {
        gv_draw_line(g, pts[i], pts[(i + 1) % count], width, c, glow);
    }
}

void gv_draw_poly_filled(gv_gfx *g, const gv_v2 *pts, int count, gv_color c)
{
    if (!g || !pts || count < 3 || count > GV_MAX_POLY || c.a == 0) return;

    SDL_Vertex verts[GV_MAX_POLY];
    int indices[(GV_MAX_POLY - 2) * 3];
    SDL_Color col = { c.r, c.g, c.b, c.a };

    for (int i = 0; i < count; ++i) {
        if (!gv_v2_finite(pts[i])) return;
        verts[i].position.x = pts[i].x;
        verts[i].position.y = pts[i].y;
        /* Sample the brush's opaque centre: the fill wants flat colour. */
        verts[i].tex_coord.x = GV_UV_LINE_U;
        verts[i].tex_coord.y = 0.5f;
        verts[i].color = col;
    }

    int n = 0;
    for (int i = 1; i + 1 < count; ++i) {
        indices[n++] = 0;
        indices[n++] = i;
        indices[n++] = i + 1;
    }
    SDL_RenderGeometry(g->renderer, g->atlas, verts, count, indices, n);
}

void gv_draw_circle(gv_gfx *g, gv_v2 centre, float radius, int segments, float width,
                    gv_color c, float glow)
{
    if (!g || !gv_v2_finite(centre) || !isfinite(radius) || radius <= 0.0f) return;
    segments = gv_clampi(segments, 3, GV_MAX_POLY);

    gv_v2 prev = gv_v2_add(centre, gv_v2_make(radius, 0.0f));
    for (int i = 1; i <= segments; ++i) {
        float a = (float)i / (float)segments * GV_TAU;
        gv_v2 p = gv_v2_add(centre, gv_v2_polar(a, radius));
        gv_draw_line(g, prev, p, width, c, glow);
        prev = p;
    }
}

void gv_draw_circle_world(gv_gfx *g, gv_v2 centre, float radius, int segments, float width,
                          gv_color c, float glow)
{
    if (!g) return;
    gv_draw_circle(g, gv_gfx_world_to_view(g, centre), radius * g->camera.zoom, segments,
                   width * g->camera.zoom, c, glow);
}

void gv_draw_arc(gv_gfx *g, gv_v2 centre, float radius, float from, float to, int segments,
                 float width, gv_color c, float glow)
{
    if (!g || !gv_v2_finite(centre) || !isfinite(radius) || radius <= 0.0f) return;
    if (!isfinite(from) || !isfinite(to)) return;
    segments = gv_clampi(segments, 1, GV_MAX_POLY);

    gv_v2 prev = gv_v2_add(centre, gv_v2_polar(from, radius));
    for (int i = 1; i <= segments; ++i) {
        float t = (float)i / (float)segments;
        gv_v2 p = gv_v2_add(centre, gv_v2_polar(gv_lerpf(from, to, t), radius));
        gv_draw_line(g, prev, p, width, c, glow);
        prev = p;
    }
}

void gv_draw_rect(gv_gfx *g, float x, float y, float w, float h, float width, gv_color c,
                  float glow)
{
    gv_v2 pts[4] = {
        gv_v2_make(x, y), gv_v2_make(x + w, y), gv_v2_make(x + w, y + h), gv_v2_make(x, y + h)
    };
    gv_draw_poly(g, pts, 4, width, c, glow);
}

void gv_draw_rect_filled(gv_gfx *g, float x, float y, float w, float h, gv_color c)
{
    gv_v2 pts[4] = {
        gv_v2_make(x, y), gv_v2_make(x + w, y), gv_v2_make(x + w, y + h), gv_v2_make(x, y + h)
    };
    gv_draw_poly_filled(g, pts, 4, c);
}

void gv_draw_dim(gv_gfx *g, float alpha)
{
    if (!g) return;
    alpha = gv_clampf(alpha, 0.0f, 1.0f);
    if (alpha <= 0.0f) return;

    SDL_SetRenderDrawBlendMode(g->renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(g->renderer, 2, 4, 10, (Uint8)(alpha * 255.0f));
    SDL_Rect full = { 0, 0, g->view_w, g->view_h };
    SDL_RenderFillRect(g->renderer, &full);
}

/* ------------------------------------------------------------------ number */

void gv_format_number(char *buf, size_t buf_size, int64_t value)
{
    if (!buf || buf_size == 0) return;

    char digits[24];
    bool negative = value < 0;
    /* Negate in unsigned space so INT64_MIN does not overflow. */
    uint64_t magnitude = negative ? (uint64_t)(-(value + 1)) + 1u : (uint64_t)value;

    int n = 0;
    do {
        digits[n++] = (char)('0' + (int)(magnitude % 10u));
        magnitude /= 10u;
    } while (magnitude > 0u && n < (int)sizeof(digits));

    size_t out = 0;
    if (negative && out + 1 < buf_size) buf[out++] = '-';
    for (int i = n - 1; i >= 0; --i) {
        if (out + 1 >= buf_size) break;
        buf[out++] = digits[i];
        if (i > 0 && (i % 3) == 0 && out + 1 < buf_size) buf[out++] = ',';
    }
    buf[out] = '\0';
}
