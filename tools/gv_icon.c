/* gv_icon.c — generates the macOS application icon.
 *
 * GRAVITON ships no art files, and the icon is no exception: it is drawn here
 * with the game's own neon renderer, so it is literally made of the same
 * glowing vector strokes as the game itself and cannot drift out of sync with
 * the art direction.
 *
 * Output is a complete .iconset directory — the ten PNGs `iconutil` expects —
 * ready to be turned into a .icns. The art is composed once at 1024x1024 and
 * box-filtered down to each size, which gives clean antialiasing at 16px where
 * re-drawing hairline strokes would fall apart.
 *
 * The icon is masked to the rounded-square silhouette macOS has expected since
 * Big Sur, inset from the canvas edge the way the human interface guidelines
 * lay out, so it sits correctly in the Dock next to system icons.
 *
 * Usage: gv_icon <out.iconset directory>
 */
#include <SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/render/gv_gfx.h"

#define GV_ICON_SRC 1024

/* ============================================================== PNG writing
 *
 * A hand-rolled encoder, so the build depends on nothing but SDL and libm.
 * The image data is wrapped in *stored* (uncompressed) deflate blocks, which
 * is entirely legal zlib and needs no compressor — just an Adler-32 for the
 * zlib trailer and a CRC-32 per PNG chunk. Icons are small; the few hundred KB
 * this costs versus a compressed stream is irrelevant, and it keeps the tool
 * free of a third-party dependency.
 */

