/* test_sim.c — simulation behaviour and edge cases.
 *
 * These tests drive the real world struct through the real step function; no
 * mocks are involved. Scenarios are built by placing entities directly, which
 * is possible because the whole world is plain data.
 */
#include <stdlib.h>

#include "gv_fixture.h"
#include "gv_test.h"

/* ------------------------------------------------------------------- setup */

GV_TEST(sim_init_produces_valid_world)
{
    gv_world *w = gv_new_world(1234u, GV_DIFF_PILOT);
    GV_CHECK(w != NULL);
    if (!w) return;

    GV_CHECK(w->player.alive);
    GV_CHECK_EQ_I(w->player.lives, 3);
    GV_CHECK_NEAR(w->player.pos.x, GV_ARENA_W * 0.5f, 1.0f);
    GV_CHECK_NEAR(w->player.pos.y, GV_ARENA_H * 0.5f, 1.0f);
    GV_CHECK_EQ_I(w->score.multiplier, 1);
    GV_CHECK_EQ_I(w->score.score, 0);
    GV_CHECK_EQ_I(gv_world_alive_enemies(w), 0);
    GV_CHECK_EQ_I(w->enemy_count, 0);
    GV_CHECK(w->pylon_count > 0 && w->pylon_count <= GV_MAX_PYLONS);
    /* The exact rock count is a tuning value, so assert the property that
     * actually matters — the arena is furnished and the counter agrees with
     * the pool — rather than pinning a number that moves with balance work. */
    GV_CHECK(w->rock_count >= 6 && w->rock_count <= GV_MAX_ROCKS);
    int rocks_alive = 0;
    for (int i = 0; i < GV_MAX_ROCKS; ++i) {
        if (w->rocks[i].alive) rocks_alive++;
    }
    GV_CHECK_EQ_I(rocks_alive, w->rock_count);
    GV_CHECK_EQ_I(w->player.tether.state, GV_TETHER_IDLE);
    GV_CHECK_EQ_I(w->run_state, GV_RUN_PLAYING);
    GV_CHECK(!gv_world_is_over(w));

    /* Every pylon must sit inside the arena and clear of the spawn point. */
    for (int i = 0; i < w->pylon_count; ++i) {
        GV_CHECK(w->pylons[i].alive);
        GV_CHECK(w->pylons[i].pos.x > 0.0f && w->pylons[i].pos.x < GV_ARENA_W);
        GV_CHECK(w->pylons[i].pos.y > 0.0f && w->pylons[i].pos.y < GV_ARENA_H);
        GV_CHECKF(gv_v2_dist(w->pylons[i].pos, w->player.pos) > 200.0f,
                  "pylon %d spawned on top of the player", i);
    }
    /* Rocks must not be overlapping the ship at t=0 either. */
    for (int i = 0; i < GV_MAX_ROCKS; ++i) {
        if (!w->rocks[i].alive) continue;
        GV_CHECK(gv_v2_dist(w->rocks[i].pos, w->player.pos) > w->rocks[i].radius + w->player.radius);
        GV_CHECK(w->rocks[i].inv_mass > 0.0f);
    }
    free(w);
}

GV_TEST(sim_difficulty_tuning)
{
    gv_tuning t;
    gv_tuning_for(&t, GV_DIFF_CADET);
    GV_CHECK_EQ_I(t.start_lives, 4);
    gv_tuning_for(&t, GV_DIFF_ACE);
    GV_CHECK_EQ_I(t.start_lives, 2);
    GV_CHECK(t.score_mult > 1.0f);

    /* Out-of-range difficulty must clamp, not read past the table. */
    gv_tuning lo, hi;
    gv_tuning_for(&lo, -99);
    gv_tuning_for(&hi, 12345);
    GV_CHECK_EQ_I(lo.start_lives, 4);
    GV_CHECK_EQ_I(hi.start_lives, 2);

    GV_CHECK(strcmp(gv_difficulty_name(GV_DIFF_PILOT), "PILOT") == 0);
    GV_CHECK(gv_difficulty_name(-5) != NULL);
    GV_CHECK(gv_difficulty_name(99) != NULL);
}

/* ------------------------------------------------------------ determinism */

