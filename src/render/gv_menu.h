/* gv_menu.h — a small immediate-ish menu widget set.
 *
 * Items point directly at the settings they edit, so there is no copy-back
 * step and no chance of the menu and the live configuration disagreeing.
 */
#ifndef GV_MENU_H
#define GV_MENU_H

#include <stdbool.h>

#include "../app/gv_input.h"
#include "gv_gfx.h"

#define GV_MENU_MAX_ITEMS 18

typedef enum {
    GV_ITEM_ACTION = 0,
    GV_ITEM_TOGGLE,
    GV_ITEM_SLIDER,
    GV_ITEM_CHOICE,
    GV_ITEM_GAP
} gv_item_kind;

typedef struct {
    const char *label;
    uint8_t kind;
    int id;
    bool enabled;

    bool *toggle;
    float *slider;
    float step;
    int *choice;
    const char *const *choice_names;
    int choice_count;
} gv_menu_item;

typedef struct {
    gv_menu_item items[GV_MENU_MAX_ITEMS];
    int count;
    int selected;
    float time;
    float select_anim; /* eases the highlight between rows */
    float select_pos;
} gv_menu;

void gv_menu_reset(gv_menu *m);

void gv_menu_add_action(gv_menu *m, int id, const char *label);
void gv_menu_add_toggle(gv_menu *m, int id, const char *label, bool *value);
void gv_menu_add_slider(gv_menu *m, int id, const char *label, float *value, float step);
void gv_menu_add_choice(gv_menu *m, int id, const char *label, int *value,
                        const char *const *names, int count);
void gv_menu_add_gap(gv_menu *m);

/* Apply one navigation action.
 *
 * Returns the id of an activated action item, or -1. `out_changed` is set when
 * a value was edited (so the caller can play a click and persist), and
 * `out_moved` when the selection moved. */
int gv_menu_handle(gv_menu *m, gv_nav_action nav, bool *out_changed, bool *out_moved);

void gv_menu_update(gv_menu *m, float dt);

/* Draw centred on `cx`, starting at `top`, with `row_h` between rows. */
void gv_menu_draw(gv_menu *m, gv_gfx *g, float cx, float top, float row_h, float size);

/* Total height the menu will occupy, for layout. */
float gv_menu_height(const gv_menu *m, float row_h);

#endif /* GV_MENU_H */
