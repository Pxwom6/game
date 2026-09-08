#include "gv_menu.h"

#include <stdio.h>
#include <string.h>

static const gv_color k_menu_idle = { 138, 166, 208, 255 };
static const gv_color k_menu_active = { 235, 250, 255, 255 };
static const gv_color k_menu_accent = { 120, 240, 255, 255 };
static const gv_color k_menu_value = { 190, 220, 255, 255 };

#define GV_ROW_GAP_SCALE 0.45f

void gv_menu_reset(gv_menu *m)
{
    if (!m) return;
    int selected = m->selected;
    float time = m->time;
    memset(m, 0, sizeof(*m));
    m->selected = selected;
    m->time = time;
}

static gv_menu_item *gv_menu_push(gv_menu *m, int kind, int id, const char *label)
{
    if (!m || m->count >= GV_MENU_MAX_ITEMS) return NULL;
    gv_menu_item *it = &m->items[m->count++];
    memset(it, 0, sizeof(*it));
    it->kind = (uint8_t)kind;
    it->id = id;
    it->label = label ? label : "";
    it->enabled = true;
    return it;
}

void gv_menu_add_action(gv_menu *m, int id, const char *label)
{
    gv_menu_push(m, GV_ITEM_ACTION, id, label);
}

void gv_menu_add_toggle(gv_menu *m, int id, const char *label, bool *value)
{
    gv_menu_item *it = gv_menu_push(m, GV_ITEM_TOGGLE, id, label);
    if (it) it->toggle = value;
}

void gv_menu_add_slider(gv_menu *m, int id, const char *label, float *value, float step)
{
    gv_menu_item *it = gv_menu_push(m, GV_ITEM_SLIDER, id, label);
    if (it) {
        it->slider = value;
        it->step = (step > 0.0f) ? step : 0.1f;
    }
}

void gv_menu_add_choice(gv_menu *m, int id, const char *label, int *value,
                        const char *const *names, int count)
{
    gv_menu_item *it = gv_menu_push(m, GV_ITEM_CHOICE, id, label);
    if (it) {
        it->choice = value;
        it->choice_names = names;
        it->choice_count = (count > 0) ? count : 1;
    }
}

void gv_menu_add_gap(gv_menu *m)
{
    gv_menu_item *it = gv_menu_push(m, GV_ITEM_GAP, -1, "");
    if (it) it->enabled = false;
}

static bool gv_item_selectable(const gv_menu_item *it)
{
    return it->enabled && it->kind != GV_ITEM_GAP;
}

/* Move the cursor, skipping gaps. Wraps at both ends. */
static void gv_menu_move(gv_menu *m, int delta)
{
    if (m->count <= 0) return;
    for (int tries = 0; tries < m->count; ++tries) {
        m->selected += delta;
        if (m->selected < 0) m->selected = m->count - 1;
        if (m->selected >= m->count) m->selected = 0;
        if (gv_item_selectable(&m->items[m->selected])) return;
    }
    /* Nothing selectable: park at the top rather than looping forever. */
    m->selected = 0;
}

int gv_menu_handle(gv_menu *m, gv_nav_action nav, bool *out_changed, bool *out_moved)
{
    if (out_changed) *out_changed = false;
    if (out_moved) *out_moved = false;
    if (!m || m->count <= 0 || nav == GV_NAV_NONE) return -1;

    m->selected = gv_clampi(m->selected, 0, m->count - 1);
    if (!gv_item_selectable(&m->items[m->selected])) gv_menu_move(m, 1);

    gv_menu_item *it = &m->items[m->selected];

    switch (nav) {
    case GV_NAV_UP:
        gv_menu_move(m, -1);
        if (out_moved) *out_moved = true;
        return -1;
    case GV_NAV_DOWN:
        gv_menu_move(m, 1);
        if (out_moved) *out_moved = true;
        return -1;

    case GV_NAV_LEFT:
    case GV_NAV_RIGHT: {
        int dir = (nav == GV_NAV_RIGHT) ? 1 : -1;
        switch (it->kind) {
        case GV_ITEM_SLIDER:
            if (it->slider) {
                *it->slider = gv_clampf(*it->slider + it->step * (float)dir, 0.0f, 1.0f);
                if (out_changed) *out_changed = true;
            }
            break;
        case GV_ITEM_TOGGLE:
            if (it->toggle) {
                *it->toggle = !*it->toggle;
                if (out_changed) *out_changed = true;
            }
            break;
        case GV_ITEM_CHOICE:
            if (it->choice && it->choice_count > 0) {
                int v = *it->choice + dir;
                /* Wrap, so a controller d-pad can cycle without dead ends. */
                if (v < 0) v = it->choice_count - 1;
                if (v >= it->choice_count) v = 0;
                *it->choice = v;
                if (out_changed) *out_changed = true;
            }
            break;
        default:
            break;
        }
        return -1;
    }

    case GV_NAV_CONFIRM:
        switch (it->kind) {
        case GV_ITEM_ACTION:
            return it->id;
        case GV_ITEM_TOGGLE:
            if (it->toggle) {
                *it->toggle = !*it->toggle;
                if (out_changed) *out_changed = true;
            }
            return -1;
        case GV_ITEM_CHOICE:
            if (it->choice && it->choice_count > 0) {
                *it->choice = (*it->choice + 1) % it->choice_count;
                if (out_changed) *out_changed = true;
            }
            return -1;
        default:
            return -1;
        }

    default:
        return -1;
    }
}

