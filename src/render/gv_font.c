/* gv_font.c — a single-stroke vector font.
 *
 * Bundling a TTF would mean shipping an asset, a loader and a licence. A
 * stroke font is a few hundred coordinates, scales to any size without
 * hinting, and — the reason it is here rather than a bitmap — glows through
 * exactly the same neon pipeline as the rest of the game, so the UI is made of
 * the same material as the arena.
 *
 * Glyphs live on a 6-wide, 8-tall grid with y running downward and the
 * baseline at y = 8. Descenders reach y = 10.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "gv_gfx.h"

#define B -1 /* pen up: start a new polyline */
#define E -2 /* end of glyph */

/* Horizontal advance per character, in grid units. */
#define GV_FONT_ADVANCE 7.4f
#define GV_FONT_CAP 8.0f

/* clang-format off */
static const int8_t k_sp[] = { E };
static const int8_t k_excl[] = { 3,0, 3,5, B, 3,7, 3,8, E };
static const int8_t k_quot[] = { 2,0, 2,2, B, 4,0, 4,2, E };
static const int8_t k_hash[] = { 1,2, 1,6, B, 4,2, 4,6, B, 0,3, 5,3, B, 0,5, 5,5, E };
static const int8_t k_dollar[] = { 6,1, 2,0, 0,2, 5,5, 4,7, 0,7, B, 3,0, 3,8, E };
static const int8_t k_pct[] = { 0,1, 1,0, 2,1, 1,2, 0,1, B, 6,0, 0,8, B, 4,7, 5,6, 6,7, 5,8, 4,7, E };
static const int8_t k_amp[] = { 6,8, 2,4, 1,2, 2,0, 4,1, 0,6, 1,8, 3,8, 6,5, E };
static const int8_t k_apos[] = { 3,0, 3,2, E };
static const int8_t k_lparen[] = { 4,0, 2,2, 2,6, 4,8, E };
static const int8_t k_rparen[] = { 2,0, 4,2, 4,6, 2,8, E };
static const int8_t k_star[] = { 3,2, 3,6, B, 1,3, 5,5, B, 5,3, 1,5, E };
static const int8_t k_plus[] = { 3,2, 3,6, B, 1,4, 5,4, E };
static const int8_t k_comma[] = { 3,7, 3,8, 2,10, E };
static const int8_t k_minus[] = { 1,4, 5,4, E };
static const int8_t k_dot[] = { 3,7, 3,8, E };
static const int8_t k_slash[] = { 6,0, 0,8, E };

static const int8_t k_0[] = { 2,0, 4,0, 6,2, 6,6, 4,8, 2,8, 0,6, 0,2, 2,0, B, 5,2, 1,6, E };
static const int8_t k_1[] = { 1,2, 3,0, 3,8, B, 1,8, 5,8, E };
static const int8_t k_2[] = { 0,2, 2,0, 4,0, 6,2, 6,3, 0,8, 6,8, E };
static const int8_t k_3[] = { 0,1, 2,0, 4,0, 6,2, 4,4, 2,4, B, 4,4, 6,6, 4,8, 2,8, 0,7, E };
static const int8_t k_4[] = { 4,8, 4,0, 0,5, 6,5, E };
static const int8_t k_5[] = { 6,0, 1,0, 0,3, 4,3, 6,5, 5,7, 3,8, 1,8, 0,7, E };
static const int8_t k_6[] = { 5,0, 2,1, 0,4, 0,6, 2,8, 4,8, 6,6, 5,4, 2,4, 0,6, E };
static const int8_t k_7[] = { 0,0, 6,0, 2,8, E };
static const int8_t k_8[] = { 2,4, 0,2, 2,0, 4,0, 6,2, 4,4, 2,4, 0,6, 2,8, 4,8, 6,6, 4,4, E };
static const int8_t k_9[] = { 1,8, 4,8, 6,5, 6,2, 4,0, 2,0, 0,2, 2,4, 5,4, 6,3, E };