static uint32_t gv_crc32(const uint8_t *data, size_t len, uint32_t crc)
{
    static uint32_t table[256];
    static bool built;
    if (!built) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        built = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < len; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static void gv_put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static bool gv_png_chunk(FILE *f, const char *type, const uint8_t *data, size_t len)
{
    uint8_t head[8];
    gv_put_be32(head, (uint32_t)len);
    memcpy(head + 4, type, 4);
    if (fwrite(head, 1, 8, f) != 8) return false;
    if (len && fwrite(data, 1, len, f) != len) return false;

    uint32_t crc = gv_crc32((const uint8_t *)type, 4, 0);
    if (len) crc = gv_crc32(data, len, crc);

    uint8_t tail[4];
    gv_put_be32(tail, crc);
    return fwrite(tail, 1, 4, f) == 4;
}

/* `pixels` is RGBA, top row first, `w * h * 4` bytes. */
static bool gv_write_png(const char *path, const uint8_t *pixels, int w, int h)
{
    /* Raw scanlines with a leading filter byte (0 = None) each. */
    size_t stride = (size_t)w * 4 + 1;
    size_t raw_len = stride * (size_t)h;
    uint8_t *raw = (uint8_t *)malloc(raw_len);
    if (!raw) return false;
    for (int y = 0; y < h; ++y) {
        raw[stride * (size_t)y] = 0;
        memcpy(raw + stride * (size_t)y + 1, pixels + (size_t)y * (size_t)w * 4, (size_t)w * 4);
    }

    /* zlib stream: 2-byte header, stored deflate blocks, Adler-32 trailer. */
    const size_t block = 65535;
    size_t blocks = (raw_len + block - 1) / block;
    if (blocks == 0) blocks = 1;
    size_t z_len = 2 + blocks * 5 + raw_len + 4;
    uint8_t *z = (uint8_t *)malloc(z_len);
    if (!z) {
        free(raw);
        return false;
    }

    size_t zi = 0;
    z[zi++] = 0x78; /* CM=8 (deflate), CINFO=7 (32K window) */
    z[zi++] = 0x01; /* no preset dictionary, check bits make it a multiple of 31 */

    size_t done = 0;
    while (done < raw_len) {
        size_t n = raw_len - done;
        if (n > block) n = block;
        bool last = (done + n >= raw_len);
        z[zi++] = last ? 1 : 0;
        z[zi++] = (uint8_t)(n & 0xFF);
        z[zi++] = (uint8_t)(n >> 8);
        z[zi++] = (uint8_t)(~n & 0xFF);
        z[zi++] = (uint8_t)((~n >> 8) & 0xFF);
        if (n) memcpy(z + zi, raw + done, n);
        zi += n;
        done += n;
        if (last) break;
    }

    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_len; ++i) {
        a = (a + raw[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    gv_put_be32(z + zi, (b << 16) | a);
    zi += 4;
    free(raw);

    FILE *f = fopen(path, "wb");
    if (!f) {
        free(z);
        return false;
    }

    static const uint8_t sig[8] = { 137, 'P', 'N', 'G', '\r', '\n', 26, '\n' };
    bool ok = fwrite(sig, 1, 8, f) == 8;

    uint8_t ihdr[13];
    gv_put_be32(ihdr, (uint32_t)w);
    gv_put_be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8;  /* 8 bits per channel */
    ihdr[9] = 6;  /* truecolour with alpha */
    ihdr[10] = 0; /* deflate */
    ihdr[11] = 0; /* adaptive filtering */
    ihdr[12] = 0; /* no interlace */
    ok = ok && gv_png_chunk(f, "IHDR", ihdr, sizeof(ihdr));
    ok = ok && gv_png_chunk(f, "IDAT", z, zi);
    ok = ok && gv_png_chunk(f, "IEND", NULL, 0);
    if (fclose(f) != 0) ok = false;

    free(z);
    return ok;
}

/* ================================================================== the art */

/* Superellipse coverage at (x, y) in [-1, 1], the shape macOS uses for app
 * icons. Returns 0 outside, 1 inside, and an antialiased edge between. */
static float gv_squircle(float x, float y, float half, float feather)
{
    const float n = 5.0f; /* exponent: 2 is a circle, large is a square */
    float d = powf(fabsf(x / half), n) + powf(fabsf(y / half), n);
    /* Convert the implicit value to an approximate signed distance so the
     * feather is uniform around the shape rather than pinched at the corners. */
    float r = powf(d, 1.0f / n);
    return gv_clampf((1.0f - r) / feather + 0.5f, 0.0f, 1.0f);
}

/* Draw the icon into a canvas `s` pixels square.
 *
 * `weight` thickens every stroke. Strokes are specified as a fraction of the
 * canvas, so a 16px icon rendered from the same artwork would carry hairlines
 * a third of a pixel wide and dissolve into a smudge. The small sizes are
 * therefore drawn from their own heavier plate — the same thing an icon
 * designer does by hand, and the reason there are two render passes below.
 *
 * The composition is deliberately reducible: a tether ring, an anchor, and the
 * ship being slung off it. That silhouette still reads at sixteen pixels. */
static void gv_draw_icon(gv_gfx *g, float s, float weight)
{
    const gv_v2 c = gv_v2_make(s * 0.5f, s * 0.5f);

    const gv_color col_player = gv_rgb(100, 230, 255);
    const gv_color col_tether = gv_rgb(124, 255, 196);
    const gv_color col_over = gv_rgb(255, 176, 70);
    const gv_color col_pylon = gv_rgb(150, 165, 255);

    /* A soft field behind everything so the plate is never flat black. */
    gv_draw_glow(g, c, s * 0.62f, gv_rgba(38, 60, 130, 190));
    gv_draw_glow(g, gv_v2_make(s * 0.30f, s * 0.30f), s * 0.34f, gv_rgba(90, 40, 150, 120));

    /* The orbit: one bright ring, the shape the whole game is played on. */
    /* Everything must fit inside the masked plate (half = 0.824 of the canvas):
     * the ship's nose reaches ring + r * 1.9, so those two numbers are chosen
     * together, not independently. */
    float ring = s * 0.240f;
    gv_draw_circle(g, c, ring, 128, s * 0.030f * weight, gv_fade(col_tether, 0.95f), 1.35f);
    gv_draw_circle(g, c, ring * 1.36f, 96, s * 0.009f * weight, gv_fade(col_tether, 0.26f), 0.9f);

    /* The anchor the rope is attached to. */
    gv_v2 anchor = gv_v2_add(c, gv_v2_polar(GV_PI * 0.78f, ring));
    gv_draw_glow(g, anchor, s * 0.10f, gv_rgba(122, 132, 255, 200));
    gv_draw_circle(g, anchor, s * 0.040f, 24, s * 0.016f * weight, col_pylon, 1.2f);

    /* The rope, taut across the ring to the ship. */
    gv_v2 ship = gv_v2_add(c, gv_v2_polar(-GV_PI * 0.16f, ring));
    gv_draw_line(g, anchor, ship, s * 0.016f * weight, gv_fade(col_tether, 0.85f), 1.3f);

    /* The ship: the same forward-swept dart the game draws, at the moment of
     * release — travelling fast, so it wears the overdrive halo. The core stays
     * warm rather than white; a white-hot centre blows out to a featureless
     * blob the moment the icon is scaled down. */
    float r = s * 0.080f;
    float angle = -GV_PI * 0.16f + GV_PI * 0.5f; /* tangent to the orbit */
    gv_v2 hull[4];
    hull[0] = gv_v2_add(ship, gv_v2_polar(angle, r * 1.9f));
    hull[1] = gv_v2_add(ship, gv_v2_polar(angle + 2.55f, r * 1.25f));
    hull[2] = gv_v2_add(ship, gv_v2_polar(angle + GV_PI, r * 0.55f));
    hull[3] = gv_v2_add(ship, gv_v2_polar(angle - 2.55f, r * 1.25f));

    gv_draw_glow(g, ship, r * 3.6f, gv_fade(col_over, 0.34f));
    gv_draw_poly_filled(g, hull, 4, gv_rgba(8, 14, 26, 245));
    gv_draw_poly(g, hull, 4, s * 0.020f * weight, col_player, 1.15f);
    gv_draw_glow(g, ship, r * 0.55f, gv_rgba(210, 245, 255, 190));
}

/* ============================================================ the icon sizes */

typedef struct {
    const char *name;
    int size;
} gv_icon_size;

/* Exactly the set `iconutil` requires; anything missing makes it refuse the
 * whole iconset. Split by which plate they are cut from — see gv_draw_icon. */
static const gv_icon_size k_large[] = {
    { "icon_128x128.png", 128 }, { "icon_128x128@2x.png", 256 },
    { "icon_256x256.png", 256 }, { "icon_256x256@2x.png", 512 },
    { "icon_512x512.png", 512 }, { "icon_512x512@2x.png", 1024 },
};
static const gv_icon_size k_small[] = {
    { "icon_16x16.png", 16 },
    { "icon_16x16@2x.png", 32 },
    { "icon_32x32.png", 32 },
    { "icon_32x32@2x.png", 64 },
};

/* Box-filter a square RGBA plate down to `size`. Alpha is averaged alongside
 * colour, which is correct here because the source is effectively premultiplied
 * by construction: every pixel the mask fades out is fading a colour that is
 * already near-black. */
static void gv_downsample(const uint8_t *src, int src_size, uint8_t *dst, int size)
{
    int factor = src_size / size;
    float inv = 1.0f / (float)(factor * factor);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            float acc[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            for (int sy = 0; sy < factor; ++sy) {
                const uint8_t *row =
                    src + ((size_t)(y * factor + sy) * (size_t)src_size + (size_t)(x * factor)) * 4;
                for (int sx = 0; sx < factor; ++sx) {
                    for (int k = 0; k < 4; ++k) acc[k] += (float)row[sx * 4 + k];
                }
            }
            uint8_t *out = dst + ((size_t)y * (size_t)size + (size_t)x) * 4;
            for (int k = 0; k < 4; ++k) out[k] = (uint8_t)(acc[k] * inv + 0.5f);
        }
    }
}

/* Render one plate: draw the artwork at `canvas` square, mask it to the
 * rounded-square silhouette and return it as RGBA. Caller frees. */
static uint8_t *gv_render_plate(int canvas, float weight)
{
    SDL_Window *window =
        SDL_CreateWindow("graviton-icon", 0, 0, canvas, canvas, SDL_WINDOW_HIDDEN);
    SDL_Renderer *renderer = window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE) : NULL;
    gv_gfx *gfx = renderer ? gv_gfx_create(renderer, canvas, canvas) : NULL;
    if (!gfx) {
        fprintf(stderr, "could not set up a %dpx renderer: %s\n", canvas,
                renderer ? gv_gfx_error() : SDL_GetError());
        if (renderer) SDL_DestroyRenderer(renderer);
        if (window) SDL_DestroyWindow(window);
        return NULL;
    }

    gv_gfx_begin_scene(gfx);
    gv_draw_icon(gfx, (float)canvas, weight);
    gv_gfx_end_scene(gfx, NULL);

    SDL_Surface *frame =
        SDL_CreateRGBSurfaceWithFormat(0, canvas, canvas, 32, SDL_PIXELFORMAT_ARGB8888);
    uint8_t *rgba = (uint8_t *)malloc((size_t)canvas * (size_t)canvas * 4);
    if (!frame || !rgba ||
        SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_ARGB8888, frame->pixels,
                             frame->pitch) != 0) {
        fprintf(stderr, "could not read the composed frame: %s\n", SDL_GetError());
        free(rgba);
        rgba = NULL;
    }

    if (rgba) {
        /* Apply the rounded-square mask. The plate is lifted towards a deep
         * blue, brighter at the top edge, so the icon reads as an object on a
         * dark Dock rather than a hole in it. */
        const float half = 0.824f; /* content inset: the icon never fills the canvas */
        const float feather = 6.0f / (float)canvas;
        const uint32_t *px = (const uint32_t *)frame->pixels;

        for (int y = 0; y < canvas; ++y) {
            float ny = ((float)y + 0.5f) / (float)canvas * 2.0f - 1.0f;
            for (int x = 0; x < canvas; ++x) {
                float nx = ((float)x + 0.5f) / (float)canvas * 2.0f - 1.0f;
                float cover = gv_squircle(nx, ny, half, feather);

                uint32_t p = px[(size_t)y * (size_t)canvas + (size_t)x];
                float r = (float)((p >> 16) & 0xFF);
                float g = (float)((p >> 8) & 0xFF);
                float b = (float)(p & 0xFF);

                float lift = 1.0f - ((float)y / (float)canvas) * 0.55f;
                r += 10.0f * lift;
                g += 16.0f * lift;
                b += 38.0f * lift;

                uint8_t *out = rgba + ((size_t)y * (size_t)canvas + (size_t)x) * 4;
                out[0] = (uint8_t)gv_clampf(r * cover, 0.0f, 255.0f);
                out[1] = (uint8_t)gv_clampf(g * cover, 0.0f, 255.0f);
                out[2] = (uint8_t)gv_clampf(b * cover, 0.0f, 255.0f);
                out[3] = (uint8_t)gv_clampf(cover * 255.0f, 0.0f, 255.0f);
            }
        }
    }

    if (frame) SDL_FreeSurface(frame);
    gv_gfx_destroy(gfx);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    return rgba;
}

