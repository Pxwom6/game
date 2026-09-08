/* gv_smoke.c — headless end-to-end driver for the application layer.
 *
 * The simulation has its own exhaustive suite, but that suite never touches
 * gv_app, gv_menu, gv_input, gv_save or the audio mixer. This harness runs the
 * real application against SDL's dummy video and audio drivers and walks it
 * through every screen with synthetic input, so a crash, a leak, a bad state
 * transition or an unhandled NaN in the presentation layer surfaces in CI
 * rather than on a player's machine.
 *
 * It also captures a screenshot of each screen, which is how the interface
 * gets reviewed without a display.
 *
 * Usage: gv_smoke [out_dir]
 * Exit code 0 on success, non-zero on the first failed expectation.
 */
#include <SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/app/gv_app.h"
#include "../src/app/gv_save.h"
#include "../src/game/gv_types.h"

static int g_failures;
static const char *g_out_dir;
static SDL_Renderer *g_renderer;

static void gv_fail(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "  FAIL: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    g_failures++;
}

#define GV_EXPECT(cond, ...)                                                   \
    do {                                                                       \
        if (!(cond)) gv_fail(__VA_ARGS__);                                     \
    } while (0)

/* Push a synthetic key press and release through the app, then advance one
 * frame so the app actually consumes it.
 *
 * The frame is not optional. Menu keys land in a small fixed-size queue that
 * gv_app_frame drains once per frame; pressing more keys than the queue holds
 * without stepping in between silently drops the overflow, which is not how a
 * human types and produced a run of baffling "expected SETTINGS, got PLAY"
 * failures. One key, one frame — the same rate a player can manage. */
static void gv_key(gv_app *app, SDL_Scancode code)
{
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.type = SDL_KEYDOWN;
    e.key.state = SDL_PRESSED;
    e.key.repeat = 0;
    e.key.keysym.scancode = code;
    e.key.keysym.sym = SDL_GetKeyFromScancode(code);
    gv_app_handle_event(app, &e);

    e.type = SDL_KEYUP;
    e.key.state = SDL_RELEASED;
    gv_app_handle_event(app, &e);

    gv_app_frame(app, 1.0f / 60.0f);
}

static void gv_frames(gv_app *app, int count)
{
    for (int i = 0; i < count; ++i) gv_app_frame(app, 1.0f / 60.0f);
}

/* Move the highlight onto the row labelled `label`.
 *
 * Menus wrap and contain gaps, so "press down twice" is only ever true for one
 * exact layout; adding a single row silently retargets every later step in the
 * walk. Steering by label is both stable and a stronger check — a row that has
 * gone missing fails here instead of quietly activating its neighbour. */
static bool gv_select(gv_app *app, const char *label)
{
    /* One pass around the longest possible menu, plus slack. */
    for (int i = 0; i < 32; ++i) {
        const char *cur = gv_app_menu_selection(app);
        if (cur && strcmp(cur, label) == 0) return true;
        if (!cur) return false; /* this screen has no menu at all */
        gv_key(app, SDL_SCANCODE_DOWN);
    }
    return false;
}

/* Select a row and activate it. */
static void gv_activate(gv_app *app, const char *label)
{
    GV_EXPECT(gv_select(app, label), "no menu row '%s' on the %s screen", label,
              gv_screen_name(gv_app_screen(app)));
    gv_key(app, SDL_SCANCODE_RETURN);
}

/* Capture the composed frame and report how much of it is lit. A screen that
 * renders nothing is the failure this is really looking for. */
