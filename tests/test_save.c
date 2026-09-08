/* test_save.c — the settings/score file.
 *
 * The save file is the one input to the game that a user can hand-edit, that a
 * disk can half-write, and that an older or newer build can have produced. It
 * is therefore the one input that is never trusted. These tests feed the parser
 * deliberately hostile files and assert the only two acceptable outcomes: the
 * value is used, or the default is. Never a crash, never a hang, never a
 * configuration the game cannot be played in.
 */
#include "../src/app/gv_save.h"
#include "../src/game/gv_types.h"
#include "gv_test.h"

/* A FILE the parser can read, holding exactly `text`. tmpfile() is used rather
 * than fmemopen() because it exists everywhere the game builds. */
static FILE *gv_stream_from(const char *text)
{
    FILE *f = tmpfile();
    if (!f) return NULL;
    if (text && *text) fwrite(text, 1, strlen(text), f);
    rewind(f);
    return f;
}

/* Parse `text` into `s`. Returns false only if the environment denied us a
 * temporary file, which the callers report rather than silently pass. */
static bool gv_parse_into(gv_save *s, const char *text)
{
    FILE *f = gv_stream_from(text);
    if (!f) return false;
    gv_save_read_stream(s, f);
    fclose(f);
    return true;
}

/* Every field within the range the game can actually run in. Any file, however
 * mangled, must leave the save in this state. */
static void gv_check_playable(const gv_save *s)
{
    const gv_settings *st = &s->settings;
    GV_CHECK(st->master_volume >= 0.0f && st->master_volume <= 1.0f);
    GV_CHECK(st->sfx_volume >= 0.0f && st->sfx_volume <= 1.0f);
    GV_CHECK(st->music_volume >= 0.0f && st->music_volume <= 1.0f);
    GV_CHECK_FINITE(st->master_volume);
    GV_CHECK_FINITE(st->sfx_volume);
    GV_CHECK_FINITE(st->music_volume);
    GV_CHECK(st->difficulty >= 0 && st->difficulty < GV_DIFF_COUNT);
    GV_CHECK(st->fx_quality >= 0 && st->fx_quality < GV_FX_COUNT);
    GV_CHECK(s->score_count >= 0 && s->score_count <= GV_MAX_SCORES);
    GV_CHECK(s->total_runs >= 0);
    GV_CHECK(s->total_kills >= 0);
    GV_CHECK(s->best_wave >= 0);
    GV_CHECK(s->total_time >= 0.0);

    for (int i = 0; i < s->score_count; ++i) {
        GV_CHECK(s->scores[i].score >= 0);
        GV_CHECK(s->scores[i].wave >= 0);
        GV_CHECK(s->scores[i].difficulty >= 0 && s->scores[i].difficulty < GV_DIFF_COUNT);
    }
    /* The table the rest of the game reads must be sorted, whatever the file
     * claimed, because the HUD and the score screen both index it directly. */
    for (int i = 1; i < s->score_count; ++i) {
        GV_CHECK(s->scores[i - 1].score >= s->scores[i].score);
    }
}

GV_TEST(save_defaults_are_playable)
{
    gv_save s;
    gv_save_defaults(&s);
    gv_check_playable(&s);

    /* A first run must start with sound on and nothing recorded. */
    GV_CHECK(s.settings.master_volume > 0.0f);
    GV_CHECK(s.settings.music_enabled);
    GV_CHECK(s.settings.bloom);
    GV_CHECK(s.settings.vsync);
    GV_CHECK(!s.settings.fullscreen);
    GV_CHECK_EQ_I(s.score_count, 0);
    GV_CHECK_EQ_I(s.total_runs, 0);
    GV_CHECK_EQ_I(gv_save_best_score(&s, -1), 0);

    /* gv_save_defaults must be callable on a struct full of junk. */
    gv_save junk;
    memset(&junk, 0xA5, sizeof(junk));
    gv_save_defaults(&junk);
    gv_check_playable(&junk);
}

