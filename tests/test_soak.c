/* test_soak.c — long-running invariant checks.
 *
 * The unit tests above cover behaviour that was designed on purpose. This file
 * covers everything else: many seeds, all difficulties, hundreds of thousands
 * of simulated steps, with every structural invariant asserted continuously.
 * It is the net that catches interactions nobody thought to write a test for.
 */
#include "../src/game/gv_autopilot.h"
#include "gv_fixture.h"
#include "gv_test.h"

/* Positions may sit slightly outside the arena for a single step before the
 * confinement pass pulls them back; anything beyond this is a real escape. */
#define GV_SOAK_SLOP 64.0f

typedef struct {
    int violations;
    const char *first_failure;
} gv_soak_result;

/* Check every structural invariant that must hold at the end of any step. */
static const char *gv_soak_check(const gv_world *w)
{
    if (!gv_world_all_finite(w)) return "non-finite value in world state";

    if (w->player.lives < 0) return "negative lives";
    if (w->player.lives > GV_PLAYER_MAX_LIVES) return "lives above the cap";
    if (w->player.speed > GV_PLAYER_MAX_SPEED + 1.0f) return "player speed above the cap";
    if (w->player.focus < 0.0f || w->player.focus > GV_FOCUS_MAX + 1e-3f) return "focus out of range";
    if (w->player.pulse_charge < -1e-3f || w->player.pulse_charge > GV_PULSE_MAX + 1e-3f) {
        return "pulse charge out of range";
    }

    if (w->player.alive) {
        if (w->player.pos.x < -GV_SOAK_SLOP || w->player.pos.x > w->arena_w + GV_SOAK_SLOP ||
            w->player.pos.y < -GV_SOAK_SLOP || w->player.pos.y > w->arena_h + GV_SOAK_SLOP) {
            return "player outside the arena";
        }
    }

    if (w->score.multiplier < 1 || w->score.multiplier > GV_MULT_MAX) return "multiplier out of range";
    if (w->score.score < 0) return "negative score";
    if (w->score.combo < 0) return "negative combo";
    if (w->events_dropped) return "event queue overflowed";
    if (w->event_count < 0 || w->event_count > GV_MAX_EVENTS) return "event count out of range";

    int alive = 0;
    for (int i = 0; i < GV_MAX_ENEMIES; ++i) {
        const gv_enemy *e = &w->enemies[i];
        if (!e->alive) continue;
        alive++;
        if (e->pos.x < -GV_SOAK_SLOP || e->pos.x > w->arena_w + GV_SOAK_SLOP ||
            e->pos.y < -GV_SOAK_SLOP || e->pos.y > w->arena_h + GV_SOAK_SLOP) {
            return "enemy outside the arena";
        }
        if (e->radius <= 0.0f) return "enemy with non-positive radius";
        if (e->kind >= GV_ENEMY_KIND_COUNT) return "enemy with an invalid kind";
        if (e->gen == 0) return "enemy with a reserved generation";
        if (e->split_depth > 4) return "split depth ran away";
    }
    if (alive != w->enemy_count) return "enemy_count out of sync with the pool";

    for (int i = 0; i < GV_MAX_ROCKS; ++i) {
        const gv_rock *r = &w->rocks[i];
        if (!r->alive) continue;
        if (r->pos.x < -GV_SOAK_SLOP || r->pos.x > w->arena_w + GV_SOAK_SLOP ||
            r->pos.y < -GV_SOAK_SLOP || r->pos.y > w->arena_h + GV_SOAK_SLOP) {
            return "rock outside the arena";
        }
        if (r->inv_mass <= 0.0f) return "rock with non-positive inverse mass";
        if (gv_v2_len(r->vel) > 2400.0f) return "rock exceeded its speed cap";
    }

    /* The tether must always point at something real, or at nothing at all. */
    const gv_tether *t = &w->player.tether;
    if (t->state == GV_TETHER_ATTACHED) {
        if (t->anchor_kind == GV_ANCHOR_NONE) return "attached tether with no anchor";
        if (t->length < GV_TETHER_MIN_LEN - 1e-2f || t->length > GV_TETHER_MAX_LEN + 1e-2f) {
            return "tether length out of range";
        }
        switch (t->anchor_kind) {
        case GV_ANCHOR_ENEMY:
            if (t->anchor_index < 0 || t->anchor_index >= GV_MAX_ENEMIES) return "bad enemy anchor";
            if (!w->enemies[t->anchor_index].alive) return "anchored to a dead enemy";
            if (w->enemies[t->anchor_index].gen != t->anchor_gen) return "anchored to a stale slot";
            break;
        case GV_ANCHOR_ROCK:
            if (t->anchor_index < 0 || t->anchor_index >= GV_MAX_ROCKS) return "bad rock anchor";
            if (!w->rocks[t->anchor_index].alive) return "anchored to a dead rock";
            break;
        case GV_ANCHOR_PYLON:
            if (t->anchor_index < 0 || t->anchor_index >= w->pylon_count) return "bad pylon anchor";
            break;
        default:
            return "attached tether with an unknown anchor kind";
        }
    } else if (t->anchor_kind != GV_ANCHOR_NONE) {
        return "unattached tether still holding an anchor";
    }

    if (w->shake < 0.0f || w->shake > 1.0f + 1e-3f) return "shake out of range";
    if (w->hitstop > 0.25f) return "hitstop ran long";
    if (w->time_scale <= 0.0f || w->time_scale > 1.0f + 1e-3f) return "time scale out of range";

    return NULL;
}