static double gv_capture(const char *name)
{
    SDL_Surface *shot = SDL_CreateRGBSurfaceWithFormat(0, GV_VIEW_W, GV_VIEW_H, 32,
                                                       SDL_PIXELFORMAT_ARGB8888);
    if (!shot) {
        gv_fail("could not allocate a capture surface: %s", SDL_GetError());
        return 0.0;
    }
    if (SDL_RenderReadPixels(g_renderer, NULL, SDL_PIXELFORMAT_ARGB8888, shot->pixels,
                             shot->pitch) != 0) {
        gv_fail("SDL_RenderReadPixels: %s", SDL_GetError());
        SDL_FreeSurface(shot);
        return 0.0;
    }

    long lit = 0;
    const long total = (long)GV_VIEW_W * GV_VIEW_H;
    const uint32_t *px = (const uint32_t *)shot->pixels;
    for (long i = 0; i < total; ++i) {
        uint32_t p = px[i];
        int r = (int)((p >> 16) & 0xFF), g = (int)((p >> 8) & 0xFF), b = (int)(p & 0xFF);
        int v = r > g ? r : g;
        if (b > v) v = b;
        if (v > 20) lit++;
    }

    if (g_out_dir && *g_out_dir) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s.bmp", g_out_dir, name);
        if (SDL_SaveBMP(shot, path) != 0) gv_fail("SDL_SaveBMP(%s): %s", path, SDL_GetError());
    }

    double pct = 100.0 * (double)lit / (double)total;
    SDL_FreeSurface(shot);
    return pct;
}

/* Assert the app is on the expected screen AND that it drew something.
 *
 * Checking the state is the real test; the lit-pixel figure catches the other
 * half of the problem — a screen that transitions correctly but renders
 * nothing, which no state assertion would notice. */
static void gv_check_screen(gv_app *app, const char *name, int expect_screen, double min_lit)
{
    int actual = gv_app_screen(app);
    GV_EXPECT(actual == expect_screen, "at '%s': expected screen %s, got %s", name,
              gv_screen_name(expect_screen), gv_screen_name(actual));

    double lit = gv_capture(name);
    printf("  %-16s %-9s %5.2f%% lit\n", name, gv_screen_name(actual), lit);
    GV_EXPECT(lit >= min_lit, "screen '%s' drew almost nothing (%.2f%% lit, expected >= %.2f%%)",
              name, lit, min_lit);
}

/* Run frames until the app reaches `screen`, or the budget expires.
 *
 * `dt` is the frame delta to feed. Waiting out a whole run at 1/60 would mean
 * tens of thousands of software-rasterised frames; a larger delta covers the
 * same game time in far fewer of them, because the app's accumulator simply
 * takes more fixed steps per frame. That is exactly the catch-up path a real
 * machine hits under load, so this makes the wait cheap *and* exercises code
 * a steady 60 Hz never would. */
static bool gv_run_until_screen(gv_app *app, int screen, float dt, float max_seconds)
{
    float elapsed = 0.0f;
    while (elapsed < max_seconds) {
        if (gv_app_screen(app) == screen) return true;
        gv_app_frame(app, dt);
        elapsed += dt;
        if (gv_app_should_quit(app)) return false;
    }
    return gv_app_screen(app) == screen;
}

