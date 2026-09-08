/* gv_app.h — the application: screens, the run loop and everything that binds
 * the simulation to the platform.
 *
 * main.c owns the window and the frame clock; everything else lives here.
 */
#ifndef GV_APP_H
#define GV_APP_H

#include <SDL.h>
#include <stdbool.h>

#include "gv_save.h"

typedef struct gv_app gv_app;

/* The screens the application can be on. Exposed so tests can assert exact
 * state transitions rather than inferring them from what got drawn. */
typedef enum {
    GV_SCREEN_TITLE = 0,
    GV_SCREEN_PLAY,
    GV_SCREEN_PAUSE,
    GV_SCREEN_SETTINGS,
    GV_SCREEN_SCORES,
    GV_SCREEN_HELP,
    GV_SCREEN_GAMEOVER,
    GV_SCREEN_COUNT
} gv_screen;

const char *gv_screen_name(int screen);

/* Takes ownership of nothing: the window and renderer outlive the app.
 * `save` is copied in; the app writes it back to disk as settings change.
 * Returns NULL on failure, with the reason in gv_app_error(). */
gv_app *gv_app_create(SDL_Window *window, SDL_Renderer *renderer, const gv_save *save);
void gv_app_destroy(gv_app *app);
const char *gv_app_error(void);

/* Feed one event. gv_app_frame() already drains the OS queue itself; this is
 * exposed for tests that want to synthesise input. */
void gv_app_handle_event(gv_app *app, const SDL_Event *event);

/* Pump events, advance and draw one frame. `real_dt` is wall-clock seconds
 * since the last call, already clamped by the caller. */
void gv_app_frame(gv_app *app, float real_dt);

bool gv_app_should_quit(const gv_app *app);

/* Which screen is currently active. */
int gv_app_screen(const gv_app *app);

/* Read-only view of the saved state, for tests and for the title screen. */
const gv_save *gv_app_save(const gv_app *app);

/* Label of the currently highlighted menu row, or NULL when the active screen
 * has no menu. Exposed so a test can drive the interface the way a player does
 * — "press down until PLAY is highlighted, then activate it" — rather than
 * counting keypresses against a layout that shifts the moment a row is added
 * or the cursor wraps. */
const char *gv_app_menu_selection(const gv_app *app);

/* Persist settings and scores. Called on exit; also called opportunistically
 * when settings change. */
void gv_app_persist(gv_app *app);

#endif /* GV_APP_H */