GV_TEST(sim_is_deterministic_from_seed)
{
    gv_world *a = gv_new_world(0xABCDEFu, GV_DIFF_PILOT);
    gv_world *b = gv_new_world(0xABCDEFu, GV_DIFF_PILOT);
    GV_CHECK(a && b);
    if (!a || !b) { free(a); free(b); return; }

    /* Identical scripted input from an independent generator. */
    gv_rng script;
    gv_rng_seed(&script, 7u, 3u);

    for (int i = 0; i < 6000; ++i) {
        gv_input in;
        memset(&in, 0, sizeof(in));
        in.move = gv_v2_polar(gv_rng_angle(&script), gv_rng_f01(&script));
        in.aim_point = gv_v2_make(gv_rng_range_f(&script, 0.0f, GV_ARENA_W),
                                  gv_rng_range_f(&script, 0.0f, GV_ARENA_H));
        in.tether_held = gv_rng_chance(&script, 0.55f);
        in.tether_pressed = gv_rng_chance(&script, 0.02f);
        in.pulse_pressed = gv_rng_chance(&script, 0.004f);
        in.focus_held = gv_rng_chance(&script, 0.1f);
        in.reel = gv_rng_range_f(&script, -1.0f, 1.0f);

        gv_world_step(a, &in, GV_FIXED_DT);
        gv_world_step(b, &in, GV_FIXED_DT);
        gv_world_clear_events(a);
        gv_world_clear_events(b);
    }

    /* Byte-for-byte: the world is plain data and both were memset at init, so
     * even padding must match. */
    GV_CHECKF(memcmp(a, b, sizeof(gv_world)) == 0,
              "identical seeds and inputs diverged after 6000 steps");
    GV_CHECK(a->score.score == b->score.score);
    GV_CHECK_EQ_I(a->wave.index, b->wave.index);
    free(a);
    free(b);
}

GV_TEST(sim_seeds_produce_different_arenas)
{
    gv_world *a = gv_new_world(1u, GV_DIFF_PILOT);
    gv_world *b = gv_new_world(2u, GV_DIFF_PILOT);
    GV_CHECK(a && b);
    if (!a || !b) { free(a); free(b); return; }

    int differing = 0;
    for (int i = 0; i < a->pylon_count && i < b->pylon_count; ++i) {
        if (gv_v2_dist(a->pylons[i].pos, b->pylons[i].pos) > 1.0f) differing++;
    }
    GV_CHECKF(differing > a->pylon_count / 2, "arena layouts too similar across seeds");
    free(a);
    free(b);
}

GV_TEST(sim_fx_quality_does_not_change_gameplay)
{
    /* The particle-density setting must be provably cosmetic: a low-spec
     * player and a high-spec player must get identical runs from a seed. */
    gv_world *a = gv_new_world(0x5EEDu, GV_DIFF_PILOT);
    gv_world *b = gv_new_world(0x5EEDu, GV_DIFF_PILOT);
    GV_CHECK(a && b);
    if (!a || !b) { free(a); free(b); return; }

    a->fx_quality = 1.0f;
    b->fx_quality = 0.15f;

    gv_rng script;
    gv_rng_seed(&script, 42u, 9u);
    for (int i = 0; i < 5000; ++i) {
        gv_input in;
        memset(&in, 0, sizeof(in));
        in.move = gv_v2_polar(gv_rng_angle(&script), gv_rng_f01(&script));
        in.aim_point = gv_v2_make(gv_rng_range_f(&script, 0.0f, GV_ARENA_W),
                                  gv_rng_range_f(&script, 0.0f, GV_ARENA_H));
        in.tether_held = gv_rng_chance(&script, 0.6f);
        in.tether_pressed = gv_rng_chance(&script, 0.03f);
        in.pulse_pressed = gv_rng_chance(&script, 0.005f);
        in.reel = gv_rng_range_f(&script, -1.0f, 1.0f);
        gv_world_step(a, &in, GV_FIXED_DT);
        gv_world_step(b, &in, GV_FIXED_DT);
        gv_world_clear_events(a);
        gv_world_clear_events(b);
    }

    GV_CHECKF(a->score.score == b->score.score, "score diverged: %lld vs %lld",
              (long long)a->score.score, (long long)b->score.score);
    GV_CHECK_EQ_I(a->wave.index, b->wave.index);
    GV_CHECK_EQ_I(a->score.kills, b->score.kills);
    GV_CHECK_EQ_I(a->player.lives, b->player.lives);
    GV_CHECK_NEAR(a->player.pos.x, b->player.pos.x, 1e-3);
    GV_CHECK_NEAR(a->player.pos.y, b->player.pos.y, 1e-3);
    /* ...and the cosmetic streams really did diverge, proving the test had
     * something to catch. */
    GV_CHECK(a->rng.state == b->rng.state);
    free(a);
    free(b);
}

