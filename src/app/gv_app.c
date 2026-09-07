#include "gv_app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../audio/gv_audio.h"
#include "../game/gv_autopilot.h"
#include "../game/gv_world.h"
#include "../render/gv_gfx.h"
#include "../render/gv_hud.h"
#include "../render/gv_menu.h"
#include "../render/gv_scene.h"
#include "gv_input.h"

#define GV_APP_VERSION "1.0.0"

/* Menu item ids. */
enum {
    GV_ID_NONE = 0,
    GV_ID_PLAY,
    GV_ID_SETTINGS,
    GV_ID_SCORES,
    GV_ID_HELP,
    GV_ID_QUIT,
    GV_ID_RESUME,
    GV_ID_RESTART,
    GV_ID_TO_TITLE,
    GV_ID_BACK,
    GV_ID_RETRY
};

static const char *const k_difficulty_names[GV_DIFF_COUNT] = { "CADET", "PILOT", "ACE" };
static const char *const k_quality_names[GV_FX_COUNT] = { "LOW", "MEDIUM", "HIGH" };

struct gv_app {
    SDL_Window *window;
    SDL_Renderer *renderer;

    gv_gfx *gfx;
    gv_scene *scene;
    gv_audio *audio;
    gv_world *world;

    gv_hud_state hud;
    gv_input_state input;
    gv_save save;
    gv_menu menu;

    int screen;
    int settings_return; /* which screen to go back to from settings */
    float screen_time;
    float accumulator;

    bool quit;
    bool settings_dirty;
    bool score_submitted;
    int last_rank;

    /* Snapshot of the finished run, so the game-over screen keeps its numbers
     * after the world has been reset for the attract demo. */
    gv_score score_snapshot;
    int wave_snapshot;

    /* Attract mode. */
    gv_rng autopilot_jitter;
    float attract_restart;

    SDL_Rect viewport;
};

static char g_app_error[256];

const char *gv_app_error(void)
{
    return g_app_error;
}

/* ------------------------------------------------------------------ layout */

/* Fit the fixed 16:9 view into the window, centred, preserving aspect. */
static SDL_Rect gv_letterbox(int win_w, int win_h)
{
    SDL_Rect r = { 0, 0, GV_VIEW_W, GV_VIEW_H };
    if (win_w <= 0 || win_h <= 0) return r;

    double target = (double)GV_VIEW_W / (double)GV_VIEW_H;
    double actual = (double)win_w / (double)win_h;

    if (actual > target) {
        r.h = win_h;
        r.w = (int)((double)win_h * target + 0.5);
    } else {
        r.w = win_w;
        r.h = (int)((double)win_w / target + 0.5);
    }
    if (r.w < 1) r.w = 1;
    if (r.h < 1) r.h = 1;
    r.x = (win_w - r.w) / 2;
    r.y = (win_h - r.h) / 2;
    return r;
}

static void gv_app_update_viewport(gv_app *app)
{
    int w = 0, h = 0;
    SDL_GetRendererOutputSize(app->renderer, &w, &h);
    app->viewport = gv_letterbox(w, h);
    gv_input_set_viewport(&app->input, app->viewport);
}

/* ---------------------------------------------------------------- settings */