GV_TEST(save_round_trips_through_a_stream)
{
    gv_save out;
    gv_save_defaults(&out);
    out.settings.master_volume = 0.25f;
    out.settings.sfx_volume = 0.0f;
    out.settings.music_volume = 1.0f;
    out.settings.music_enabled = false;
    out.settings.difficulty = GV_DIFF_ACE;
    out.settings.fx_quality = GV_FX_LOW;
    out.settings.bloom = false;
    out.settings.screen_shake = false;
    out.settings.offscreen_markers = false;
    out.settings.high_contrast = true;
    out.settings.fullscreen = true;
    out.settings.vsync = false;
    gv_save_submit_score(&out, 5000, 12, GV_DIFF_PILOT, 111);
    gv_save_submit_score(&out, 9000, 20, GV_DIFF_ACE, 222);
    gv_save_submit_score(&out, 100, 2, GV_DIFF_CADET, 333);
    /* Set the lifetime counters after submitting: submitting advances them. */
    out.total_runs = 41;
    out.total_kills = 9001;
    out.total_time = 1234.5;
    out.best_wave = 17;

    FILE *f = tmpfile();
    GV_CHECKF(f != NULL, "tmpfile() unavailable");
    if (!f) return;

    GV_CHECK(gv_save_write_stream(&out, f));
    rewind(f);

    gv_save in;
    GV_CHECK(gv_save_read_stream(&in, f));
    fclose(f);
    gv_check_playable(&in);

    GV_CHECK_NEAR(in.settings.master_volume, 0.25f, 1e-3);
    GV_CHECK_NEAR(in.settings.sfx_volume, 0.0f, 1e-3);
    GV_CHECK_NEAR(in.settings.music_volume, 1.0f, 1e-3);
    GV_CHECK(!in.settings.music_enabled);
    GV_CHECK_EQ_I(in.settings.difficulty, GV_DIFF_ACE);
    GV_CHECK_EQ_I(in.settings.fx_quality, GV_FX_LOW);
    GV_CHECK(!in.settings.bloom);
    GV_CHECK(!in.settings.screen_shake);
    GV_CHECK(!in.settings.offscreen_markers);
    GV_CHECK(in.settings.high_contrast);
    GV_CHECK(in.settings.fullscreen);
    GV_CHECK(!in.settings.vsync);
    GV_CHECK_EQ_I(in.total_runs, 41);
    GV_CHECK_EQ_I(in.total_kills, 9001);
    GV_CHECK_NEAR(in.total_time, 1234.5, 0.2);
    GV_CHECK_EQ_I(in.best_wave, 17);

    GV_CHECK_EQ_I(in.score_count, out.score_count);
    for (int i = 0; i < in.score_count && i < out.score_count; ++i) {
        GV_CHECK_EQ_I(in.scores[i].score, out.scores[i].score);
        GV_CHECK_EQ_I(in.scores[i].wave, out.scores[i].wave);
        GV_CHECK_EQ_I(in.scores[i].difficulty, out.scores[i].difficulty);
        GV_CHECK_EQ_I(in.scores[i].timestamp, out.scores[i].timestamp);
    }

    /* A second trip must be a fixed point: writing what we just read has to
     * produce byte-identical text, or the file would drift every launch. */
    FILE *a = tmpfile();
    FILE *b = tmpfile();
    GV_CHECKF(a != NULL && b != NULL, "tmpfile() unavailable");
    if (a && b) {
        GV_CHECK(gv_save_write_stream(&out, a));
        GV_CHECK(gv_save_write_stream(&in, b));
        rewind(a);
        rewind(b);
        int ca, cb;
        long same = 0;
        do {
            ca = fgetc(a);
            cb = fgetc(b);
            same++;
        } while (ca == cb && ca != EOF);
        GV_CHECKF(ca == EOF && cb == EOF, "save text drifts on rewrite at byte %ld", same);
    }
    if (a) fclose(a);
    if (b) fclose(b);
}