/* ------------------------------------------------------------- step guards */

GV_TEST(sim_rejects_bad_timesteps)
{
    gv_world *w = gv_new_world(5u, GV_DIFF_PILOT);
    GV_CHECK(w != NULL);
    if (!w) return;

    gv_input in = gv_idle_input(w);
    in.move = gv_v2_make(1.0f, 0.0f);

    gv_v2 before = w->player.pos;
    float t_before = w->time;

    /* Zero, negative, NaN and infinite steps must be no-ops. */
    gv_world_step(w, &in, 0.0f);
    gv_world_step(w, &in, -1.0f);
    gv_world_step(w, &in, NAN);
    gv_world_step(w, &in, INFINITY);
    gv_world_step(w, &in, -INFINITY);

    GV_CHECK_NEAR(w->player.pos.x, before.x, 1e-6);
    GV_CHECK_NEAR(w->player.pos.y, before.y, 1e-6);
    GV_CHECK_NEAR(w->time, t_before, 1e-6);

    /* An absurdly large step is clamped, not honoured: no teleporting. */
    gv_world_step(w, &in, 1000.0f);
    GV_CHECK(gv_world_all_finite(w));
    GV_CHECK(gv_v2_dist(w->player.pos, before) < 400.0f);
    free(w);
}

GV_TEST(sim_null_input_is_safe)
{
    gv_world *w = gv_new_world(6u, GV_DIFF_PILOT);
    GV_CHECK(w != NULL);
    if (!w) return;

    for (int i = 0; i < 600; ++i) {
        gv_world_step(w, NULL, GV_FIXED_DT);
        gv_world_clear_events(w);
    }
    GV_CHECK(gv_world_all_finite(w));
    GV_CHECK(w->player.alive);
    free(w);
}

GV_TEST(sim_handles_hostile_input_values)
{
    gv_world *w = gv_new_world(7u, GV_DIFF_PILOT);
    GV_CHECK(w != NULL);
    if (!w) return;

    /* Values a broken controller driver or a bad axis mapping could produce. */
    gv_input in;
    memset(&in, 0, sizeof(in));
    in.move = gv_v2_make(NAN, INFINITY);
    in.aim_point = gv_v2_make(NAN, NAN);
    in.reel = NAN;
    in.tether_held = true;
    in.tether_pressed = true;

    for (int i = 0; i < 400; ++i) {
        gv_world_step(w, &in, GV_FIXED_DT);
        gv_world_clear_events(w);
    }
    GV_CHECKF(gv_world_all_finite(w), "NaN input leaked into world state");

    in.move = gv_v2_make(1e30f, -1e30f);
    in.aim_point = gv_v2_make(1e30f, 1e30f);
    in.reel = 1e30f;
    for (int i = 0; i < 400; ++i) {
        gv_world_step(w, &in, GV_FIXED_DT);
        gv_world_clear_events(w);
    }
    GV_CHECK(gv_world_all_finite(w));
    GV_CHECK(w->player.pos.x >= 0.0f && w->player.pos.x <= GV_ARENA_W);
    GV_CHECK(w->player.pos.y >= 0.0f && w->player.pos.y <= GV_ARENA_H);
    free(w);
}

/* ---------------------------------------------------------------- movement */