/* Push the current settings into every subsystem that mirrors them. */
static void gv_app_apply_settings(gv_app *app)
{
    const gv_settings *s = &app->save.settings;

    gv_audio_set_volumes(app->audio, s->master_volume, s->sfx_volume, s->music_volume);
    gv_audio_set_music_enabled(app->audio, s->music_enabled);
    gv_gfx_set_bloom(app->gfx, s->bloom, 0.85f);

    gv_scene_options opt;
    memset(&opt, 0, sizeof(opt));
    opt.screen_shake = s->screen_shake;
    opt.particle_density = gv_fx_quality_scale(s->fx_quality);
    opt.show_offscreen_markers = s->offscreen_markers;
    opt.high_contrast = s->high_contrast;
    gv_scene_set_options(app->scene, &opt);

    if (app->world) app->world->fx_quality = gv_fx_quality_scale(s->fx_quality);

    Uint32 flags = SDL_GetWindowFlags(app->window);
    bool is_fullscreen = (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
    if (is_fullscreen != s->fullscreen) {
        SDL_SetWindowFullscreen(app->window, s->fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
        gv_app_update_viewport(app);
    }
    /* Ignore failure: some drivers refuse to change vsync after creation, and
     * that is not worth interrupting the player over. */
    (void)SDL_RenderSetVSync(app->renderer, s->vsync ? 1 : 0);
}

void gv_app_persist(gv_app *app)
{
    if (!app) return;
    gv_save_write(&app->save);
    app->settings_dirty = false;
}

/* ------------------------------------------------------------------ menus */

static void gv_app_build_menu(gv_app *app)
{
    gv_menu *m = &app->menu;
    gv_menu_reset(m);

    switch (app->screen) {
    case GV_SCREEN_TITLE:
        gv_menu_add_action(m, GV_ID_PLAY, "PLAY");
        gv_menu_add_choice(m, GV_ID_NONE, "DIFFICULTY", &app->save.settings.difficulty,
                           k_difficulty_names, GV_DIFF_COUNT);
        gv_menu_add_gap(m);
        gv_menu_add_action(m, GV_ID_HELP, "HOW TO PLAY");
        gv_menu_add_action(m, GV_ID_SCORES, "HIGH SCORES");
        gv_menu_add_action(m, GV_ID_SETTINGS, "SETTINGS");
        gv_menu_add_action(m, GV_ID_QUIT, "QUIT");
        break;

    case GV_SCREEN_PAUSE:
        gv_menu_add_action(m, GV_ID_RESUME, "RESUME");
        gv_menu_add_action(m, GV_ID_RESTART, "RESTART RUN");
        gv_menu_add_gap(m);
        gv_menu_add_action(m, GV_ID_SETTINGS, "SETTINGS");
        gv_menu_add_action(m, GV_ID_TO_TITLE, "QUIT TO TITLE");
        break;

    case GV_SCREEN_SETTINGS:
        gv_menu_add_slider(m, GV_ID_NONE, "MASTER", &app->save.settings.master_volume, 0.1f);
        gv_menu_add_slider(m, GV_ID_NONE, "EFFECTS", &app->save.settings.sfx_volume, 0.1f);
        gv_menu_add_slider(m, GV_ID_NONE, "MUSIC", &app->save.settings.music_volume, 0.1f);
        gv_menu_add_toggle(m, GV_ID_NONE, "SOUNDTRACK", &app->save.settings.music_enabled);
        gv_menu_add_gap(m);
        gv_menu_add_choice(m, GV_ID_NONE, "PARTICLES", &app->save.settings.fx_quality,
                           k_quality_names, GV_FX_COUNT);
        gv_menu_add_toggle(m, GV_ID_NONE, "BLOOM", &app->save.settings.bloom);
        gv_menu_add_toggle(m, GV_ID_NONE, "SCREEN SHAKE", &app->save.settings.screen_shake);
        gv_menu_add_toggle(m, GV_ID_NONE, "EDGE MARKERS", &app->save.settings.offscreen_markers);
        gv_menu_add_toggle(m, GV_ID_NONE, "HIGH CONTRAST", &app->save.settings.high_contrast);
        gv_menu_add_gap(m);
        gv_menu_add_toggle(m, GV_ID_NONE, "FULLSCREEN", &app->save.settings.fullscreen);
        gv_menu_add_toggle(m, GV_ID_NONE, "VSYNC", &app->save.settings.vsync);
        gv_menu_add_gap(m);
        gv_menu_add_action(m, GV_ID_BACK, "BACK");
        break;

    case GV_SCREEN_SCORES:
    case GV_SCREEN_HELP:
        gv_menu_add_action(m, GV_ID_BACK, "BACK");
        break;

    case GV_SCREEN_GAMEOVER:
        gv_menu_add_action(m, GV_ID_RETRY, "RETRY");
        gv_menu_add_action(m, GV_ID_TO_TITLE, "TITLE");
        break;

    default:
        break;
    }
    m->selected = 0;
}

static void gv_app_set_screen(gv_app *app, int screen)
{
    app->screen = screen;
    app->screen_time = 0.0f;
    gv_app_build_menu(app);
}

/* -------------------------------------------------------------------- runs */

static uint64_t gv_app_fresh_seed(void)
{
    /* Two independent clocks, so a run started immediately after launch is
     * still unpredictable. */
    return (uint64_t)SDL_GetPerformanceCounter() ^ ((uint64_t)time(NULL) << 21);
}

static void gv_app_start_run(gv_app *app)
{
    gv_world_init(app->world, gv_app_fresh_seed(), app->save.settings.difficulty);
    app->world->fx_quality = gv_fx_quality_scale(app->save.settings.fx_quality);
    gv_scene_reset(app->scene, app->world);
    gv_hud_init(&app->hud);
    gv_hud_set_best(&app->hud, gv_save_best_score(&app->save, app->save.settings.difficulty));
    app->accumulator = 0.0f;
    app->score_submitted = false;
    app->last_rank = -1;
    gv_app_set_screen(app, GV_SCREEN_PLAY);
}

static void gv_app_start_attract(gv_app *app)
{
    gv_world_init(app->world, gv_app_fresh_seed(), GV_DIFF_PILOT);
    app->world->fx_quality = gv_fx_quality_scale(app->save.settings.fx_quality);
    gv_scene_reset(app->scene, app->world);
    gv_rng_seed(&app->autopilot_jitter, gv_app_fresh_seed(), 5u);
    app->accumulator = 0.0f;
    app->attract_restart = 0.0f;
}

/* ------------------------------------------------------------------ create */

gv_app *gv_app_create(SDL_Window *window, SDL_Renderer *renderer, const gv_save *save)
{
    if (!window || !renderer) {
        snprintf(g_app_error, sizeof(g_app_error), "no window or renderer");
        return NULL;
    }

    gv_app *app = (gv_app *)calloc(1, sizeof(gv_app));
    if (!app) {
        snprintf(g_app_error, sizeof(g_app_error), "out of memory");
        return NULL;
    }

    app->window = window;
    app->renderer = renderer;
    if (save) {
        app->save = *save;
    } else {
        gv_save_defaults(&app->save);
    }

    app->world = (gv_world *)malloc(sizeof(gv_world));
    app->gfx = gv_gfx_create(renderer, GV_VIEW_W, GV_VIEW_H);
    app->scene = gv_scene_create(gv_app_fresh_seed());
    app->audio = gv_audio_create();

    if (!app->world || !app->gfx || !app->scene) {
        snprintf(g_app_error, sizeof(g_app_error), "%s",
                 app->gfx ? "out of memory" : gv_gfx_error());
        gv_app_destroy(app);
        return NULL;
    }

    gv_input_init(&app->input);
    gv_hud_init(&app->hud);
    gv_app_update_viewport(app);
    gv_app_apply_settings(app);

    gv_app_start_attract(app);
    gv_app_set_screen(app, GV_SCREEN_TITLE);
    return app;
}

void gv_app_destroy(gv_app *app)
{
    if (!app) return;
    gv_input_shutdown(&app->input);
    gv_audio_destroy(app->audio);
    gv_scene_destroy(app->scene);
    gv_gfx_destroy(app->gfx);
    free(app->world);
    free(app);
}

bool gv_app_should_quit(const gv_app *app)
{
    return app && app->quit;
}

int gv_app_screen(const gv_app *app)
{
    return app ? app->screen : GV_SCREEN_TITLE;
}

const gv_save *gv_app_save(const gv_app *app)
{
    return app ? &app->save : NULL;
}

const char *gv_app_menu_selection(const gv_app *app)
{
    if (!app) return NULL;
    const gv_menu *m = &app->menu;
    if (m->count <= 0 || m->selected < 0 || m->selected >= m->count) return NULL;
    return m->items[m->selected].label;
}

const char *gv_screen_name(int screen)
{
    switch (screen) {
    case GV_SCREEN_TITLE: return "TITLE";
    case GV_SCREEN_PLAY: return "PLAY";
    case GV_SCREEN_PAUSE: return "PAUSE";
    case GV_SCREEN_SETTINGS: return "SETTINGS";
    case GV_SCREEN_SCORES: return "SCORES";
    case GV_SCREEN_HELP: return "HELP";
    case GV_SCREEN_GAMEOVER: return "GAMEOVER";
    default: return "?";
    }
}

/* ------------------------------------------------------------------ events */

void gv_app_handle_event(gv_app *app, const SDL_Event *e)
{
    if (!app || !e) return;

    switch (e->type) {
    case SDL_QUIT:
        app->quit = true;
        return;

    case SDL_WINDOWEVENT:
        if (e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
            e->window.event == SDL_WINDOWEVENT_RESIZED) {
            gv_app_update_viewport(app);
        } else if (e->window.event == SDL_WINDOWEVENT_FOCUS_LOST &&
                   app->screen == GV_SCREEN_PLAY) {
            /* Losing focus mid-run pauses rather than letting the player be
             * killed by something they could not see. */
            gv_app_set_screen(app, GV_SCREEN_PAUSE);
            gv_audio_ui(app->audio, GV_UI_BACK);
        }
        break;

    case SDL_KEYDOWN:
        if (e->key.repeat) break;
        /* Fullscreen toggle works from anywhere, including mid-run. */
        if (e->key.keysym.scancode == SDL_SCANCODE_F11 ||
            ((e->key.keysym.mod & (KMOD_GUI | KMOD_ALT)) &&
             e->key.keysym.scancode == SDL_SCANCODE_RETURN)) {
            app->save.settings.fullscreen = !app->save.settings.fullscreen;
            gv_app_apply_settings(app);
            app->settings_dirty = true;
            return;
        }
        break;

    default:
        break;
    }

    gv_input_handle_event(&app->input, e);
}

/* ------------------------------------------------------------- navigation */

static void gv_app_activate(gv_app *app, int id)
{
    switch (id) {
    case GV_ID_PLAY:
        gv_audio_ui(app->audio, GV_UI_START);
        gv_app_start_run(app);
        break;

    case GV_ID_SETTINGS:
        gv_audio_ui(app->audio, GV_UI_CONFIRM);
        app->settings_return = app->screen;
        gv_app_set_screen(app, GV_SCREEN_SETTINGS);
        break;

    case GV_ID_SCORES:
        gv_audio_ui(app->audio, GV_UI_CONFIRM);
        gv_app_set_screen(app, GV_SCREEN_SCORES);
        break;

    case GV_ID_HELP:
        gv_audio_ui(app->audio, GV_UI_CONFIRM);
        gv_app_set_screen(app, GV_SCREEN_HELP);
        break;

    case GV_ID_QUIT:
        gv_audio_ui(app->audio, GV_UI_BACK);
        app->quit = true;
        break;

    case GV_ID_RESUME:
        gv_audio_ui(app->audio, GV_UI_BACK);
        gv_app_set_screen(app, GV_SCREEN_PLAY);
        break;

    case GV_ID_RESTART:
    case GV_ID_RETRY:
        gv_audio_ui(app->audio, GV_UI_START);
        gv_app_start_run(app);
        break;

    case GV_ID_TO_TITLE:
        gv_audio_ui(app->audio, GV_UI_BACK);
        gv_app_start_attract(app);
        gv_app_set_screen(app, GV_SCREEN_TITLE);
        break;

    case GV_ID_BACK:
        gv_audio_ui(app->audio, GV_UI_BACK);
        if (app->screen == GV_SCREEN_SETTINGS) {
            gv_app_persist(app);
            gv_app_set_screen(app, app->settings_return == GV_SCREEN_PAUSE ? GV_SCREEN_PAUSE
                                                                          : GV_SCREEN_TITLE);
        } else {
            gv_app_set_screen(app, GV_SCREEN_TITLE);
        }
        break;

    default:
        break;
    }
}

static void gv_app_navigate(gv_app *app)
{
    gv_nav_action nav;
    while ((nav = gv_input_next_nav(&app->input)) != GV_NAV_NONE) {
        /* Escape has a screen-specific meaning that the menu never sees. */
        if (nav == GV_NAV_BACK) {
            switch (app->screen) {
            case GV_SCREEN_PLAY:
                gv_audio_ui(app->audio, GV_UI_BACK);
                gv_app_set_screen(app, GV_SCREEN_PAUSE);
                continue;
            case GV_SCREEN_PAUSE:
                gv_audio_ui(app->audio, GV_UI_BACK);
                gv_app_set_screen(app, GV_SCREEN_PLAY);
                continue;
            case GV_SCREEN_SETTINGS:
            case GV_SCREEN_SCORES:
            case GV_SCREEN_HELP:
                gv_app_activate(app, GV_ID_BACK);
                continue;
            case GV_SCREEN_GAMEOVER:
                gv_app_activate(app, GV_ID_TO_TITLE);
                continue;
            case GV_SCREEN_TITLE:
            default:
                continue;
            }
        }

        if (app->screen == GV_SCREEN_PLAY) continue;

        bool changed = false, moved = false;
        int id = gv_menu_handle(&app->menu, nav, &changed, &moved);
        if (moved) gv_audio_ui(app->audio, GV_UI_MOVE);
        if (changed) {
            gv_audio_ui(app->audio, GV_UI_MOVE);
            gv_app_apply_settings(app);
            app->settings_dirty = true;
        }
        if (id > 0) gv_app_activate(app, id);
    }
}

/* ------------------------------------------------------------- simulation */

static void gv_app_step_world(gv_app *app, float real_dt, bool autopilot)
{
    gv_world *w = app->world;

    /* Focus slows the world by feeding the accumulator less time, which keeps
     * the simulation on its fixed step while still producing real slow motion. */
    float scaled = real_dt * gv_clampf(w->time_scale, 0.05f, 1.0f);
    app->accumulator += scaled;

    int steps = 0;
    while (app->accumulator >= GV_FIXED_DT && steps < GV_MAX_STEPS_PER_FRAME) {
        gv_input in;
        if (autopilot) {
            in = gv_autopilot_think(w, &app->autopilot_jitter);
        } else {
            in = gv_input_build(&app->input, app->gfx, w);
        }

        gv_world_step(w, &in, GV_FIXED_DT);
        gv_audio_handle_events(app->audio, w, app->scene ? gv_gfx_camera(app->gfx)->pos : w->player.pos,
                               (float)GV_VIEW_W * 0.5f);
        gv_world_clear_events(w);

        app->accumulator -= GV_FIXED_DT;
        steps++;
    }

    /* If the loop hit its ceiling the process was stalled (a window drag, a
     * breakpoint). Drop the backlog rather than fast-forwarding through it. */
    if (steps >= GV_MAX_STEPS_PER_FRAME) app->accumulator = 0.0f;
}

/* --------------------------------------------------------------- screens */

static const gv_color k_col_title = { 235, 250, 255, 255 };
static const gv_color k_col_soft = { 130, 158, 200, 255 };
static const gv_color k_col_accent = { 120, 240, 255, 255 };
static const gv_color k_col_gold = { 255, 214, 130, 255 };

static void gv_draw_logo(gv_gfx *g, float cx, float y, float t)
{
    /* A slow breathing glow keeps the title alive without animating the
     * letterforms themselves, which would only make them harder to read. */
    float pulse = 0.85f + 0.15f * sinf(t * 1.6f);
    gv_draw_text_centred(g, cx, y, 82.0f, gv_fade(k_col_accent, 0.32f * pulse), 2.4f, "GRAVITON");
    gv_draw_text_centred(g, cx, y, 82.0f, k_col_title, 1.6f, "GRAVITON");
    gv_draw_text_centred(g, cx, y + 100.0f, 15.0f, gv_fade(k_col_soft, 0.9f), 0.5f,
                         "SWING . SLAM . SURVIVE");
}

static void gv_draw_title(gv_app *app, gv_gfx *g)
{
    const float cx = (float)GV_VIEW_W * 0.5f;

    gv_draw_dim(g, 0.68f);
    gv_draw_logo(g, cx, 68.0f, app->screen_time);
    gv_menu_draw(&app->menu, g, cx, 268.0f, 40.0f, 22.0f);

    char buf[64];
    int64_t best = gv_save_best_score(&app->save, -1);
    if (best > 0) {
        char n[48];
        gv_format_number(n, sizeof(n), best);
        snprintf(buf, sizeof(buf), "BEST %s", n);
        gv_draw_text_centred(g, cx, (float)GV_VIEW_H - 96.0f, 16.0f, gv_fade(k_col_gold, 0.9f),
                             0.6f, buf);
    }
    if (app->save.total_runs > 0) {
        snprintf(buf, sizeof(buf), "%lld RUNS . WAVE %d REACHED", (long long)app->save.total_runs,
                 app->save.best_wave);
        gv_draw_text_centred(g, cx, (float)GV_VIEW_H - 72.0f, 12.0f, gv_fade(k_col_soft, 0.6f),
                             0.2f, buf);
    }

    gv_draw_text(g, 22.0f, (float)GV_VIEW_H - 30.0f, 11.0f, gv_fade(k_col_soft, 0.45f), 0.0f,
                 "GRAVITON " GV_APP_VERSION);
    gv_draw_text_right(g, (float)GV_VIEW_W - 22.0f, (float)GV_VIEW_H - 30.0f, 11.0f,
                       gv_fade(k_col_soft, 0.45f), 0.0f,
                       gv_input_has_pad(&app->input) ? "GAMEPAD READY" : "MOUSE + KEYBOARD");
}

static void gv_draw_pause(gv_app *app, gv_gfx *g)
{
    const float cx = (float)GV_VIEW_W * 0.5f;
    gv_draw_dim(g, 0.72f);
    gv_draw_text_centred(g, cx, 150.0f, 46.0f, k_col_title, 1.3f, "PAUSED");
    gv_menu_draw(&app->menu, g, cx, 268.0f, 42.0f, 22.0f);
}

static void gv_draw_settings(gv_app *app, gv_gfx *g)
{
    const float cx = (float)GV_VIEW_W * 0.5f;
    gv_draw_dim(g, 0.80f);
    gv_draw_text_centred(g, cx, 54.0f, 38.0f, k_col_title, 1.2f, "SETTINGS");
    gv_menu_draw(&app->menu, g, cx, 132.0f, 34.0f, 18.0f);

    if (!gv_gfx_has_bloom(g)) {
        gv_draw_text_centred(g, cx, (float)GV_VIEW_H - 52.0f, 12.0f, gv_fade(k_col_soft, 0.6f),
                             0.2f, "BLOOM UNAVAILABLE ON THIS RENDERER");
    } else if (!gv_audio_active(app->audio)) {
        gv_draw_text_centred(g, cx, (float)GV_VIEW_H - 52.0f, 12.0f, gv_fade(k_col_soft, 0.6f),
                             0.2f, "NO AUDIO DEVICE");
    }
}

static void gv_draw_scores(gv_app *app, gv_gfx *g)
{
    const float cx = (float)GV_VIEW_W * 0.5f;
    gv_draw_dim(g, 0.82f);
    gv_draw_text_centred(g, cx, 54.0f, 38.0f, k_col_title, 1.2f, "HIGH SCORES");

    float y = 140.0f;
    int count = gv_clampi(app->save.score_count, 0, GV_MAX_SCORES);

    if (count == 0) {
        gv_draw_text_centred(g, cx, y + 40.0f, 18.0f, gv_fade(k_col_soft, 0.7f), 0.3f,
                             "NO RUNS RECORDED YET");
    } else {
        /* Column headers make the difficulty column legible at a glance. */
        gv_draw_text(g, cx - 300.0f, y, 12.0f, gv_fade(k_col_soft, 0.55f), 0.0f, "RANK");
        gv_draw_text(g, cx - 210.0f, y, 12.0f, gv_fade(k_col_soft, 0.55f), 0.0f, "SCORE");
        gv_draw_text(g, cx + 40.0f, y, 12.0f, gv_fade(k_col_soft, 0.55f), 0.0f, "WAVE");
        gv_draw_text(g, cx + 150.0f, y, 12.0f, gv_fade(k_col_soft, 0.55f), 0.0f, "MODE");
        y += 26.0f;

        for (int i = 0; i < count; ++i) {
            const gv_score_entry *e = &app->save.scores[i];
            /* Highlight the entry the player just set. */
            bool fresh = (i == app->last_rank);
            gv_color c = fresh ? k_col_gold : gv_color_lerp(k_col_title, k_col_soft,
                                                            (float)i / (float)GV_MAX_SCORES);
            char buf[48], num[48];

            snprintf(buf, sizeof(buf), "%d", i + 1);
            gv_draw_text(g, cx - 300.0f, y, 20.0f, gv_fade(c, 0.7f), fresh ? 1.0f : 0.4f, buf);

            gv_format_number(num, sizeof(num), e->score);
            gv_draw_text(g, cx - 210.0f, y, 20.0f, c, fresh ? 1.2f : 0.6f, num);

            snprintf(buf, sizeof(buf), "%d", e->wave);
            gv_draw_text(g, cx + 40.0f, y, 20.0f, gv_fade(c, 0.85f), 0.4f, buf);

            gv_draw_text(g, cx + 150.0f, y, 17.0f, gv_fade(c, 0.7f), 0.3f,
                         gv_difficulty_name(e->difficulty));
            y += 34.0f;
        }
    }

    gv_menu_draw(&app->menu, g, cx, (float)GV_VIEW_H - 84.0f, 34.0f, 20.0f);
}

static void gv_draw_help(gv_app *app, gv_gfx *g)
{
    const float cx = (float)GV_VIEW_W * 0.5f;
    gv_draw_dim(g, 0.86f);
    gv_draw_text_centred(g, cx, 40.0f, 34.0f, k_col_title, 1.2f, "HOW TO PLAY");

    /* The one rule everything else hangs off, stated first and plainly. */
    gv_draw_text_centred(g, cx, 92.0f, 19.0f, k_col_gold, 1.0f,
                         "SPEED IS YOUR ONLY WEAPON");
    gv_draw_text_centred(g, cx, 120.0f, 14.0f, gv_fade(k_col_soft, 0.95f), 0.3f,
                         "ABOVE THE OVERDRIVE LINE YOU DESTROY WHAT YOU HIT.");
    gv_draw_text_centred(g, cx, 140.0f, 14.0f, gv_fade(k_col_soft, 0.95f), 0.3f,
                         "BELOW IT, WHAT YOU HIT DESTROYS YOU.");

    struct {
        const char *key;
        const char *what;
    } rows[] = {
        { "WASD / ARROWS", "THRUST" },
        { "MOUSE", "AIM" },
        { "LEFT MOUSE (HOLD)", "FIRE AND HOLD TETHER" },
        { "MOUSE WHEEL / Q E", "REEL IN AND OUT" },
        { "RIGHT MOUSE / SPACE", "PULSE - SHOVE AND CLEAR SHOTS" },
        { "SHIFT (HOLD)", "FOCUS - SLOW TIME TO AIM" },
        { "ESC", "PAUSE" },
    };

    float y = 186.0f;
    gv_draw_text(g, cx - 320.0f, y, 13.0f, gv_fade(k_col_accent, 0.8f), 0.3f, "CONTROLS");
    y += 26.0f;
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        gv_draw_text_right(g, cx - 40.0f, y, 15.0f, gv_fade(k_col_title, 0.9f), 0.4f, rows[i].key);
        gv_draw_text(g, cx - 10.0f, y, 15.0f, gv_fade(k_col_soft, 0.9f), 0.2f, rows[i].what);
        y += 26.0f;
    }

    y += 14.0f;
    gv_draw_text(g, cx - 320.0f, y, 13.0f, gv_fade(k_col_accent, 0.8f), 0.3f, "THE LOOP");
    y += 24.0f;
    const char *loop[] = {
        "ANCHOR THE TETHER TO A PYLON OR A ROCK, THEN SWING.",
        "REEL IN WHILE SWINGING TO TRADE RADIUS FOR SPEED.",
        "RELEASE AND RAM. CHAIN KILLS TO CLIMB THE MULTIPLIER.",
        "A GOLD RING MEANS YOU ARE FAST ENOUGH TO BREAK THAT TARGET.",
        "WHIP A ROCK INTO SOMETHING FOR DOUBLE SCORE.",
    };
    for (size_t i = 0; i < sizeof(loop) / sizeof(loop[0]); ++i) {
        gv_draw_text_centred(g, cx, y, 14.0f, gv_fade(k_col_soft, 0.9f), 0.2f, loop[i]);
        y += 22.0f;
    }

    if (gv_input_has_pad(&app->input)) {
        gv_draw_text_centred(g, cx, y + 8.0f, 13.0f, gv_fade(k_col_accent, 0.75f), 0.3f,
                             "GAMEPAD: STICKS MOVE AND AIM, RT TETHER, LT FOCUS, A PULSE");
    }

    gv_menu_draw(&app->menu, g, cx, (float)GV_VIEW_H - 56.0f, 32.0f, 20.0f);
}

static void gv_draw_gameover(gv_app *app, gv_gfx *g)
{
    const float cx = (float)GV_VIEW_W * 0.5f;
    const gv_score *s = &app->score_snapshot;

    gv_draw_dim(g, 0.78f);

    /* Slide and fade in, so the screen arrives rather than appearing. */
    float intro = gv_clampf(app->screen_time * 2.6f, 0.0f, 1.0f);
    float slide = (1.0f - intro) * 26.0f;

    gv_draw_text_centred(g, cx, 62.0f - slide, 40.0f, gv_fade(k_col_title, intro), 1.2f,
                         "RUN COMPLETE");

    if (app->last_rank == 0) {
        float pulse = 0.6f + 0.4f * sinf(app->screen_time * 6.0f);
        gv_draw_text_centred(g, cx, 112.0f, 22.0f, gv_fade(k_col_gold, intro * pulse), 1.4f,
                             "NEW PERSONAL BEST");
    } else if (app->last_rank > 0) {
        char buf[48];
        snprintf(buf, sizeof(buf), "RANKED #%d", app->last_rank + 1);
        gv_draw_text_centred(g, cx, 112.0f, 18.0f, gv_fade(k_col_accent, intro), 0.8f, buf);
    }

    char num[48];
    gv_format_number(num, sizeof(num), s->score);
    gv_draw_text_centred(g, cx, 156.0f, 58.0f, gv_fade(k_col_title, intro), 1.5f, num);

    struct {
        const char *label;
        char value[32];
    } stats[6];
    snprintf(stats[0].value, sizeof(stats[0].value), "%d", app->wave_snapshot);
    stats[0].label = "WAVE REACHED";
    snprintf(stats[1].value, sizeof(stats[1].value), "%d", s->kills);
    stats[1].label = "DESTROYED";
    snprintf(stats[2].value, sizeof(stats[2].value), "%d", s->best_combo);
    stats[2].label = "BEST CHAIN";
    snprintf(stats[3].value, sizeof(stats[3].value), "%d", s->whip_kills);
    stats[3].label = "ROCK KILLS";
    snprintf(stats[4].value, sizeof(stats[4].value), "%d:%02d", (int)(s->time_alive / 60.0f),
             (int)s->time_alive % 60);
    stats[4].label = "TIME";
    snprintf(stats[5].value, sizeof(stats[5].value), "%s",
             gv_difficulty_name(app->save.settings.difficulty));
    stats[5].label = "MODE";

    float y = 246.0f;
    for (int i = 0; i < 6; ++i) {
        /* Stagger each row's fade so the table assembles itself. */
        float row = gv_clampf((app->screen_time - 0.15f - (float)i * 0.07f) * 4.0f, 0.0f, 1.0f);
        gv_draw_text_right(g, cx - 20.0f, y, 15.0f, gv_fade(k_col_soft, row * 0.85f), 0.2f,
                           stats[i].label);
        gv_draw_text(g, cx + 20.0f, y, 15.0f, gv_fade(k_col_title, row), 0.5f, stats[i].value);
        y += 26.0f;
    }

    gv_menu_draw(&app->menu, g, cx, (float)GV_VIEW_H - 112.0f, 38.0f, 22.0f);
}

/* -------------------------------------------------------------------- frame */

void gv_app_frame(gv_app *app, float real_dt)
{
    if (!app) return;
    if (!isfinite(real_dt) || real_dt < 0.0f) real_dt = 0.0f;
    real_dt = gv_minf(real_dt, 0.1f);

    /* Pump the OS queue here rather than in main, so the reset-then-poll
     * ordering that the input mapper depends on can never be got wrong by a
     * caller. */
    gv_input_begin_frame(&app->input);
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        gv_app_handle_event(app, &event);
    }

    app->screen_time += real_dt;
    gv_app_navigate(app);
    gv_menu_update(&app->menu, real_dt);

    bool in_menu = (app->screen != GV_SCREEN_PLAY);
    bool attract = (app->screen == GV_SCREEN_TITLE || app->screen == GV_SCREEN_SCORES ||
                    app->screen == GV_SCREEN_HELP || app->screen == GV_SCREEN_SETTINGS);

    /* The world keeps running behind the title and its sub-screens, driven by
     * the autopilot: the menu sits over a live game rather than a still. */
    if (app->screen == GV_SCREEN_PLAY || attract) {
        gv_app_step_world(app, real_dt, attract);
        gv_scene_update(app->scene, app->world, real_dt);
    }

    if (attract && gv_world_is_over(app->world)) {
        /* Restart the demo shortly after the autopilot dies. */
        app->attract_restart += real_dt;
        if (app->attract_restart > 2.0f) gv_app_start_attract(app);
    }

    if (app->screen == GV_SCREEN_PLAY) {
        gv_hud_update(&app->hud, app->world, real_dt);

        if (gv_world_is_over(app->world) && !app->score_submitted) {
            app->score_submitted = true;
            app->score_snapshot = app->world->score;
            /* The director sits on wave 0 during the opening intro; a run that
             * started reached wave one, so report and record it that way. */
            app->wave_snapshot = gv_maxi(app->world->wave.index, 1);
            app->save.total_kills += app->world->score.kills;
            app->save.total_time += (double)app->world->score.time_alive;
            app->last_rank = gv_save_submit_score(&app->save, app->world->score.score,
                                                  app->wave_snapshot,
                                                  app->save.settings.difficulty, (int64_t)time(NULL));
            gv_app_persist(app);
            gv_app_set_screen(app, GV_SCREEN_GAMEOVER);
        }
    }

    gv_audio_update(app->audio, app->world, real_dt, in_menu);

    /* ---- draw ---- */
    SDL_SetRenderDrawBlendMode(app->renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(app->renderer, 0, 0, 0, 255);
    SDL_RenderClear(app->renderer);

    gv_gfx_begin_scene(app->gfx);

    float lag = gv_clampf(app->accumulator, 0.0f, GV_FIXED_DT);
    gv_scene_draw(app->scene, app->gfx, app->world, lag);

    switch (app->screen) {
    case GV_SCREEN_PLAY:
        gv_hud_draw(&app->hud, app->gfx, app->world);
        break;
    case GV_SCREEN_PAUSE:
        gv_hud_draw(&app->hud, app->gfx, app->world);
        gv_draw_pause(app, app->gfx);
        break;
    case GV_SCREEN_TITLE:
        gv_draw_title(app, app->gfx);
        break;
    case GV_SCREEN_SETTINGS:
        gv_draw_settings(app, app->gfx);
        break;
    case GV_SCREEN_SCORES:
        gv_draw_scores(app, app->gfx);
        break;
    case GV_SCREEN_HELP:
        gv_draw_help(app, app->gfx);
        break;
    case GV_SCREEN_GAMEOVER:
        gv_draw_gameover(app, app->gfx);
        break;
    default:
        break;
    }

    gv_gfx_end_scene(app->gfx, &app->viewport);
    SDL_RenderPresent(app->renderer);
}
