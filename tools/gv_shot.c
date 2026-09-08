/* gv_shot.c — headless screenshot harness.
 *
 * Renders the game to an offscreen surface with SDL's dummy video driver and
 * writes a BMP. This is how the renderer gets reviewed and regression-checked
 * without a display: it exercises the real gv_gfx and gv_scene code paths, so
 * a crash, a NaN reaching the geometry, or a blank frame shows up here.
 *
 * Usage: gv_shot <out.bmp> [seconds] [seed] [difficulty] [min_enemies]
 *
 * With min_enemies set, the harness keeps simulating past `seconds` until that
 * many enemies are on the field, so a review shot captures a real fight rather
 * than whatever lull the clock happened to land on.
 */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/game/gv_world.h"
#include "../src/render/gv_gfx.h"
#include "../src/render/gv_hud.h"
#include "../src/render/gv_scene.h"
#include "../src/game/gv_autopilot.h"

int main(int argc, char **argv)
{
    const char *out = (argc > 1) ? argv[1] : "shot.bmp";
    float seconds = (argc > 2) ? (float)atof(argv[2]) : 12.0f;
    uint64_t seed = (argc > 3) ? strtoull(argv[3], NULL, 10) : 7u;
    int difficulty = (argc > 4) ? atoi(argv[4]) : GV_DIFF_PILOT;

    /* Force the dummy driver unless the caller already chose one. */
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
    if (!getenv("SDL_VIDEODRIVER")) SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window *window = SDL_CreateWindow("graviton-shot", SDL_WINDOWPOS_CENTERED,
                                          SDL_WINDOWPOS_CENTERED, GV_VIEW_W, GV_VIEW_H,
                                          SDL_WINDOW_HIDDEN);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer) {
        fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    gv_gfx *gfx = gv_gfx_create(renderer, GV_VIEW_W, GV_VIEW_H);
    if (!gfx) {
        fprintf(stderr, "gv_gfx_create: %s\n", gv_gfx_error());
        return 1;
    }
    fprintf(stderr, "render targets: %s\n", gv_gfx_has_bloom(gfx) ? "yes (bloom on)" : "no");

    gv_scene *scene = gv_scene_create(seed);
    gv_world *world = (gv_world *)malloc(sizeof(gv_world));
    if (!scene || !world) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    gv_world_init(world, seed, difficulty);
    gv_scene_reset(scene, world);

    /* Play forward with the bot so the frame has something in it. */
    gv_rng jitter;
    gv_rng_seed(&jitter, seed ^ 0xB07u, 5u);
    int min_enemies = (argc > 5) ? atoi(argv[5]) : 0;
    int steps = (int)(seconds / GV_FIXED_DT);
    /* Generous ceiling on the extra search, so a bad request cannot hang. */
    int max_steps = steps + (int)(180.0f / GV_FIXED_DT);

    /* The HUD runs alongside the simulation, not after it. Building it at the
     * end instead made it see the current wave as one that had just started,
     * so every shot ever taken carried a spurious WAVE banner across the
     * middle of the frame — which is exactly the sort of thing a review of
     * these images is supposed to catch, not be fooled by. */
    gv_hud_state hud;
    gv_hud_init(&hud);

    for (int i = 0; i < max_steps; ++i) {
        gv_input in = gv_autopilot_think(world, &jitter);
        gv_world_step(world, &in, GV_FIXED_DT);
        gv_world_clear_events(world);
        gv_scene_update(scene, world, GV_FIXED_DT);
        gv_hud_update(&hud, world, GV_FIXED_DT);
        if (gv_world_is_over(world)) break;
        if (i >= steps && world->enemy_count >= min_enemies) break;
    }

    gv_gfx_begin_scene(gfx);
    gv_scene_draw(scene, gfx, world, 0.0f);
    gv_hud_draw(&hud, gfx, world);
    gv_gfx_end_scene(gfx, NULL);

    /* Read the composed backbuffer straight out of the renderer. */
    SDL_Surface *shot = SDL_CreateRGBSurfaceWithFormat(0, GV_VIEW_W, GV_VIEW_H, 32,
                                                       SDL_PIXELFORMAT_ARGB8888);
    if (!shot) {
        fprintf(stderr, "surface: %s\n", SDL_GetError());
        return 1;
    }
    if (SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_ARGB8888, shot->pixels,
                             shot->pitch) != 0) {
        fprintf(stderr, "SDL_RenderReadPixels: %s\n", SDL_GetError());
        return 1;
    }

    if (SDL_SaveBMP(shot, out) != 0) {
        fprintf(stderr, "SDL_SaveBMP: %s\n", SDL_GetError());
        return 1;
    }

    /* Report a crude brightness histogram: a silently black frame is the most
     * likely renderer failure and the easiest one to miss. */
    uint32_t *px = (uint32_t *)shot->pixels;
    long lit = 0, total = GV_VIEW_W * GV_VIEW_H;
    double sum = 0.0;
    for (long i = 0; i < total; ++i) {
        uint32_t p = px[i];
        int r = (int)((p >> 16) & 0xFF), g2 = (int)((p >> 8) & 0xFF), b = (int)(p & 0xFF);
        int v = (r > g2 ? r : g2) > b ? (r > g2 ? r : g2) : b;
        if (v > 16) lit++;
        sum += v;
    }
    fprintf(stderr, "wrote %s  wave=%d score=%lld lit=%.2f%% mean=%.1f\n", out, world->wave.index,
            (long long)world->score.score, 100.0 * (double)lit / (double)total,
            sum / (double)total);

    SDL_FreeSurface(shot);
    free(world);
    gv_scene_destroy(scene);
    gv_gfx_destroy(gfx);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