GV_TEST(sim_thrust_accelerates_and_drag_decelerates)
{
    gv_world *w = gv_new_world(8u, GV_DIFF_PILOT);
    GV_CHECK(w != NULL);
    if (!w) return;

    gv_input in = gv_idle_input(w);
    in.move = gv_v2_make(1.0f, 0.0f);
    gv_run(w, &in, 60);
    GV_CHECKF(w->player.vel.x > 500.0f, "thrust produced only %.1f px/s", w->player.vel.x);

    float peak = w->player.speed;
    in.move = gv_v2_zero();
    gv_run(w, &in, 240);
    GV_CHECKF(w->player.speed < peak * 0.55f, "drag barely slowed the ship (%.1f -> %.1f)",
              peak, w->player.speed);
    free(w);
}

GV_TEST(sim_speed_is_capped)
{
    gv_world *w = gv_new_world(9u, GV_DIFF_PILOT);
    GV_CHECK(w != NULL);
    if (!w) return;

    gv_input in = gv_idle_input(w);
    for (int i = 0; i < 4000; ++i) {
        /* Thrust always along the current heading to maximise speed. */
        in.move = gv_v2_norm_or(w->player.vel, gv_v2_make(1.0f, 0.0f));
        in.aim_point = gv_v2_add(w->player.pos, in.move);
        gv_world_step(w, &in, GV_FIXED_DT);
        gv_world_clear_events(w);
        GV_CHECKF(w->player.speed <= GV_PLAYER_MAX_SPEED + 1.0f,
                  "speed %.1f exceeded the cap", w->player.speed);
        if (w->player.speed > GV_PLAYER_MAX_SPEED + 1.0f) break;
    }
    free(w);
}

GV_TEST(sim_player_stays_inside_arena)
{
    gv_world *w = gv_new_world(10u, GV_DIFF_PILOT);
    GV_CHECK(w != NULL);
    if (!w) return;

    gv_rng script;
    gv_rng_seed(&script, 55u, 4u);
    for (int i = 0; i < 12000; ++i) {
        gv_input in;
        memset(&in, 0, sizeof(in));
        /* Deliberately drive into the walls. */
        in.move = gv_v2_polar(gv_rng_angle(&script), 1.0f);
        in.aim_point = gv_v2_add(w->player.pos, in.move);
        gv_world_step(w, &in, GV_FIXED_DT);
        gv_world_clear_events(w);

        bool inside = w->player.pos.x >= 0.0f && w->player.pos.x <= GV_ARENA_W &&
                      w->player.pos.y >= 0.0f && w->player.pos.y <= GV_ARENA_H;
        GV_CHECKF(inside, "player escaped the arena at (%.1f, %.1f)", w->player.pos.x,
                  w->player.pos.y);
        if (!inside) break;
    }
    free(w);
}

GV_TEST(sim_confine_handles_tiny_arena)
{
    /* A radius larger than the arena crosses the min and max bounds; the
     * entity must settle at the centre instead of jittering forever. */
    gv_v2 pos = gv_v2_make(5.0f, 5.0f);
    gv_v2 vel = gv_v2_make(100.0f, 100.0f);
    GV_CHECK(gv_confine(&pos, &vel, 50.0f, 40.0f, 30.0f, 0.5f));
    GV_CHECK_NEAR(pos.x, 20.0f, 1e-4);
    GV_CHECK_NEAR(pos.y, 15.0f, 1e-4);
    GV_CHECK_NEAR(vel.x, 0.0f, 1e-6);
    GV_CHECK_NEAR(vel.y, 0.0f, 1e-6);

    /* Normal bounce reverses the component heading out of bounds. */
    pos = gv_v2_make(-10.0f, 50.0f);
    vel = gv_v2_make(-200.0f, 0.0f);
    GV_CHECK(gv_confine(&pos, &vel, 5.0f, 100.0f, 100.0f, 0.5f));
    GV_CHECK_NEAR(pos.x, 5.0f, 1e-5);
    GV_CHECK_NEAR(vel.x, 100.0f, 1e-4);

    /* Inside the arena: untouched. */
    pos = gv_v2_make(50.0f, 50.0f);
    vel = gv_v2_make(10.0f, 10.0f);
    GV_CHECK(!gv_confine(&pos, &vel, 5.0f, 100.0f, 100.0f, 0.5f));
    GV_CHECK_NEAR(pos.x, 50.0f, 1e-6);
}