static bool gv_cut_sizes(const char *dir, const uint8_t *plate, int canvas,
                         const gv_icon_size *sizes, size_t count)
{
    uint8_t *scratch = (uint8_t *)malloc((size_t)canvas * (size_t)canvas * 4);
    if (!scratch) return false;

    bool ok = true;
    for (size_t i = 0; i < count && ok; ++i) {
        const uint8_t *data = plate;
        if (sizes[i].size != canvas) {
            gv_downsample(plate, canvas, scratch, sizes[i].size);
            data = scratch;
        }
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, sizes[i].name);
        ok = gv_write_png(path, data, sizes[i].size, sizes[i].size);
        if (!ok) fprintf(stderr, "could not write %s\n", path);
    }

    free(scratch);
    return ok;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <out.iconset directory>\n", argv[0]);
        return 2;
    }
    const char *dir = argv[1];

    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
    if (!getenv("SDL_VIDEODRIVER")) SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    /* Two plates: the display sizes from a 1024px render, and the list-view
     * sizes from a 256px render with heavier strokes so they survive the
     * reduction. */
    int failed = 0;
    uint8_t *large = gv_render_plate(GV_ICON_SRC, 1.0f);
    if (!large || !gv_cut_sizes(dir, large, GV_ICON_SRC, k_large,
                                sizeof(k_large) / sizeof(k_large[0]))) {
        failed = 1;
    }
    free(large);

    uint8_t *small = gv_render_plate(256, 2.4f);
    if (!small ||
        !gv_cut_sizes(dir, small, 256, k_small, sizeof(k_small) / sizeof(k_small[0]))) {
        failed = 1;
    }
    free(small);

    SDL_Quit();
    if (failed) return 1;

    fprintf(stderr, "wrote %zu icon images to %s\n",
            sizeof(k_large) / sizeof(k_large[0]) + sizeof(k_small) / sizeof(k_small[0]), dir);
    return 0;
}