/* Drive one run to completion (or the step budget) with the given policy. */
static gv_soak_result gv_soak_run(uint64_t seed, int difficulty, int max_steps, bool use_bot)
{
    gv_soak_result out = { 0, NULL };
    gv_world *w = gv_new_world(seed, difficulty);
    if (!w) {
        out.violations++;
        out.first_failure = "allocation failed";
        return out;
    }

    gv_rng script;
    gv_rng_seed(&script, seed ^ 0xA5A5A5A5u, 11u);

    for (int i = 0; i < max_steps; ++i) {
        gv_input in;
        if (use_bot) {
            in = gv_autopilot_think(w, &script);
        } else {
            memset(&in, 0, sizeof(in));
            in.move = gv_v2_polar(gv_rng_angle(&script), gv_rng_f01(&script) * 1.4f);
            in.aim_point = gv_v2_make(gv_rng_range_f(&script, -500.0f, GV_ARENA_W + 500.0f),
                                      gv_rng_range_f(&script, -500.0f, GV_ARENA_H + 500.0f));
            in.tether_held = gv_rng_chance(&script, 0.6f);
            in.tether_pressed = gv_rng_chance(&script, 0.05f);
            in.pulse_pressed = gv_rng_chance(&script, 0.02f);
            in.focus_held = gv_rng_chance(&script, 0.15f);
            in.reel = gv_rng_range_f(&script, -1.5f, 1.5f);
        }

        gv_world_step(w, &in, GV_FIXED_DT);
        const char *fail = gv_soak_check(w);
        if (fail) {
            out.violations++;
            if (!out.first_failure) out.first_failure = fail;
            break;
        }
        gv_world_clear_events(w);

        /* Restart on death so a single run does not end the soak early. */
        if (gv_world_is_over(w)) gv_world_init(w, seed + (uint64_t)i, difficulty);
    }

    free(w);
    return out;
}

GV_TEST(soak_random_input_across_seeds)
{
    /* Pure fuzz: nonsense input, out-of-range aim points, over-unit axes. */
    for (uint64_t seed = 1; seed <= 12; ++seed) {
        for (int d = 0; d < GV_DIFF_COUNT; ++d) {
            gv_soak_result r = gv_soak_run(seed * 6151u, d, 9000, false);
            GV_CHECKF(r.violations == 0, "seed %llu difficulty %d: %s",
                      (unsigned long long)(seed * 6151u), d,
                      r.first_failure ? r.first_failure : "?");
        }
    }
}

GV_TEST(soak_bot_play_across_seeds)
{
    /* Competent play reaches states random input never will: deep waves,
     * wardens, saturated arenas, long chains. */
    for (uint64_t seed = 1; seed <= 8; ++seed) {
        for (int d = 0; d < GV_DIFF_COUNT; ++d) {
            gv_soak_result r = gv_soak_run(seed * 7919u, d, 24000, true);
            GV_CHECKF(r.violations == 0, "seed %llu difficulty %d: %s",
                      (unsigned long long)(seed * 7919u), d,
                      r.first_failure ? r.first_failure : "?");
        }
    }
}

