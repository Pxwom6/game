/* gv_save.c — settings and score-table logic.
 *
 * Deliberately free of any SDL dependency. Everything here operates on plain
 * data or on a FILE stream the caller supplies, which is what lets the test
 * suite exercise the save format — including deliberately corrupt input —
 * without a window, a renderer or a preferences directory.
 *
 * Locating the file on disk lives in gv_save_path.c, which is the only part
 * that needs the platform.
 */
#include "gv_save.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/gv_math.h"
#include "../game/gv_world.h"

#define GV_LINE_MAX 256

float gv_fx_quality_scale(int quality)
{
    switch (quality) {
    case GV_FX_LOW: return 0.35f;
    case GV_FX_MEDIUM: return 0.68f;
    default: return 1.0f;
    }
}

const char *gv_fx_quality_name(int quality)
{
    switch (quality) {
    case GV_FX_LOW: return "LOW";
    case GV_FX_MEDIUM: return "MEDIUM";
    default: return "HIGH";
    }
}

void gv_save_defaults(gv_save *s)
{
    if (!s) return;
    memset(s, 0, sizeof(*s));

    s->settings.master_volume = 0.8f;
    s->settings.sfx_volume = 0.9f;
    s->settings.music_volume = 0.6f;
    s->settings.music_enabled = true;

    s->settings.difficulty = GV_DIFF_PILOT;
    s->settings.fx_quality = GV_FX_HIGH;
    s->settings.bloom = true;
    s->settings.screen_shake = true;
    s->settings.offscreen_markers = true;
    s->settings.high_contrast = false;

    s->settings.fullscreen = false;
    s->settings.vsync = true;
}

/* --------------------------------------------------------------- parsing */

/* Trim ASCII whitespace from both ends, in place. */
static char *gv_trim(char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    char *end = s + strlen(s);
    while (end > s) {
        char c = end[-1];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
        end--;
    }
    *end = '\0';
    return s;
}

static bool gv_parse_float(const char *v, float lo, float hi, float *out)
{
    char *end = NULL;
    double d = strtod(v, &end);
    if (end == v || !isfinite(d)) return false;
    *out = gv_clampf((float)d, lo, hi);
    return true;
}

static bool gv_parse_int(const char *v, int lo, int hi, int *out)
{
    char *end = NULL;
    long l = strtol(v, &end, 10);
    if (end == v) return false;
    if (l < (long)lo) l = lo;
    if (l > (long)hi) l = hi;
    *out = (int)l;
    return true;
}

static bool gv_parse_i64(const char *v, int64_t lo, int64_t hi, int64_t *out)
{
    char *end = NULL;
    long long l = strtoll(v, &end, 10);
    if (end == v) return false;
    if (l < (long long)lo) l = lo;
    if (l > (long long)hi) l = hi;
    *out = (int64_t)l;
    return true;
}

static bool gv_parse_bool(const char *v, bool *out)
{
    if (!v || !*v) return false;
    if (v[0] == '1' || v[0] == 't' || v[0] == 'T' || v[0] == 'y' || v[0] == 'Y') {
        *out = true;
        return true;
    }
    if (v[0] == '0' || v[0] == 'f' || v[0] == 'F' || v[0] == 'n' || v[0] == 'N') {
        *out = false;
        return true;
    }
    return false;
}

/* Cut the next ':'-delimited field out of *cursor, advancing it past the
 * separator. Returns NULL once the string is exhausted.
 *
 * Written out rather than using strtok_r because that is POSIX, not C11, and
 * this file is built as strict C11 — on both Darwin and glibc the declaration
 * is then hidden, and the implicit int-to-pointer conversion that follows
 * truncates the returned pointer on a 64-bit target. Empty fields also have to
 * be reported as empty rather than skipped, which strtok_r cannot do: "5::1:0"
 * is a malformed entry, not a three-field one.
 */
static char *gv_next_field(char **cursor)
{
    char *start = *cursor;
    if (!start) return NULL;

    char *sep = strchr(start, ':');
    if (sep) {
        *sep = '\0';
        *cursor = sep + 1;
    } else {
        *cursor = NULL;
    }
    return start;
}

