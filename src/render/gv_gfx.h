/* gv_gfx.h — the drawing layer.
 *
 * GRAVITON ships no art files. Every texture here is generated procedurally at
 * startup, and every shape is drawn as glowing vector geometry, which is both
 * the aesthetic and the reason the whole game fits in a single self-contained
 * binary.
 *
 * The neon look is built the way it was before pixel shaders existed: each
 * element is drawn two or three times — a wide dim halo, a mid-width body and
 * a near-white core — all additively blended over black. An optional bloom
 * pass adds the final haze.
 */
#ifndef GV_GFX_H
#define GV_GFX_H

#include <SDL.h>
#include <stdbool.h>
#include <stdint.h>

#include "../core/gv_math.h"

typedef struct {
    uint8_t r, g, b, a;
} gv_color;

static inline gv_color gv_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    gv_color c = { r, g, b, 255 };
    return c;
}

static inline gv_color gv_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    gv_color c = { r, g, b, a };
    return c;
}

/* Scale a colour's alpha by t (0..1), clamped. */
static inline gv_color gv_fade(gv_color c, float t)
{
    float a = (float)c.a * gv_clampf(t, 0.0f, 1.0f);
    c.a = (uint8_t)gv_clampf(a, 0.0f, 255.0f);
    return c;
}

static inline gv_color gv_color_lerp(gv_color a, gv_color b, float t)
{
    t = gv_clampf(t, 0.0f, 1.0f);
    gv_color c;
    c.r = (uint8_t)gv_lerpf((float)a.r, (float)b.r, t);
    c.g = (uint8_t)gv_lerpf((float)a.g, (float)b.g, t);
    c.b = (uint8_t)gv_lerpf((float)a.b, (float)b.b, t);
    c.a = (uint8_t)gv_lerpf((float)a.a, (float)b.a, t);
    return c;
}

/* The view transform. World units are pixels at zoom 1. */
typedef struct {
    gv_v2 pos;   /* world-space centre of the view */
    float zoom;
    float shake_x, shake_y;
} gv_camera;

typedef struct gv_gfx gv_gfx;

/* ------------------------------------------------------------------ device */

/* Create the drawing layer against an existing renderer. `view_w`/`view_h` are
 * the fixed logical resolution everything is composed at. Returns NULL on
 * failure; call gv_gfx_error() for the reason. */
gv_gfx *gv_gfx_create(SDL_Renderer *renderer, int view_w, int view_h);
void gv_gfx_destroy(gv_gfx *g);
const char *gv_gfx_error(void);

/* True when render targets are available and the bloom pass can run. Some
 * software and remote drivers do not support them; the game still draws
 * correctly without, just without the haze. */
bool gv_gfx_has_bloom(const gv_gfx *g);
void gv_gfx_set_bloom(gv_gfx *g, bool enabled, float strength);

/* Begin composing a frame into the offscreen scene target (or straight to the
 * backbuffer when targets are unavailable). */
void gv_gfx_begin_scene(gv_gfx *g);

/* Resolve the scene: run bloom, then present it into `dest` on the backbuffer.
 * `dest` may be NULL to fill the whole window. */
void gv_gfx_end_scene(gv_gfx *g, const SDL_Rect *dest);

int gv_gfx_view_w(const gv_gfx *g);
int gv_gfx_view_h(const gv_gfx *g);

/* ---------------------------------------------------------------- camera */

/* Convert between world space and view space using the active camera. */
gv_v2 gv_gfx_world_to_view(const gv_gfx *g, gv_v2 world);
gv_v2 gv_gfx_view_to_world(const gv_gfx *g, gv_v2 view);
void gv_gfx_set_camera(gv_gfx *g, const gv_camera *cam);
const gv_camera *gv_gfx_camera(const gv_gfx *g);

/* True when a world-space circle could touch the visible area. Used to skip
 * work rather than to clip: the renderer stays correct without it. */
bool gv_gfx_visible(const gv_gfx *g, gv_v2 world, float radius);

/* ------------------------------------------------------------ primitives
 * All coordinates are view-space pixels unless the name says "world".
 * `glow` scales the halo: 0 draws a plain line, 1 is the standard neon look. */

void gv_draw_line(gv_gfx *g, gv_v2 a, gv_v2 b, float width, gv_color c, float glow);
void gv_draw_line_world(gv_gfx *g, gv_v2 a, gv_v2 b, float width, gv_color c, float glow);

/* A soft radial dot — the workhorse for point lights and particles. */
void gv_draw_glow(gv_gfx *g, gv_v2 centre, float radius, gv_color c);
void gv_draw_glow_world(gv_gfx *g, gv_v2 centre, float radius, gv_color c);

/* Closed polygon outline. `pts` are view-space. */
void gv_draw_poly(gv_gfx *g, const gv_v2 *pts, int count, float width, gv_color c, float glow);
void gv_draw_poly_filled(gv_gfx *g, const gv_v2 *pts, int count, gv_color c);

void gv_draw_circle(gv_gfx *g, gv_v2 centre, float radius, int segments, float width,
                    gv_color c, float glow);
void gv_draw_circle_world(gv_gfx *g, gv_v2 centre, float radius, int segments, float width,
                          gv_color c, float glow);

/* Partial ring, for meters and charge indicators. Angles in radians. */
void gv_draw_arc(gv_gfx *g, gv_v2 centre, float radius, float from, float to, int segments,
                 float width, gv_color c, float glow);

void gv_draw_rect(gv_gfx *g, float x, float y, float w, float h, float width, gv_color c,
                  float glow);
void gv_draw_rect_filled(gv_gfx *g, float x, float y, float w, float h, gv_color c);

/* Darken the whole view. Every other primitive here blends additively and so
 * can only ever brighten; this one uses ordinary alpha blending, which is what
 * lets a menu sit legibly over a running game. */
void gv_draw_dim(gv_gfx *g, float alpha);

/* --------------------------------------------------------------------- text
 * A built-in single-stroke vector font: no font files, and it scales and glows
 * exactly like every other shape in the game. `size` is the cap height in
 * pixels. Lowercase is drawn as uppercase. */

void gv_draw_text(gv_gfx *g, float x, float y, float size, gv_color c, float glow,
                  const char *text);
void gv_draw_text_centred(gv_gfx *g, float cx, float y, float size, gv_color c, float glow,
                          const char *text);
void gv_draw_text_right(gv_gfx *g, float right, float y, float size, gv_color c, float glow,
                        const char *text);
float gv_text_width(float size, const char *text);

/* Convenience: printf-style, into a fixed internal buffer. */
void gv_draw_textf(gv_gfx *g, float x, float y, float size, gv_color c, float glow,
                   const char *fmt, ...);
void gv_draw_textf_centred(gv_gfx *g, float cx, float y, float size, gv_color c, float glow,
                           const char *fmt, ...);

/* Format an integer with thousands separators into `buf`. */
void gv_format_number(char *buf, size_t buf_size, int64_t value);

#endif /* GV_GFX_H */