GV_TEST(soak_bot_reaches_deep_waves)
{
    /* A balance guard rather than a correctness one: if a change makes the
     * game unplayably hard or trivially easy, the bot's reach moves and this
     * test says so. The bounds are wide on purpose — it should only fire on a
     * real regression, not on tuning noise. */
    int total_waves = 0, runs = 0;
    int64_t total_score = 0;
    int best_wave = 0;

    for (uint64_t seed = 1; seed <= 6; ++seed) {
        gv_world *w = gv_new_world(seed * 104729u, GV_DIFF_PILOT);
        GV_CHECK(w != NULL);
        if (!w) return;

        gv_rng script;
        gv_rng_seed(&script, seed, 13u);
        for (int i = 0; i < 120 * 300; ++i) {
            gv_input in = gv_autopilot_think(w, &script);
            gv_world_step(w, &in, GV_FIXED_DT);
            gv_world_clear_events(w);
            if (gv_world_is_over(w)) break;
        }
        total_waves += w->wave.index;
        total_score += w->score.score;
        if (w->wave.index > best_wave) best_wave = w->wave.index;
        runs++;
        free(w);
    }

    float avg_wave = (float)total_waves / (float)runs;
    GV_CHECKF(avg_wave >= 3.0f, "bot averaged only wave %.1f — the game may be too hard",
              (double)avg_wave);
    GV_CHECKF(avg_wave <= 40.0f, "bot averaged wave %.1f — the game may be too easy",
              (double)avg_wave);
    GV_CHECKF(total_score > 0, "bot scored nothing across %d runs", runs);
    GV_CHECKF(best_wave >= 5, "no run reached wave 5 (best %d)", best_wave);
}

GV_TEST(soak_survives_repeated_restarts)
{
    /* Re-initialising over a used world must fully reset it: a leaked entity
     * or a stale counter would show up as an immediate invariant violation. */
    gv_world *w = gv_new_world(1u, GV_DIFF_PILOT);
    GV_CHECK(w != NULL);
    if (!w) return;

    gv_rng script;
    gv_rng_seed(&script, 4u, 17u);
    for (int run = 0; run < 40; ++run) {
        gv_world_init(w, (uint64_t)run * 31u + 5u, run % GV_DIFF_COUNT);

        GV_CHECK_EQ_I(w->enemy_count, 0);
        GV_CHECK_EQ_I(gv_world_alive_enemies(w), 0);
        GV_CHECK_EQ_I(w->score.score, 0);
        GV_CHECK_EQ_I(w->score.kills, 0);
        GV_CHECK_EQ_I(w->score.multiplier, 1);
        GV_CHECK_EQ_I(w->player.tether.state, GV_TETHER_IDLE);
        GV_CHECK_EQ_I(w->run_state, GV_RUN_PLAYING);
        GV_CHECK_EQ_I(gv_count_live_bullets(w), 0);
        GV_CHECK_EQ_I(gv_count_live_pickups(w), 0);
        GV_CHECK_EQ_I(gv_count_live_particles(w), 0);
        GV_CHECK(w->player.alive);

        for (int i = 0; i < 900; ++i) {
            gv_input in = gv_autopilot_think(w, &script);
            gv_world_step(w, &in, GV_FIXED_DT);
            gv_world_clear_events(w);
        }
        const char *fail = gv_soak_check(w);
        GV_CHECKF(fail == NULL, "run %d: %s", run, fail ? fail : "");
        if (fail) break;
    }
    free(w);
}

GV_TEST(soak_alternate_timesteps_stay_stable)
{
    /* The game always runs at the fixed step, but the simulation should not
     * fall apart at other rates — a stiff constraint solver that only works at
     * one dt is a latent bug waiting for a future refactor. */
    const float steps[] = { 1.0f / 240.0f, 1.0f / 120.0f, 1.0f / 60.0f, 1.0f / 30.0f };

    for (size_t s = 0; s < sizeof(steps) / sizeof(steps[0]); ++s) {
        gv_world *w = gv_new_world(9001u, GV_DIFF_PILOT);
        GV_CHECK(w != NULL);
        if (!w) return;

        gv_rng script;
        gv_rng_seed(&script, 19u, 23u);
        int seconds = 60;
        int count = (int)((float)seconds / steps[s]);
        for (int i = 0; i < count; ++i) {
            gv_input in = gv_autopilot_think(w, &script);
            gv_world_step(w, &in, steps[s]);
            const char *fail = gv_soak_check(w);
            GV_CHECKF(fail == NULL, "dt=1/%.0f: %s", (double)(1.0f / steps[s]),
                      fail ? fail : "");
            if (fail) break;
            gv_world_clear_events(w);
            if (gv_world_is_over(w)) break;
        }
        free(w);
    }
}
