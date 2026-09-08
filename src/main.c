/* main.c — entry point.
 *
 * Owns the window, the renderer and the frame clock. Everything else lives in
 * gv_app, which pumps its own events.
 */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>

#include "app/gv_app.h"
#include "app/gv_save.h"
#include "game/gv_types.h"

/* A frame longer than this means the process was stalled — a window drag, a
 * sleep, a breakpoint. Clamp rather than letting the simulation try to catch
 * up through it. */
#define GV_MAX_FRAME_SECONDS 0.25

static void gv_fatal(const char *what, const char *detail)
{
    fprintf(stderr, "GRAVITON: %s: %s\n", what, detail ? detail : "");
    /* A dialog is the only way a double-clicked .app can report anything. */
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "GRAVITON", what, NULL);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    SDL_SetHint(SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES, "1");
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        gv_fatal("Could not initialise SDL", SDL_GetError());
        return 1;
    }

    /* Load settings before the window exists so fullscreen and vsync are right
     * on the very first frame rather than snapping into place afterwards. */
    gv_save save;
    gv_save_load(&save);

    Uint32 window_flags = SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE;
    if (save.settings.fullscreen) window_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

    SDL_Window *window = SDL_CreateWindow("GRAVITON", SDL_WINDOWPOS_CENTERED,
                                          SDL_WINDOWPOS_CENTERED, GV_VIEW_W, GV_VIEW_H,
                                          window_flags);
    if (!window) {
        gv_fatal("Could not create the window", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_SetWindowMinimumSize(window, 640, 360);

    Uint32 renderer_flags = SDL_RENDERER_ACCELERATED;
    if (save.settings.vsync) renderer_flags |= SDL_RENDERER_PRESENTVSYNC;

    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, renderer_flags);
    if (!renderer) {
        /* Fall back to software rather than giving up: an old machine, a
         * remote session or a broken driver should still get a playable game,
         * just without the bloom pass. */
        fprintf(stderr, "GRAVITON: accelerated renderer unavailable (%s), using software\n",
                SDL_GetError());
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!renderer) {
        gv_fatal("Could not create a renderer", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    gv_app *app = gv_app_create(window, renderer, &save);
    if (!app) {
        gv_fatal("Could not start the game", gv_app_error());
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    const double frequency = (double)SDL_GetPerformanceFrequency();
    Uint64 previous = SDL_GetPerformanceCounter();

    while (!gv_app_should_quit(app)) {
        Uint64 now = SDL_GetPerformanceCounter();
        double dt = (frequency > 0.0) ? (double)(now - previous) / frequency : 1.0 / 60.0;
        previous = now;
        if (!(dt > 0.0)) dt = 1.0 / 60.0;
        if (dt > GV_MAX_FRAME_SECONDS) dt = GV_MAX_FRAME_SECONDS;

        gv_app_frame(app, (float)dt);
    }

    gv_app_persist(app);
    gv_app_destroy(app);
    gv_save_shutdown();

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