void gv_menu_update(gv_menu *m, float dt)
{
    if (!m || !(dt > 0.0f)) return;
    m->time += dt;
    m->select_pos = gv_approach_exp(m->select_pos, (float)m->selected, 22.0f, dt);
    m->select_anim = gv_approach_exp(m->select_anim, 1.0f, 10.0f, dt);
}

float gv_menu_height(const gv_menu *m, float row_h)
{
    if (!m) return 0.0f;
    float h = 0.0f;
    for (int i = 0; i < m->count; ++i) {
        h += (m->items[i].kind == GV_ITEM_GAP) ? row_h * GV_ROW_GAP_SCALE : row_h;
    }
    return h;
}

/* A slider drawn as a segmented bar: discrete steps read more clearly than a
 * continuous fill at a glance, and they match how the value actually changes. */
static void gv_draw_slider(gv_gfx *g, float x, float y, float w, float value, gv_color c)
{
    const int segments = 10;
    float seg_w = w / (float)segments;
    int filled = (int)(gv_clampf(value, 0.0f, 1.0f) * (float)segments + 0.5f);

    for (int i = 0; i < segments; ++i) {
        float sx = x + (float)i * seg_w;
        if (i < filled) {
            gv_draw_rect_filled(g, sx + 1.0f, y, seg_w - 3.0f, 9.0f, gv_fade(c, 0.9f));
        } else {
            gv_draw_rect(g, sx + 1.0f, y, seg_w - 3.0f, 9.0f, 1.0f, gv_fade(c, 0.28f), 0.0f);
        }
    }
}

void gv_menu_draw(gv_menu *m, gv_gfx *g, float cx, float top, float row_h, float size)
{
    if (!m || !g) return;

    const float value_x = cx + 130.0f;   /* right-hand column for values */
    const float label_right = cx + 100.0f;
    float y = top;

    for (int i = 0; i < m->count; ++i) {
        gv_menu_item *it = &m->items[i];
        if (it->kind == GV_ITEM_GAP) {
            y += row_h * GV_ROW_GAP_SCALE;
            continue;
        }

        bool selected = (i == m->selected);
        /* Distance from the animated cursor drives a subtle glow falloff, so
         * the highlight feels like it slides rather than teleports. */
        float nearness = 1.0f - gv_clampf(gv_absf((float)i - m->select_pos), 0.0f, 1.0f);
        gv_color c = gv_color_lerp(k_menu_idle, k_menu_active, nearness);
        float glow = 0.25f + nearness * 1.0f;
        if (!it->enabled) {
            c = gv_fade(k_menu_idle, 0.35f);
            glow = 0.0f;
        }

        if (selected) {
            /* Breathing caret. */
            float pulse = 0.55f + 0.45f * sinf(m->time * 5.0f);
            gv_v2 caret[3];
            float ax = cx - 190.0f;
            float ay = y + size * 0.5f;
            caret[0] = gv_v2_make(ax + 9.0f, ay);
            caret[1] = gv_v2_make(ax - 4.0f, ay - 7.0f);
            caret[2] = gv_v2_make(ax - 4.0f, ay + 7.0f);
            gv_draw_poly(g, caret, 3, 1.8f, gv_fade(k_menu_accent, pulse), 1.1f);
        }

        gv_draw_text_right(g, label_right, y, size, c, glow, it->label);

        char buf[64];
        switch (it->kind) {
        case GV_ITEM_TOGGLE:
            if (it->toggle) {
                gv_draw_text(g, value_x, y, size,
                             *it->toggle ? gv_color_lerp(k_menu_value, k_menu_accent, 0.6f)
                                         : gv_fade(k_menu_value, 0.45f),
                             glow, *it->toggle ? "ON" : "OFF");
            }
            break;
        case GV_ITEM_SLIDER:
            if (it->slider) {
                gv_draw_slider(g, value_x, y + size * 0.15f, 150.0f, *it->slider,
                               selected ? k_menu_accent : k_menu_value);
                snprintf(buf, sizeof(buf), "%d", (int)(gv_clampf(*it->slider, 0.0f, 1.0f) * 100.0f
                                                       + 0.5f));
                gv_draw_text(g, value_x + 162.0f, y, size * 0.85f, gv_fade(k_menu_value, 0.8f),
                             glow * 0.5f, buf);
            }
            break;
        case GV_ITEM_CHOICE:
            if (it->choice && it->choice_names && it->choice_count > 0) {
                int v = gv_clampi(*it->choice, 0, it->choice_count - 1);
                const char *name = it->choice_names[v] ? it->choice_names[v] : "?";
                gv_draw_text(g, value_x, y, size,
                             selected ? gv_color_lerp(k_menu_value, k_menu_accent, 0.7f)
                                      : k_menu_value,
                             glow, name);
                /* Arrows hint that this row cycles. */
                if (selected) {
                    gv_draw_text(g, value_x - 26.0f, y, size * 0.8f, gv_fade(k_menu_accent, 0.7f),
                                 0.5f, "<");
                    gv_draw_text(g, value_x + gv_text_width(size, name) + 12.0f, y, size * 0.8f,
                                 gv_fade(k_menu_accent, 0.7f), 0.5f, ">");
                }
            }
            break;
        default:
            break;
        }

        y += row_h;
    }
}