/* A score line is "score:wave:difficulty:timestamp". Any malformed field
 * discards the whole entry rather than storing something half-parsed. */
static bool gv_parse_score(const char *v, gv_score_entry *e)
{
    char buf[GV_LINE_MAX];
    snprintf(buf, sizeof(buf), "%s", v);

    char *cursor = buf;
    char *tok = gv_next_field(&cursor);
    if (!tok || !gv_parse_i64(tok, 0, INT64_MAX, &e->score)) return false;

    tok = gv_next_field(&cursor);
    if (!tok || !gv_parse_int(tok, 0, 100000, &e->wave)) return false;

    tok = gv_next_field(&cursor);
    if (!tok || !gv_parse_int(tok, 0, GV_DIFF_COUNT - 1, &e->difficulty)) return false;

    /* The timestamp is the one optional field: an entry written by a build
     * that did not record it is still a valid score. */
    tok = gv_next_field(&cursor);
    if (!tok || !gv_parse_i64(tok, 0, INT64_MAX, &e->timestamp)) e->timestamp = 0;
    return true;
}

bool gv_save_read_stream(gv_save *s, FILE *f)
{
    if (!s) return false;
    gv_save_defaults(s);
    if (!f) return false;

    gv_settings *st = &s->settings;
    char line[GV_LINE_MAX];
    int line_count = 0;

    while (fgets(line, sizeof(line), f)) {
        /* Bound the work: a pathological file must not stall startup. */
        if (++line_count > 512) break;

        char *text = gv_trim(line);
        if (!*text || *text == '#') continue;

        char *eq = strchr(text, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = gv_trim(text);
        char *val = gv_trim(eq + 1);
        if (!*key || !*val) continue;

        /* Unknown keys are ignored on purpose, so a save written by a newer
         * build still loads in an older one. */
        if (strcmp(key, "master_volume") == 0) {
            gv_parse_float(val, 0.0f, 1.0f, &st->master_volume);
        } else if (strcmp(key, "sfx_volume") == 0) {
            gv_parse_float(val, 0.0f, 1.0f, &st->sfx_volume);
        } else if (strcmp(key, "music_volume") == 0) {
            gv_parse_float(val, 0.0f, 1.0f, &st->music_volume);
        } else if (strcmp(key, "music_enabled") == 0) {
            gv_parse_bool(val, &st->music_enabled);
        } else if (strcmp(key, "difficulty") == 0) {
            gv_parse_int(val, 0, GV_DIFF_COUNT - 1, &st->difficulty);
        } else if (strcmp(key, "fx_quality") == 0) {
            gv_parse_int(val, 0, GV_FX_COUNT - 1, &st->fx_quality);
        } else if (strcmp(key, "bloom") == 0) {
            gv_parse_bool(val, &st->bloom);
        } else if (strcmp(key, "screen_shake") == 0) {
            gv_parse_bool(val, &st->screen_shake);
        } else if (strcmp(key, "offscreen_markers") == 0) {
            gv_parse_bool(val, &st->offscreen_markers);
        } else if (strcmp(key, "high_contrast") == 0) {
            gv_parse_bool(val, &st->high_contrast);
        } else if (strcmp(key, "fullscreen") == 0) {
            gv_parse_bool(val, &st->fullscreen);
        } else if (strcmp(key, "vsync") == 0) {
            gv_parse_bool(val, &st->vsync);
        } else if (strcmp(key, "total_runs") == 0) {
            gv_parse_i64(val, 0, INT64_MAX, &s->total_runs);
        } else if (strcmp(key, "total_kills") == 0) {
            gv_parse_i64(val, 0, INT64_MAX, &s->total_kills);
        } else if (strcmp(key, "best_wave") == 0) {
            gv_parse_int(val, 0, 100000, &s->best_wave);
        } else if (strcmp(key, "total_time") == 0) {
            float t = 0.0f;
            if (gv_parse_float(val, 0.0f, 1e9f, &t)) s->total_time = (double)t;
        } else if (strcmp(key, "score") == 0) {
            if (s->score_count < GV_MAX_SCORES) {
                gv_score_entry e;
                memset(&e, 0, sizeof(e));
                if (gv_parse_score(val, &e)) s->scores[s->score_count++] = e;
            }
        }
    }

    /* The table is written sorted, but a hand-edited file might not be. */
    for (int i = 1; i < s->score_count; ++i) {
        gv_score_entry key = s->scores[i];
        int j = i - 1;
        while (j >= 0 && s->scores[j].score < key.score) {
            s->scores[j + 1] = s->scores[j];
            j--;
        }
        s->scores[j + 1] = key;
    }
    return true;
}

bool gv_save_write_stream(const gv_save *s, FILE *f)
{
    if (!s || !f) return false;

    const gv_settings *st = &s->settings;
    fprintf(f, "# GRAVITON settings and scores\n");
    fprintf(f, "version = %d\n", GV_SAVE_VERSION);
    fprintf(f, "master_volume = %.3f\n", (double)st->master_volume);
    fprintf(f, "sfx_volume = %.3f\n", (double)st->sfx_volume);
    fprintf(f, "music_volume = %.3f\n", (double)st->music_volume);
    fprintf(f, "music_enabled = %d\n", st->music_enabled ? 1 : 0);
    fprintf(f, "difficulty = %d\n", st->difficulty);
    fprintf(f, "fx_quality = %d\n", st->fx_quality);
    fprintf(f, "bloom = %d\n", st->bloom ? 1 : 0);
    fprintf(f, "screen_shake = %d\n", st->screen_shake ? 1 : 0);
    fprintf(f, "offscreen_markers = %d\n", st->offscreen_markers ? 1 : 0);
    fprintf(f, "high_contrast = %d\n", st->high_contrast ? 1 : 0);
    fprintf(f, "fullscreen = %d\n", st->fullscreen ? 1 : 0);
    fprintf(f, "vsync = %d\n", st->vsync ? 1 : 0);
    fprintf(f, "total_runs = %lld\n", (long long)s->total_runs);
    fprintf(f, "total_kills = %lld\n", (long long)s->total_kills);
    fprintf(f, "total_time = %.1f\n", s->total_time);
    fprintf(f, "best_wave = %d\n", s->best_wave);

    int count = gv_clampi(s->score_count, 0, GV_MAX_SCORES);
    for (int i = 0; i < count; ++i) {
        const gv_score_entry *e = &s->scores[i];
        fprintf(f, "score = %lld:%d:%d:%lld\n", (long long)e->score, e->wave, e->difficulty,
                (long long)e->timestamp);
    }
    return (fflush(f) == 0) && (ferror(f) == 0);
}

int gv_save_submit_score(gv_save *s, int64_t score, int wave, int difficulty, int64_t when)
{
    if (!s) return -1;

    s->total_runs++;
    if (wave > s->best_wave) s->best_wave = wave;

    if (score <= 0) return -1;

    int count = gv_clampi(s->score_count, 0, GV_MAX_SCORES);

    /* Find the insertion point; ties keep the existing entry ahead. */
    int rank = count;
    for (int i = 0; i < count; ++i) {
        if (score > s->scores[i].score) {
            rank = i;
            break;
        }
    }
    if (rank >= GV_MAX_SCORES) return -1;

    for (int i = gv_mini(count, GV_MAX_SCORES - 1); i > rank; --i) {
        s->scores[i] = s->scores[i - 1];
    }

    s->scores[rank].score = score;
    s->scores[rank].wave = wave;
    s->scores[rank].difficulty = gv_clampi(difficulty, 0, GV_DIFF_COUNT - 1);
    s->scores[rank].timestamp = when;

    if (count < GV_MAX_SCORES) s->score_count = count + 1;
    return rank;
}

int64_t gv_save_best_score(const gv_save *s, int difficulty)
{
    if (!s) return 0;
    int64_t best = 0;
    int count = gv_clampi(s->score_count, 0, GV_MAX_SCORES);
    for (int i = 0; i < count; ++i) {
        if (difficulty >= 0 && s->scores[i].difficulty != difficulty) continue;
        if (s->scores[i].score > best) best = s->scores[i].score;
    }
    return best;
}
