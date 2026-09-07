#include "gv_hud.h"

#include <stdio.h>
#include <string.h>

#define GV_HUD_MARGIN 26.0f

static const gv_color k_hud_dim = { 132, 158, 196, 255 };
static const gv_color k_hud_bright = { 214, 238, 255, 255 };
static const gv_color k_hud_accent = { 120, 240, 255, 255 };
static const gv_color k_hud_warn = { 255, 120, 130, 255 };
static const gv_color k_hud_gold = { 255, 214, 130, 255 };

void gv_hud_init(gv_hud_state *h)
{
    if (!h) return;
    memset(h, 0, sizeof(*h));
    h->last_multiplier = 1;
    h->last_lives = -1;
}

void gv_hud_set_best(gv_hud_state *h, int64_t best)
{
    if (h) h->best_score = best;
}

static void gv_hud_banner(gv_hud_state *h, const char *title, const char *sub, float life)
{
    snprintf(h->banner, sizeof(h->banner), "%s", title ? title : "");
    snprintf(h->banner_sub, sizeof(h->banner_sub), "%s", sub ? sub : "");
    h->banner_timer = life;
    h->banner_life = life;
}

void gv_hud_update(gv_hud_state *h, const gv_world *w, float dt)
{
    if (!h || !w || !(dt > 0.0f)) return;

    /* Ease the displayed score so it rolls up rather than snapping — the
     * count-up is most of what makes a big kill feel like a big kill. */
    double target = (double)w->score.score;
    double diff = target - h->score_shown;
    if (diff > 0.0) {
        /* Fast enough to keep up with a chain, slow enough to be legible. */
        h->score_shown += diff * gv_minf(1.0f, 9.0f * dt) + gv_minf((float)diff, 60.0f * dt);
        if (h->score_shown > target) h->score_shown = target;
    } else {
        h->score_shown = target;
    }

    if (w->wave.index != h->last_wave && w->wave.index > 0) {
        h->last_wave = w->wave.index;
        char title[48];
        snprintf(title, sizeof(title), "WAVE %d", w->wave.index);
        gv_hud_banner(h, title, w->wave.warden_wave ? "WARDEN INBOUND" : "", 2.2f);
    }

    if (w->score.multiplier > h->last_multiplier) h->mult_pop = 1.0f;
    h->last_multiplier = w->score.multiplier;
    h->mult_pop = gv_maxf(0.0f, h->mult_pop - dt * 3.4f);

    if (h->last_lives >= 0 && w->player.lives < h->last_lives) h->life_flash = 1.0f;
    h->last_lives = w->player.lives;
    h->life_flash = gv_maxf(0.0f, h->life_flash - dt * 1.6f);

    h->hunt_pulse += dt * 4.0f;
    h->banner_timer = gv_maxf(0.0f, h->banner_timer - dt);
}

/* ------------------------------------------------------------------ widgets */

/* A horizontal meter with an optional threshold tick. */
static void gv_hud_bar(gv_gfx *g, float x, float y, float w, float h, float fill,
                       gv_color c, float tick)
{
    fill = gv_clampf(fill, 0.0f, 1.0f);
    gv_draw_rect(g, x, y, w, h, 1.2f, gv_fade(k_hud_dim, 0.45f), 0.0f);
    if (fill > 0.001f) {
        gv_draw_rect_filled(g, x + 1.5f, y + 1.5f, (w - 3.0f) * fill, h - 3.0f, gv_fade(c, 0.9f));
        gv_draw_glow(g, gv_v2_make(x + (w - 3.0f) * fill, y + h * 0.5f), h * 1.4f,
                     gv_fade(c, 0.55f));
    }
    if (tick >= 0.0f && tick <= 1.0f) {
        float tx = x + w * tick;
        gv_draw_line(g, gv_v2_make(tx, y - 3.0f), gv_v2_make(tx, y + h + 3.0f), 1.6f,
                     gv_fade(k_hud_gold, 0.9f), 0.8f);
    }
}

/* The ship glyph used for the lives readout. */
static void gv_hud_ship_icon(gv_gfx *g, gv_v2 at, float size, gv_color c)
{
    /* A filled dart with a bright core. The earlier open outline read as the
     * letter 'A' in a row, which is not what a life counter should say. */
    gv_v2 pts[4];
    pts[0] = gv_v2_add(at, gv_v2_polar(-GV_PI * 0.5f, size));
    pts[1] = gv_v2_add(at, gv_v2_polar(-GV_PI * 0.5f + 2.35f, size * 0.80f));
    pts[2] = gv_v2_add(at, gv_v2_polar(GV_PI * 0.5f, size * 0.22f));
    pts[3] = gv_v2_add(at, gv_v2_polar(-GV_PI * 0.5f - 2.35f, size * 0.80f));
    gv_draw_poly_filled(g, pts, 4, gv_fade(c, 0.62f));
    gv_draw_poly(g, pts, 4, 1.8f, c, 1.0f);
}