static const int8_t k_colon[] = { 3,2, 3,3, B, 3,6, 3,7, E };
static const int8_t k_semi[] = { 3,2, 3,3, B, 3,6, 3,7, 2,9, E };
static const int8_t k_lt[] = { 5,1, 1,4, 5,7, E };
static const int8_t k_eq[] = { 1,3, 5,3, B, 1,5, 5,5, E };
static const int8_t k_gt[] = { 1,1, 5,4, 1,7, E };
static const int8_t k_quest[] = { 0,2, 2,0, 4,0, 6,2, 4,4, 3,5, B, 3,7, 3,8, E };
static const int8_t k_at[] = { 4,5, 3,4, 2,5, 3,6, 4,5, 4,3, 5,3, 6,4, 6,6, 4,8, 2,8, 0,6, 0,2, 2,0, 4,0, 6,1, E };

static const int8_t k_A[] = { 0,8, 3,0, 6,8, B, 1,5, 5,5, E };
static const int8_t k_Bc[] = { 0,0, 0,8, B, 0,0, 4,0, 5,1, 5,3, 4,4, 0,4, B, 4,4, 5,5, 5,7, 4,8, 0,8, E };
static const int8_t k_C[] = { 6,2, 4,0, 2,0, 0,2, 0,6, 2,8, 4,8, 6,6, E };
static const int8_t k_D[] = { 0,0, 0,8, B, 0,0, 3,0, 5,2, 5,6, 3,8, 0,8, E };
static const int8_t k_Ec[] = { 6,0, 0,0, 0,8, 6,8, B, 0,4, 4,4, E };
static const int8_t k_F[] = { 6,0, 0,0, 0,8, B, 0,4, 4,4, E };
static const int8_t k_G[] = { 6,2, 4,0, 2,0, 0,2, 0,6, 2,8, 4,8, 6,6, 6,4, 3,4, E };
static const int8_t k_H[] = { 0,0, 0,8, B, 6,0, 6,8, B, 0,4, 6,4, E };
static const int8_t k_I[] = { 1,0, 5,0, B, 3,0, 3,8, B, 1,8, 5,8, E };
static const int8_t k_J[] = { 5,0, 5,6, 4,8, 2,8, 0,6, E };
static const int8_t k_K[] = { 0,0, 0,8, B, 5,0, 0,4, 6,8, E };
static const int8_t k_L[] = { 0,0, 0,8, 6,8, E };
static const int8_t k_M[] = { 0,8, 0,0, 3,4, 6,0, 6,8, E };
static const int8_t k_N[] = { 0,8, 0,0, 6,8, 6,0, E };
static const int8_t k_O[] = { 2,0, 4,0, 6,2, 6,6, 4,8, 2,8, 0,6, 0,2, 2,0, E };
static const int8_t k_P[] = { 0,8, 0,0, 4,0, 6,2, 4,4, 0,4, E };
static const int8_t k_Q[] = { 2,0, 4,0, 6,2, 6,6, 4,8, 2,8, 0,6, 0,2, 2,0, B, 4,6, 6,9, E };
static const int8_t k_R[] = { 0,8, 0,0, 4,0, 6,2, 4,4, 0,4, B, 3,4, 6,8, E };
static const int8_t k_S[] = { 6,1, 4,0, 2,0, 0,2, 1,3, 5,5, 6,6, 4,8, 2,8, 0,7, E };
static const int8_t k_T[] = { 0,0, 6,0, B, 3,0, 3,8, E };
static const int8_t k_U[] = { 0,0, 0,6, 2,8, 4,8, 6,6, 6,0, E };
static const int8_t k_V[] = { 0,0, 3,8, 6,0, E };
static const int8_t k_W[] = { 0,0, 1,8, 3,4, 5,8, 6,0, E };
static const int8_t k_X[] = { 0,0, 6,8, B, 6,0, 0,8, E };
static const int8_t k_Y[] = { 0,0, 3,4, 6,0, B, 3,4, 3,8, E };
static const int8_t k_Z[] = { 0,0, 6,0, 0,8, 6,8, E };

static const int8_t k_lbrack[] = { 4,0, 2,0, 2,8, 4,8, E };
static const int8_t k_bslash[] = { 0,0, 6,8, E };
static const int8_t k_rbrack[] = { 2,0, 4,0, 4,8, 2,8, E };
static const int8_t k_caret[] = { 1,3, 3,0, 5,3, E };
static const int8_t k_under[] = { 0,9, 6,9, E };
static const int8_t k_grave[] = { 2,0, 4,2, E };
static const int8_t k_lbrace[] = { 4,0, 3,1, 3,3, 2,4, 3,5, 3,7, 4,8, E };
static const int8_t k_pipe[] = { 3,0, 3,8, E };
static const int8_t k_rbrace[] = { 2,0, 3,1, 3,3, 4,4, 3,5, 3,7, 2,8, E };
static const int8_t k_tilde[] = { 0,5, 2,3, 4,5, 6,3, E };