GV_TEST(save_survives_hostile_files)
{
    /* Each of these has been a real crash in some game's config parser at some
     * point: unterminated lines, no separator, empty keys, binary, huge
     * numbers, and text where a number belongs. */
    static const char *const hostile[] = {
        "",
        "\n\n\n",
        "=",
        "=1",
        "master_volume",
        "master_volume=",
        "   =   ",
        "#############",
        "master_volume = ",
        "master_volume = abc",
        "master_volume = 0.5", /* no trailing newline */
        "difficulty = -2147483648\nfx_quality = 2147483647\n",
        "total_runs = 99999999999999999999999999\n",
        "total_kills = -99999999999999999999999999\n",
        "best_wave = nan\ntotal_time = inf\n",
        "master_volume = nan\nsfx_volume = inf\nmusic_volume = -inf\n",
        "master_volume = 1e400\n",
        "bloom = maybe\nvsync = \x01\n",
        "\xff\xfe\x01\x02\x03\x04\x7f",
        "score = \nscore = :::\nscore = a:b:c:d\nscore = 5\nscore = 5:\n",
        "score = -100:-4:-9:-1\n",
        "score = 9223372036854775808:1:1:1\n",
        "master_volume\r\n=\r\n0.5\r\n",
        "\tmaster_volume\t=\t0.5\t\n",
        "MASTER_VOLUME = 0.1\n", /* keys are case-sensitive; must be ignored */
        "master_volume = 0.5 = 0.9\n",
        "score=1:1:1:1\nscore=2:2:2:2\nscore=3:3:3:3\n",
    };

    for (size_t i = 0; i < sizeof(hostile) / sizeof(hostile[0]); ++i) {
        gv_save s;
        memset(&s, 0x5A, sizeof(s));
        GV_CHECKF(gv_parse_into(&s, hostile[i]), "tmpfile() unavailable");
        gv_check_playable(&s);
    }

    /* Embedded NUL bytes: fgets keeps reading to the newline, so the parser
     * sees a buffer whose C string ends early. Nothing may read past it. */
    {
        static const char nuls[] = "master_volume\0 = 0.5\nvsync\0 = 0\n\0\0\n";
        FILE *f = tmpfile();
        GV_CHECKF(f != NULL, "tmpfile() unavailable");
        if (f) {
            fwrite(nuls, 1, sizeof(nuls) - 1, f);
            rewind(f);
            gv_save s;
            gv_save_read_stream(&s, f);
            fclose(f);
            gv_check_playable(&s);
        }
    }

    /* A file that is one enormous unterminated line: fgets never sees a
     * newline, so the loop has to terminate on EOF alone. */
    {
        FILE *f = tmpfile();
        GV_CHECKF(f != NULL, "tmpfile() unavailable");
        if (f) {
            for (int i = 0; i < 4096; ++i) fputs("0123456789ABCDEF", f);
            rewind(f);
            gv_save s;
            gv_save_read_stream(&s, f);
            fclose(f);
            gv_check_playable(&s);
        }
    }

    /* A line far longer than the parser's buffer is split across reads; the
     * tail must be treated as another (nonsense) line, not overrun anything. */
    size_t huge_len = 64 * 1024;
    char *huge = (char *)malloc(huge_len + 1);
    GV_CHECK(huge != NULL);
    if (huge) {
        memset(huge, 'x', huge_len);
        huge[huge_len] = '\0';
        memcpy(huge, "master_volume = 0.5", 19);

        gv_save s;
        GV_CHECKF(gv_parse_into(&s, huge), "tmpfile() unavailable");
        gv_check_playable(&s);
        free(huge);
    }
}

GV_TEST(save_bounds_a_pathological_file)
{
    /* A multi-megabyte file must not stall startup: the parser stops after a
     * fixed number of lines. The settings before the cut still apply. */
    const int lines = 200000;
    size_t cap = (size_t)lines * 24 + 64;
    char *text = (char *)malloc(cap);
    GV_CHECK(text != NULL);
    if (!text) return;

    size_t n = 0;
    n += (size_t)snprintf(text + n, cap - n, "difficulty = 2\n");
    for (int i = 0; i < lines && n + 32 < cap; ++i) {
        n += (size_t)snprintf(text + n, cap - n, "score = %d:1:0:0\n", i + 1);
    }

    gv_save s;
    GV_CHECKF(gv_parse_into(&s, text), "tmpfile() unavailable");
    free(text);

    gv_check_playable(&s);
    GV_CHECK_EQ_I(s.settings.difficulty, 2);
    GV_CHECK_EQ_I(s.score_count, GV_MAX_SCORES);
}

GV_TEST(save_clamps_rather_than_rejecting)
{
    /* Out-of-range but well-formed values are clamped, not discarded: a save
     * from a build with a wider range should still be usable here. */
    gv_save s;
    GV_CHECKF(gv_parse_into(&s,
                            "master_volume = 7.5\n"
                            "sfx_volume = -3\n"
                            "music_volume = 0.5\n"
                            "difficulty = 900\n"
                            "fx_quality = -900\n"
                            "best_wave = 999999999\n"
                            "total_time = 1e30\n"),
              "tmpfile() unavailable");
    gv_check_playable(&s);
    GV_CHECK_NEAR(s.settings.master_volume, 1.0f, 1e-4);
    GV_CHECK_NEAR(s.settings.sfx_volume, 0.0f, 1e-4);
    GV_CHECK_NEAR(s.settings.music_volume, 0.5f, 1e-4);
    GV_CHECK_EQ_I(s.settings.difficulty, GV_DIFF_COUNT - 1);
    GV_CHECK_EQ_I(s.settings.fx_quality, 0);

    /* An unparseable value leaves the default in place rather than zeroing it,
     * so one bad line cannot mute the game. */
    gv_save d;
    gv_save_defaults(&d);
    gv_save t;
    GV_CHECKF(gv_parse_into(&t, "master_volume = loud\nmusic_enabled = perhaps\n"),
              "tmpfile() unavailable");
    GV_CHECK_NEAR(t.settings.master_volume, d.settings.master_volume, 1e-6);
    GV_CHECK_EQ_I(t.settings.music_enabled, d.settings.music_enabled);
}