/* ---------------------------------------------------------------------- draw */

void gv_hud_draw(gv_hud_state *h, gv_gfx *g, const gv_world *w)
{
    if (!h || !g || !w) return;

    const float vw = (float)gv_gfx_view_w(g);
    const float vh = (float)gv_gfx_view_h(g);
    char buf[48];

    /* ---- score, top left ---- */
    gv_draw_text(g, GV_HUD_MARGIN, GV_HUD_MARGIN, 13.0f, gv_fade(k_hud_dim, 0.85f), 0.3f, "SCORE");
    gv_format_number(buf, sizeof(buf), (int64_t)(h->score_shown + 0.5));
    gv_draw_text(g, GV_HUD_MARGIN, GV_HUD_MARGIN + 20.0f, 34.0f, k_hud_bright, 0.9f, buf);

    /* ---- multiplier, under the score ---- */
    if (w->score.multiplier > 1) {
        float pop = 1.0f + h->mult_pop * 0.45f;
        float y = GV_HUD_MARGIN + 72.0f;
        gv_color c = gv_color_lerp(k_hud_accent, k_hud_gold,
                                   gv_clampf((float)w->score.multiplier / GV_MULT_MAX, 0.0f, 1.0f));
        snprintf(buf, sizeof(buf), "X%d", w->score.multiplier);
        gv_draw_text(g, GV_HUD_MARGIN, y, 26.0f * pop, c, 1.2f, buf);

        /* A draining ring shows exactly how long the chain has left. */
        float remaining = gv_clampf(w->score.mult_timer / GV_MULT_WINDOW, 0.0f, 1.0f);
        gv_v2 ring_at = gv_v2_make(GV_HUD_MARGIN + gv_text_width(26.0f * pop, buf) + 22.0f,
                                   y + 13.0f);
        gv_draw_circle(g, ring_at, 11.0f, 18, 1.2f, gv_fade(k_hud_dim, 0.4f), 0.0f);
        gv_draw_arc(g, ring_at, 11.0f, -GV_PI * 0.5f, -GV_PI * 0.5f + GV_TAU * remaining,
                    20, 2.4f, c, 1.0f);
    }

    /* ---- wave and best, top right ----
     * The director numbers waves from 1 but sits at 0 during the opening
     * intro, so a fresh run would otherwise announce "WAVE 0" for its first
     * couple of seconds. The player is already in wave one; say so. */
    snprintf(buf, sizeof(buf), "WAVE %d", gv_maxi(w->wave.index, 1));
    gv_draw_text_right(g, vw - GV_HUD_MARGIN, GV_HUD_MARGIN, 22.0f, k_hud_bright, 0.7f, buf);

    if (h->best_score > 0) {
        /* Sized so a fully separated 64-bit score plus the label always fits;
         * a truncated best score would be worse than none. */
        char best[48];
        char line[64];
        gv_format_number(best, sizeof(best), h->best_score);
        snprintf(line, sizeof(line), "BEST %s", best);
        gv_draw_text_right(g, vw - GV_HUD_MARGIN, GV_HUD_MARGIN + 28.0f, 13.0f,
                           gv_fade(k_hud_dim, 0.9f), 0.2f, line);
    }

    /* Escalation warning: the arena is closing in on a straggler. */
    if (w->wave.hunt > 0.05f) {
        float a = 0.45f + 0.55f * (0.5f + 0.5f * sinf(h->hunt_pulse));
        gv_draw_text_right(g, vw - GV_HUD_MARGIN, GV_HUD_MARGIN + 50.0f, 14.0f,
                           gv_fade(k_hud_warn, a * w->wave.hunt), 1.0f, "PURSUIT");
    }

    /* ---- lives, bottom left ---- */
    for (int i = 0; i < w->player.lives; ++i) {
        gv_v2 at = gv_v2_make(GV_HUD_MARGIN + 11.0f + (float)i * 26.0f, vh - GV_HUD_MARGIN - 12.0f);
        gv_color c = k_hud_accent;
        if (w->player.lives <= 1) {
            /* One life left: pulse, so the stake is impossible to miss. */
            c = gv_color_lerp(k_hud_warn, k_hud_bright, 0.5f + 0.5f * sinf(h->hunt_pulse * 1.6f));
        }
        gv_hud_ship_icon(g, at, 11.0f, c);
    }
    if (h->life_flash > 0.0f) {
        gv_draw_text(g, GV_HUD_MARGIN, vh - GV_HUD_MARGIN - 44.0f, 15.0f,
                     gv_fade(k_hud_warn, h->life_flash), 1.0f, "HULL BREACH");
    }

    /* ---- speed and overdrive, bottom centre ----
     * The most important meter in the game: it says whether the ship is
     * currently a weapon or a target. The gold tick is the overdrive line. */
    {
        const float bar_w = 300.0f, bar_h = 12.0f;
        float x = (vw - bar_w) * 0.5f;
        float y = vh - GV_HUD_MARGIN - 22.0f;
        float fill = gv_clampf(w->player.speed / GV_PLAYER_MAX_SPEED, 0.0f, 1.0f);
        float threshold = GV_OVERDRIVE_SPEED / GV_PLAYER_MAX_SPEED;

        gv_color c = (w->player.speed >= GV_OVERDRIVE_SPEED)
                         ? gv_color_lerp(k_hud_gold, gv_rgb(255, 140, 60), w->player.overdrive)
                         : k_hud_accent;
        gv_hud_bar(g, x, y, bar_w, bar_h, fill, c, threshold);

        const char *label = (w->player.speed >= GV_OVERDRIVE_SPEED) ? "OVERDRIVE" : "VELOCITY";
        gv_draw_text_centred(g, vw * 0.5f, y - 18.0f, 12.0f,
                             (w->player.speed >= GV_OVERDRIVE_SPEED) ? gv_fade(k_hud_gold, 0.95f)
                                                                     : gv_fade(k_hud_dim, 0.8f),
                             0.4f, label);
    }

    /* ---- pulse and focus, bottom right ---- */
    {
        float x = vw - GV_HUD_MARGIN;
        float y = vh - GV_HUD_MARGIN - 12.0f;

        /* Pulse charges as discrete pips: a resource you can count at a glance. */
        int whole = (int)w->player.pulse_charge;
        float partial = w->player.pulse_charge - (float)whole;
        for (int i = 0; i < (int)GV_PULSE_MAX; ++i) {
            gv_v2 at = gv_v2_make(x - 12.0f - (float)i * 28.0f, y);
            if (i < whole) {
                gv_draw_circle(g, at, 9.0f, 14, 2.0f, k_hud_accent, 1.1f);
                gv_draw_glow(g, at, 8.0f, gv_fade(k_hud_accent, 0.6f));
            } else if (i == whole && partial > 0.01f) {
                gv_draw_circle(g, at, 9.0f, 14, 1.2f, gv_fade(k_hud_dim, 0.4f), 0.0f);
                gv_draw_arc(g, at, 9.0f, -GV_PI * 0.5f, -GV_PI * 0.5f + GV_TAU * partial, 14,
                            2.0f, gv_fade(k_hud_accent, 0.8f), 0.8f);
            } else {
                gv_draw_circle(g, at, 9.0f, 14, 1.2f, gv_fade(k_hud_dim, 0.35f), 0.0f);
            }
        }
        gv_draw_text_right(g, x - 12.0f - (float)((int)GV_PULSE_MAX - 1) * 28.0f - 14.0f,
                           y - 6.0f, 12.0f, gv_fade(k_hud_dim, 0.8f), 0.2f, "PULSE");

        /* Focus meter. */
        float fx = x - 118.0f, fy = y - 34.0f;
        gv_color fc = w->player.focus_active ? gv_rgb(190, 150, 255) : gv_fade(k_hud_dim, 0.9f);
        gv_hud_bar(g, fx, fy, 118.0f, 7.0f, w->player.focus / GV_FOCUS_MAX, fc, -1.0f);
        gv_draw_text_right(g, fx - 8.0f, fy - 3.0f, 11.0f, gv_fade(k_hud_dim, 0.75f), 0.2f,
                           "FOCUS");
    }

    /* ---- centre banner ---- */
    if (h->banner_timer > 0.0f && h->banner[0]) {
        float t = h->banner_timer / gv_maxf(0.01f, h->banner_life);
        /* Snap in, hold, fade out. */
        float alpha = gv_minf(1.0f, t * 3.0f) * gv_minf(1.0f, (1.0f - t) * 6.0f + 0.35f);
        float slide = (1.0f - gv_minf(1.0f, (1.0f - t) * 5.0f)) * 24.0f;
        float y = vh * 0.26f - slide;

        gv_draw_text_centred(g, vw * 0.5f, y, 46.0f, gv_fade(k_hud_bright, alpha), 1.3f,
                             h->banner);
        if (h->banner_sub[0]) {
            gv_draw_text_centred(g, vw * 0.5f, y + 56.0f, 18.0f, gv_fade(k_hud_warn, alpha), 1.0f,
                                 h->banner_sub);
        }
    }

    /* ---- wave-clear flourish ---- */
    if (w->wave.state == GV_WAVE_CLEARED) {
        gv_draw_text_centred(g, vw * 0.5f, vh * 0.44f, 26.0f, gv_fade(k_hud_gold, 0.9f), 1.1f,
                             "WAVE CLEAR");
    }
}