/* Indexed by (character - 32), covering printable ASCII. */
static const int8_t *const k_glyphs[] = {
    k_sp,     k_excl,   k_quot,   k_hash,   k_dollar, k_pct,    k_amp,    k_apos,
    k_lparen, k_rparen, k_star,   k_plus,   k_comma,  k_minus,  k_dot,    k_slash,
    k_0,      k_1,      k_2,      k_3,      k_4,      k_5,      k_6,      k_7,
    k_8,      k_9,      k_colon,  k_semi,   k_lt,     k_eq,     k_gt,     k_quest,
    k_at,     k_A,      k_Bc,     k_C,      k_D,      k_Ec,     k_F,      k_G,
    k_H,      k_I,      k_J,      k_K,      k_L,      k_M,      k_N,      k_O,
    k_P,      k_Q,      k_R,      k_S,      k_T,      k_U,      k_V,      k_W,
    k_X,      k_Y,      k_Z,      k_lbrack, k_bslash, k_rbrack, k_caret,  k_under,
    k_grave,  k_A,      k_Bc,     k_C,      k_D,      k_Ec,     k_F,      k_G,
    k_H,      k_I,      k_J,      k_K,      k_L,      k_M,      k_N,      k_O,
    k_P,      k_Q,      k_R,      k_S,      k_T,      k_U,      k_V,      k_W,
    k_X,      k_Y,      k_Z,      k_lbrace, k_pipe,   k_rbrace, k_tilde,
};
/* clang-format on */

#define GV_GLYPH_COUNT ((int)(sizeof(k_glyphs) / sizeof(k_glyphs[0])))

static const int8_t *gv_glyph_for(char c)
{
    int index = (int)(unsigned char)c - 32;
    if (index < 0 || index >= GV_GLYPH_COUNT) {
        /* Anything unprintable, including non-ASCII bytes, renders as a blank
         * rather than reading past the table. */
        return k_sp;
    }
    return k_glyphs[index];
}

float gv_text_width(float size, const char *text)
{
    if (!text) return 0.0f;
    float unit = size / GV_FONT_CAP;
    size_t n = strlen(text);
    if (n == 0) return 0.0f;
    /* Advance for every character, minus the trailing gap after the last. */
    return ((float)n * GV_FONT_ADVANCE - 1.4f) * unit;
}

void gv_draw_text(gv_gfx *g, float x, float y, float size, gv_color c, float glow,
                  const char *text)
{
    if (!g || !text || !(size > 0.0f) || !isfinite(x) || !isfinite(y)) return;

    float unit = size / GV_FONT_CAP;
    float width = gv_maxf(1.0f, size * 0.115f);
    float pen = x;

    for (const char *p = text; *p; ++p, pen += GV_FONT_ADVANCE * unit) {
        const int8_t *stroke = gv_glyph_for(*p);

        bool pen_down = false;
        gv_v2 prev = gv_v2_zero();
        for (int i = 0; stroke[i] != E; ) {
            if (stroke[i] == B) {
                pen_down = false;
                i++;
                continue;
            }
            gv_v2 pt = gv_v2_make(pen + (float)stroke[i] * unit,
                                  y + (float)stroke[i + 1] * unit);
            i += 2;
            if (pen_down) gv_draw_line(g, prev, pt, width, c, glow);
            prev = pt;
            pen_down = true;
        }
    }
}

void gv_draw_text_centred(gv_gfx *g, float cx, float y, float size, gv_color c, float glow,
                          const char *text)
{
    gv_draw_text(g, cx - gv_text_width(size, text) * 0.5f, y, size, c, glow, text);
}

void gv_draw_text_right(gv_gfx *g, float right, float y, float size, gv_color c, float glow,
                        const char *text)
{
    gv_draw_text(g, right - gv_text_width(size, text), y, size, c, glow, text);
}

void gv_draw_textf(gv_gfx *g, float x, float y, float size, gv_color c, float glow,
                   const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    gv_draw_text(g, x, y, size, c, glow, buf);
}

void gv_draw_textf_centred(gv_gfx *g, float cx, float y, float size, gv_color c, float glow,
                           const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    gv_draw_text_centred(g, cx, y, size, c, glow, buf);
}