GV_TEST(save_ignores_what_it_does_not_know)
{
    /* Forward compatibility: a file written by a later build must still load,
     * keeping the keys this build understands and dropping the rest. */
    gv_save s;
    GV_CHECKF(gv_parse_into(&s,
                            "# GRAVITON settings and scores\n"
                            "version = 99\n"
                            "ray_tracing = 1\n"
                            "colour_grade = filmic\n"
                            "master_volume = 0.4\n"
                            "  # indented comment\n"
                            "\n"
                            "difficulty = 0\n"),
              "tmpfile() unavailable");
    gv_check_playable(&s);
    GV_CHECK_NEAR(s.settings.master_volume, 0.4f, 1e-4);
    GV_CHECK_EQ_I(s.settings.difficulty, GV_DIFF_CADET);
}

GV_TEST(save_sorts_a_hand_edited_table)
{
    gv_save s;
    GV_CHECKF(gv_parse_into(&s,
                            "score = 300:3:1:0\n"
                            "score = 100:1:1:0\n"
                            "score = 900:9:2:0\n"
                            "score = 200:2:0:0\n"),
              "tmpfile() unavailable");
    gv_check_playable(&s);
    GV_CHECK_EQ_I(s.score_count, 4);
    GV_CHECK_EQ_I(s.scores[0].score, 900);
    GV_CHECK_EQ_I(s.scores[1].score, 300);
    GV_CHECK_EQ_I(s.scores[2].score, 200);
    GV_CHECK_EQ_I(s.scores[3].score, 100);
    /* The rest of the entry must travel with the score, not be sorted apart. */
    GV_CHECK_EQ_I(s.scores[0].wave, 9);
    GV_CHECK_EQ_I(s.scores[3].wave, 1);
}

GV_TEST(save_submit_ranks_and_caps)
{
    gv_save s;
    gv_save_defaults(&s);

    /* Fill the table with a descending ladder. */
    for (int i = 0; i < GV_MAX_SCORES; ++i) {
        int64_t score = (int64_t)(GV_MAX_SCORES - i) * 1000;
        GV_CHECK_EQ_I(gv_save_submit_score(&s, score, i + 1, GV_DIFF_PILOT, 0), i);
    }
    GV_CHECK_EQ_I(s.score_count, GV_MAX_SCORES);
    GV_CHECK_EQ_I(s.total_runs, GV_MAX_SCORES);
    gv_check_playable(&s);

    /* Below the floor: no place, table untouched, but the run still counts. */
    int64_t before_runs = s.total_runs;
    GV_CHECK_EQ_I(gv_save_submit_score(&s, 1, 1, GV_DIFF_PILOT, 0), -1);
    GV_CHECK_EQ_I(s.total_runs, before_runs + 1);
    GV_CHECK_EQ_I(s.score_count, GV_MAX_SCORES);
    GV_CHECK_EQ_I(s.scores[GV_MAX_SCORES - 1].score, 1000);

    /* A new best goes to the top and pushes the worst entry off the end. */
    GV_CHECK_EQ_I(gv_save_submit_score(&s, 99999, 30, GV_DIFF_ACE, 7), 0);
    GV_CHECK_EQ_I(s.score_count, GV_MAX_SCORES);
    GV_CHECK_EQ_I(s.scores[0].score, 99999);
    GV_CHECK_EQ_I(s.scores[0].wave, 30);
    GV_CHECK_EQ_I(s.scores[0].timestamp, 7);
    GV_CHECK_EQ_I(s.scores[1].score, (int64_t)GV_MAX_SCORES * 1000);
    GV_CHECK_EQ_I(s.scores[GV_MAX_SCORES - 1].score, 2000);
    gv_check_playable(&s);

    /* A tie keeps the older entry ahead — you have to beat a score to pass it. */
    GV_CHECK_EQ_I(gv_save_submit_score(&s, 99999, 99, GV_DIFF_ACE, 8), 1);
    GV_CHECK_EQ_I(s.scores[0].wave, 30);
    GV_CHECK_EQ_I(s.scores[1].wave, 99);

    /* An out-of-range difficulty is clamped rather than stored and later used
     * to index the difficulty names. */
    GV_CHECK_EQ_I(gv_save_submit_score(&s, 500000, 1, 12345, 0), 0);
    GV_CHECK_EQ_I(s.scores[0].difficulty, GV_DIFF_COUNT - 1);
    GV_CHECK_EQ_I(gv_save_submit_score(&s, 600000, 1, -12345, 0), 0);
    GV_CHECK_EQ_I(s.scores[0].difficulty, 0);
    gv_check_playable(&s);
}

