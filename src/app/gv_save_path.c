/* gv_save_path.c — where the save file lives, and reading/writing it there.
 *
 * The only part of persistence that needs the platform. Split out so that the
 * format and the score-table logic in gv_save.c stay testable without SDL.
 */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gv_save.h"

#define GV_SAVE_FILE "graviton.cfg"
#define GV_SAVE_TMP "graviton.cfg.tmp"

static char *g_save_dir;
static char *g_save_path;

static bool gv_ensure_paths(void)
{
    if (g_save_path) return true;

    /* An empty organisation keeps the macOS path at the conventional
     * ~/Library/Application Support/Graviton/ rather than nesting it. */
    char *dir = SDL_GetPrefPath("", "Graviton");
    if (!dir) return false;

    size_t dir_len = strlen(dir);
    size_t path_len = dir_len + strlen(GV_SAVE_FILE) + 1;
    char *path = (char *)malloc(path_len);
    if (!path) {
        SDL_free(dir);
        return false;
    }
    snprintf(path, path_len, "%s%s", dir, GV_SAVE_FILE);

    g_save_dir = dir;
    g_save_path = path;
    return true;
}

const char *gv_save_path(void)
{
    return gv_ensure_paths() ? g_save_path : NULL;
}

void gv_save_shutdown(void)
{
    if (g_save_dir) {
        SDL_free(g_save_dir);
        g_save_dir = NULL;
    }
    free(g_save_path);
    g_save_path = NULL;
}


bool gv_save_load(gv_save *s)
{
    if (!s) return false;
    gv_save_defaults(s);

    const char *path = gv_save_path();
    if (!path) return false;

    FILE *f = fopen(path, "rb");
    if (!f) return false;

    bool ok = gv_save_read_stream(s, f);
    fclose(f);
    return ok;
}

bool gv_save_write(const gv_save *s)
{
    if (!s || !gv_ensure_paths()) return false;

    /* Write to a sibling temporary file and rename over the real one, so a
     * crash or a full disk cannot destroy an existing save. */
    size_t tmp_len = strlen(g_save_dir) + strlen(GV_SAVE_TMP) + 1;
    char *tmp = (char *)malloc(tmp_len);
    if (!tmp) return false;
    snprintf(tmp, tmp_len, "%s%s", g_save_dir, GV_SAVE_TMP);

    FILE *f = fopen(tmp, "wb");
    if (!f) {
        free(tmp);
        return false;
    }

    bool ok = gv_save_write_stream(s, f);
    if (fclose(f) != 0) ok = false;

    if (ok) {
        /* rename() replaces atomically on POSIX; on Windows the target has to
         * be gone first, hence the remove(). */
        remove(g_save_path);
        ok = (rename(tmp, g_save_path) == 0);
    }
    if (!ok) remove(tmp);

    free(tmp);
    return ok;
}