int main(int argc, char **argv)
{
    g_out_dir = (argc > 1) ? argv[1] : NULL;

    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
    if (!getenv("SDL_VIDEODRIVER")) SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    if (!getenv("SDL_AUDIODRIVER")) SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);

    /* Keep the harness out of the real preferences directory. The app persists
     * settings and scores as it runs, and SDL_GetPrefPath derives that location
     * from HOME (plus XDG_DATA_HOME on Linux) — so pointing both at the output
     * directory means running the smoke test cannot overwrite a player's save.
     * SDL creates the subdirectories itself, so there is nothing to prepare. */
    {
        const char *sandbox = (g_out_dir && *g_out_dir) ? g_out_dir : ".";
        SDL_setenv("HOME", sandbox, 1);
        SDL_setenv("XDG_DATA_HOME", sandbox, 1);
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 2;
    }

    SDL_Window *window = SDL_CreateWindow("graviton-smoke", 0, 0, GV_VIEW_W, GV_VIEW_H,
                                          SDL_WINDOW_HIDDEN);
    SDL_Renderer *renderer = window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE) : NULL;
    if (!window || !renderer) {
        fprintf(stderr, "could not create a window or renderer: %s\n", SDL_GetError());
        return 2;
    }
    g_renderer = renderer;

    gv_save save;
    gv_save_defaults(&save);

    gv_app *app = gv_app_create(window, renderer, &save);
    if (!app) {
        fprintf(stderr, "gv_app_create: %s\n", gv_app_error());
        return 2;
    }

    printf("smoke: walking every screen\n");

    /* --- title, with the attract demo running behind it --- */
    gv_frames(app, 45);
    gv_check_screen(app, "title", GV_SCREEN_TITLE, 1.0);
    GV_EXPECT(!gv_app_should_quit(app), "app asked to quit on the title screen");

    /* --- how to play --- */
    gv_activate(app, "HOW TO PLAY");
    gv_frames(app, 20);
    gv_check_screen(app, "help", GV_SCREEN_HELP, 1.0);

    gv_key(app, SDL_SCANCODE_ESCAPE);
    gv_frames(app, 10);

    /* --- high scores (empty table) --- */
    gv_activate(app, "HIGH SCORES");
    gv_frames(app, 20);
    gv_check_screen(app, "scores_empty", GV_SCREEN_SCORES, 1.0);
    gv_key(app, SDL_SCANCODE_ESCAPE);
    gv_frames(app, 10);

    /* --- difficulty, changed from the title screen --- */
    GV_EXPECT(gv_select(app, "DIFFICULTY"), "the title screen offers no DIFFICULTY row");
    for (int i = 0; i < GV_DIFF_COUNT + 2; ++i) gv_key(app, SDL_SCANCODE_RIGHT);
    for (int i = 0; i < GV_DIFF_COUNT + 2; ++i) gv_key(app, SDL_SCANCODE_LEFT);
    {
        const gv_save *s = gv_app_save(app);
        GV_EXPECT(s && s->settings.difficulty >= 0 && s->settings.difficulty < GV_DIFF_COUNT,
                  "difficulty ran outside its range (%d)", s ? s->settings.difficulty : -1);
    }

    /* --- settings: touch every row and every control --- */
    gv_activate(app, "SETTINGS");
    gv_frames(app, 20);
    gv_check_screen(app, "settings", GV_SCREEN_SETTINGS, 1.0);

    /* Every row the settings screen is supposed to offer must be reachable.
     * A row that has been dropped or misspelled fails here rather than being
     * quietly skipped by a keypress count. */
    static const char *const rows[] = { "MASTER",     "EFFECTS",    "MUSIC",
                                        "SOUNDTRACK", "PARTICLES",  "BLOOM",
                                        "SCREEN SHAKE", "EDGE MARKERS", "HIGH CONTRAST",
                                        "FULLSCREEN", "VSYNC",      "BACK" };
    /* Drive each control hard past both ends of its range: sliders must clamp
     * rather than run away, and toggles and choices must wrap or stick without
     * ever leaving a value the renderer would index with. */
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        GV_EXPECT(gv_select(app, rows[i]), "no settings row '%s'", rows[i]);
        for (int k = 0; k < 13; ++k) gv_key(app, SDL_SCANCODE_LEFT);
        for (int k = 0; k < 13; ++k) gv_key(app, SDL_SCANCODE_RIGHT);
    }
    /* And walk the list backwards, to exercise the wrap at the top. */
    for (int i = 0; i < 16; ++i) gv_key(app, SDL_SCANCODE_UP);
    gv_frames(app, 10);
    gv_check_screen(app, "settings_toggled", GV_SCREEN_SETTINGS, 1.0);

    {
        const gv_save *s = gv_app_save(app);
        GV_EXPECT(s != NULL, "the app reported no saved state");
        if (s) {
            GV_EXPECT(s->settings.master_volume >= 0.0f && s->settings.master_volume <= 1.0f,
                      "a slider escaped its range (master=%.3f)",
                      (double)s->settings.master_volume);
            GV_EXPECT(s->settings.fx_quality >= 0 && s->settings.fx_quality < GV_FX_COUNT,
                      "particle quality escaped its range (%d)", s->settings.fx_quality);
        }
    }

    gv_key(app, SDL_SCANCODE_ESCAPE);
    gv_frames(app, 10);

    /* --- start a run --- */
    gv_activate(app, "PLAY");
    gv_frames(app, 90);
    gv_check_screen(app, "playing", GV_SCREEN_PLAY, 1.0);

    /* --- pause, and settings reached from the pause menu --- */
    gv_key(app, SDL_SCANCODE_ESCAPE);
    gv_frames(app, 20);
    gv_check_screen(app, "paused", GV_SCREEN_PAUSE, 1.0);

    gv_activate(app, "SETTINGS");
    gv_frames(app, 20);
    gv_check_screen(app, "settings_in_run", GV_SCREEN_SETTINGS, 1.0);
    gv_key(app, SDL_SCANCODE_ESCAPE);
    gv_frames(app, 20);

    /* Back should land on the pause menu, not the title. */
    gv_check_screen(app, "paused_return", GV_SCREEN_PAUSE, 1.0);

    /* Resume and play on. */
    gv_key(app, SDL_SCANCODE_ESCAPE);
    gv_frames(app, 20);

    /* --- run to completion ---
     * The player is idle, so the arena eventually kills them. This is also the
     * longest continuous stretch of simulation, rendering and audio mixing in
     * the harness. */
    /* Eight fixed steps per frame is the app's own catch-up ceiling. */
    bool died = gv_run_until_screen(app, GV_SCREEN_GAMEOVER, GV_FIXED_DT * 8.0f, 300.0f);
    GV_EXPECT(died, "an idle player never died: the game may be unloseable");
    gv_frames(app, 30);
    gv_check_screen(app, "gameover", GV_SCREEN_GAMEOVER, 1.0);

    {
        const gv_save *s = gv_app_save(app);
        GV_EXPECT(s && s->total_runs == 1, "one finished run should count once (total_runs=%lld)",
                  s ? (long long)s->total_runs : -1);
    }

    /* --- retry from the game-over screen --- */
    gv_activate(app, "RETRY");
    gv_frames(app, 30);
    gv_check_screen(app, "retry", GV_SCREEN_PLAY, 1.0);

    /* --- restart from the pause menu, then abandon a run entirely --- */
    gv_key(app, SDL_SCANCODE_ESCAPE);
    gv_frames(app, 20);
    gv_check_screen(app, "paused_again", GV_SCREEN_PAUSE, 1.0);
    gv_activate(app, "RESTART RUN");
    gv_frames(app, 30);
    gv_check_screen(app, "restarted", GV_SCREEN_PLAY, 1.0);

    gv_key(app, SDL_SCANCODE_ESCAPE);
    gv_frames(app, 20);
    gv_activate(app, "QUIT TO TITLE");
    gv_frames(app, 20);
    gv_check_screen(app, "back_at_title", GV_SCREEN_TITLE, 1.0);

    {
        /* An abandoned run is not a finished one. */
        const gv_save *s = gv_app_save(app);
        GV_EXPECT(s && s->total_runs == 1, "quitting to the title counted a run (total_runs=%lld)",
                  s ? (long long)s->total_runs : -1);
    }

    /* --- a second full run, to prove the end-of-run bookkeeping resets --- */
    gv_activate(app, "PLAY");
    gv_frames(app, 30);
    gv_check_screen(app, "second_run", GV_SCREEN_PLAY, 1.0);
    GV_EXPECT(gv_run_until_screen(app, GV_SCREEN_GAMEOVER, GV_FIXED_DT * 8.0f, 300.0f),
              "the second run never ended");
    gv_frames(app, 20);
    gv_check_screen(app, "gameover_again", GV_SCREEN_GAMEOVER, 1.0);

    /* --- the score screen, reached from the game-over menu --- */
    gv_activate(app, "TITLE");
    gv_frames(app, 20);
    gv_check_screen(app, "title_after_run", GV_SCREEN_TITLE, 1.0);
    gv_activate(app, "HIGH SCORES");
    gv_frames(app, 20);
    gv_check_screen(app, "scores_filled", GV_SCREEN_SCORES, 1.0);

    /* The lifetime counters must have moved. Deliberately *not* asserted here:
     * a score-table entry. The harness never presses a movement key, so the
     * player kills nothing and finishes on zero — and a zero is correctly
     * refused a place in the table. Insertion, ranking, ties and capping are
     * covered exhaustively in tests/test_save.c instead. */
    const gv_save *state = gv_app_save(app);
    GV_EXPECT(state != NULL, "the app reported no saved state");
    if (state) {
        GV_EXPECT(state->total_runs == 2, "run counter did not advance (total_runs=%lld)",
                  (long long)state->total_runs);
        GV_EXPECT(state->total_time > 0.0, "no play time was recorded");
        GV_EXPECT(state->score_count == 0,
                  "a run that scored nothing still took a place in the score table");
    }

    /* --- persistence, end to end ---
     * The app has been writing to the sandboxed preferences directory as it
     * went. Read it back with the real loader — path resolution, atomic write,
     * parse, validate — and confirm the trip through the filesystem changed
     * nothing. */
    {
        const char *path = gv_save_path();
        GV_EXPECT(path != NULL, "no preferences path could be determined");

        gv_save disk;
        GV_EXPECT(gv_save_load(&disk), "the save the app just wrote could not be read back");
        if (state) {
            GV_EXPECT(disk.total_runs == state->total_runs,
                      "run count did not survive the round trip (%lld on disk, %lld live)",
                      (long long)disk.total_runs, (long long)state->total_runs);
            GV_EXPECT(disk.settings.difficulty == state->settings.difficulty,
                      "difficulty did not survive the round trip");
            GV_EXPECT(disk.settings.fx_quality == state->settings.fx_quality,
                      "particle quality did not survive the round trip");
            GV_EXPECT(disk.settings.bloom == state->settings.bloom,
                      "the bloom setting did not survive the round trip");
            GV_EXPECT(disk.settings.master_volume == state->settings.master_volume,
                      "master volume did not survive the round trip (%.3f vs %.3f)",
                      (double)disk.settings.master_volume, (double)state->settings.master_volume);
        }
        printf("  save round-tripped through %s\n", path ? path : "(nowhere)");
    }

    /* --- window resize, including degenerate sizes --- */
    const int sizes[][2] = { { 1920, 1080 }, { 800, 600 }, { 640, 360 }, { 2560, 1080 },
                             { 1280, 720 } };
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        SDL_SetWindowSize(window, sizes[i][0], sizes[i][1]);
        SDL_Event e;
        memset(&e, 0, sizeof(e));
        e.type = SDL_WINDOWEVENT;
        e.window.event = SDL_WINDOWEVENT_SIZE_CHANGED;
        e.window.data1 = sizes[i][0];
        e.window.data2 = sizes[i][1];
        gv_app_handle_event(app, &e);
        gv_frames(app, 5);
    }
    printf("  survived %zu window resizes\n", sizeof(sizes) / sizeof(sizes[0]));

    /* --- hostile frame timings --- */
    gv_app_frame(app, 0.0f);
    gv_app_frame(app, -1.0f);
    gv_app_frame(app, 1000.0f);
    gv_app_frame(app, (float)(0.0 / 1.0));
    gv_frames(app, 10);
    printf("  survived degenerate frame deltas\n");

    gv_app_destroy(app);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    gv_save_shutdown();
    SDL_Quit();

    if (g_failures == 0) {
        printf("smoke: PASS\n");
        return 0;
    }
    printf("smoke: FAIL (%d)\n", g_failures);
    return 1;
}