GV_TEST(save_submit_counts_worthless_runs)
{
    /* Dying without scoring is still a run and can still be a best wave; it
     * just does not earn a place in the table. */
    gv_save s;
    gv_save_defaults(&s);
    GV_CHECK_EQ_I(gv_save_submit_score(&s, 0, 4, GV_DIFF_PILOT, 0), -1);
    GV_CHECK_EQ_I(s.score_count, 0);
    GV_CHECK_EQ_I(s.total_runs, 1);
    GV_CHECK_EQ_I(s.best_wave, 4);

    GV_CHECK_EQ_I(gv_save_submit_score(&s, -50, 2, GV_DIFF_PILOT, 0), -1);
    GV_CHECK_EQ_I(s.score_count, 0);
    GV_CHECK_EQ_I(s.total_runs, 2);
    GV_CHECK_EQ_I(s.best_wave, 4); /* a worse wave must not lower the record */
    gv_check_playable(&s);
}

GV_TEST(save_best_score_filters_by_difficulty)
{
    gv_save s;
    gv_save_defaults(&s);
    gv_save_submit_score(&s, 400, 4, GV_DIFF_CADET, 0);
    gv_save_submit_score(&s, 900, 9, GV_DIFF_PILOT, 0);
    gv_save_submit_score(&s, 700, 7, GV_DIFF_PILOT, 0);

    GV_CHECK_EQ_I(gv_save_best_score(&s, -1), 900);
    GV_CHECK_EQ_I(gv_save_best_score(&s, GV_DIFF_CADET), 400);
    GV_CHECK_EQ_I(gv_save_best_score(&s, GV_DIFF_PILOT), 900);
    GV_CHECK_EQ_I(gv_save_best_score(&s, GV_DIFF_ACE), 0);

    /* A difficulty that does not exist is simply never matched. */
    GV_CHECK_EQ_I(gv_save_best_score(&s, 999), 0);

    /* A corrupt in-memory count must not read past the array. */
    s.score_count = GV_MAX_SCORES + 100;
    GV_CHECK_EQ_I(gv_save_best_score(&s, -1), 900);
}

GV_TEST(save_tolerates_null_arguments)
{
    /* Every entry point is reachable from a failed load, so none of them may
     * assume it was handed something. */
    gv_save_defaults(NULL);
    GV_CHECK(!gv_save_read_stream(NULL, NULL));
    GV_CHECK(!gv_save_write_stream(NULL, NULL));
    GV_CHECK_EQ_I(gv_save_submit_score(NULL, 100, 1, 0, 0), -1);
    GV_CHECK_EQ_I(gv_save_best_score(NULL, -1), 0);

    /* A NULL stream still leaves usable defaults behind. */
    gv_save s;
    memset(&s, 0x3C, sizeof(s));
    GV_CHECK(!gv_save_read_stream(&s, NULL));
    gv_check_playable(&s);

    gv_save d;
    gv_save_defaults(&d);
    GV_CHECK(!gv_save_write_stream(&d, NULL));
}

GV_TEST(save_fx_quality_mapping_is_total)
{
    /* The setting indexes straight into these, including values a corrupt file
     * could have produced before clamping, so they must never fall through. */
    for (int q = -3; q < GV_FX_COUNT + 3; ++q) {
        float scale = gv_fx_quality_scale(q);
        GV_CHECK_FINITE(scale);
        GV_CHECK(scale > 0.0f && scale <= 1.0f);
        const char *name = gv_fx_quality_name(q);
        GV_CHECK(name != NULL && *name != '\0');
    }
    GV_CHECK(gv_fx_quality_scale(GV_FX_LOW) < gv_fx_quality_scale(GV_FX_MEDIUM));
    GV_CHECK(gv_fx_quality_scale(GV_FX_MEDIUM) < gv_fx_quality_scale(GV_FX_HIGH));
    GV_CHECK_NEAR(gv_fx_quality_scale(GV_FX_HIGH), 1.0f, 1e-6);
    GV_CHECK(strcmp(gv_fx_quality_name(GV_FX_LOW), "LOW") == 0);
    GV_CHECK(strcmp(gv_fx_quality_name(GV_FX_MEDIUM), "MEDIUM") == 0);
    GV_CHECK(strcmp(gv_fx_quality_name(GV_FX_HIGH), "HIGH") == 0);
}
