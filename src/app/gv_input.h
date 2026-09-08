/* gv_input.h — turns SDL input into the simulation's gv_input, and into
 * discrete menu actions.
 *
 * Keyboard and mouse is the primary scheme; a game controller is fully
 * supported and can be hot-plugged at any time. Both feed the same gv_input
 * struct, so the simulation never learns which one is in use.
 */
#ifndef GV_INPUT_H
#define GV_INPUT_H

#include <SDL.h>
#include <stdbool.h>

#include "../game/gv_types.h"
#include "../render/gv_gfx.h"

/* Discrete menu actions, edge-triggered. */
typedef enum {
    GV_NAV_NONE = 0,
    GV_NAV_UP,
    GV_NAV_DOWN,
    GV_NAV_LEFT,
    GV_NAV_RIGHT,
    GV_NAV_CONFIRM,
    GV_NAV_BACK
} gv_nav_action;

#define GV_NAV_QUEUE 8

typedef struct {
    SDL_GameController *pad;
    SDL_JoystickID pad_id;

    /* Where the scene is drawn inside the window, so mouse coordinates can be
     * mapped back into the fixed 1280x720 view. */
    SDL_Rect viewport;

    gv_v2 mouse_window;
    float wheel;            /* accumulated this frame, reset by begin_frame */
    bool mouse_left, mouse_right;
    bool mouse_moved_recently;

    /* Edge detection for the tether button. */
    bool tether_prev;
    bool pulse_prev;

    /* Last aim direction, kept so the ship holds its heading when neither the
     * mouse nor the right stick is giving a fresh direction. */
    gv_v2 aim_dir;
    bool using_pad;

    gv_nav_action nav[GV_NAV_QUEUE];
    int nav_count;
} gv_input_state;

void gv_input_init(gv_input_state *in);
void gv_input_shutdown(gv_input_state *in);

/* Clear per-frame accumulators. Call once before pumping events. */
void gv_input_begin_frame(gv_input_state *in);

/* Feed one SDL event. Handles controller hot-plug internally. */
void gv_input_handle_event(gv_input_state *in, const SDL_Event *e);

/* Tell the mapper where the scene is drawn, for mouse-to-world conversion. */
void gv_input_set_viewport(gv_input_state *in, SDL_Rect viewport);

/* Build one frame of simulation input. */
gv_input gv_input_build(gv_input_state *in, gv_gfx *gfx, const gv_world *w);

/* Pop the next queued menu action, or GV_NAV_NONE. */
gv_nav_action gv_input_next_nav(gv_input_state *in);

/* True when a controller is currently connected. */
bool gv_input_has_pad(const gv_input_state *in);

#endif /* GV_INPUT_H */
