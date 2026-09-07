/* gv_save.h — settings and high scores on disk.
 *
 * The file lives in the platform's preferences directory (on macOS that is
 * ~/Library/Application Support/Graviton/). It is a small, human-readable
 * key/value text file: easy to inspect, easy to delete, and — importantly —
 * easy to validate.
 *
 * Nothing read from disk is trusted. Every value is range-checked on load and
 * anything unparseable falls back to its default, because a corrupt or
 * hand-edited save must never be able to crash the game or produce an
 * unplayable configuration.
 */
#ifndef GV_SAVE_H
#define GV_SAVE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define GV_MAX_SCORES 8
#define GV_SAVE_VERSION 1

typedef enum {
    GV_FX_LOW = 0,
    GV_FX_MEDIUM,
    GV_FX_HIGH,
    GV_FX_COUNT
} gv_fx_quality;

typedef struct {
    int64_t score;
    int wave;
    int difficulty;
    int64_t timestamp; /* seconds since the epoch, 0 if unknown */
} gv_score_entry;

typedef struct {
    float master_volume;
    float sfx_volume;
    float music_volume;
    bool music_enabled;

    int difficulty;       /* gv_difficulty */
    int fx_quality;       /* gv_fx_quality */
    bool bloom;
    bool screen_shake;
    bool offscreen_markers;
    bool high_contrast;

    bool fullscreen;
    bool vsync;
} gv_settings;

typedef struct {
    gv_settings settings;
    gv_score_entry scores[GV_MAX_SCORES];
    int score_count;

    /* Lifetime statistics, shown on the title screen. */
    int64_t total_runs;
    int64_t total_kills;
    double total_time;
    int best_wave;
} gv_save;

/* Reset to a known-good configuration. Always safe. */
void gv_save_defaults(gv_save *s);

/* Load from disk. Returns false when nothing was loaded (missing file, no
 * writable preferences directory, or unreadable contents); `s` is left holding
 * valid defaults either way, so the caller can ignore the result. */
bool gv_save_load(gv_save *s);

/* Write atomically: a temporary file plus a rename, so an interrupted write
 * cannot leave a half-written save behind. Returns false on failure. */
bool gv_save_write(const gv_save *s);

/* Insert a finished run into the score table. Returns the 0-based rank it
 * landed at, or -1 if it did not place. */
int gv_save_submit_score(gv_save *s, int64_t score, int wave, int difficulty, int64_t when);

/* Best score recorded for a difficulty, or 0 if none. Pass -1 for any. */
int64_t gv_save_best_score(const gv_save *s, int difficulty);

/* Read/write the save format on an already-open stream. These carry the whole
 * format and all of its validation, and need no platform support, so the tests
 * can feed them hand-written and deliberately corrupt files. */
bool gv_save_read_stream(gv_save *s, FILE *f);
bool gv_save_write_stream(const gv_save *s, FILE *f);

/* Absolute path of the save file, or NULL if none could be determined.
 * The returned string is owned by this module. */
const char *gv_save_path(void);

/* Release the cached path. Called at shutdown so leak checkers stay quiet. */
void gv_save_shutdown(void);

/* Map the fx quality setting onto the world's particle density multiplier. */
float gv_fx_quality_scale(int quality);
const char *gv_fx_quality_name(int quality);

#endif /* GV_SAVE_H */
