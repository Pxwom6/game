/* test_progress.c — waves, scoring, pickups and the end of a run. */
#include "../src/game/gv_autopilot.h"
#include "gv_fixture.h"
#include "gv_test.h"

GV_TEST(progress_waves_advance)
{
    gv_world *w = gv_new_world(4001u, GV_DIFF_PILOT);
    GV_CHECK(w != NULL);
    if (!w) return;

    GV_CHECK_EQ_I(w->wave.index, 0);
    GV_CHECK_EQ_I(w->wave.state, GV_WAVE_INTRO);

    /* Immortal observer: the director must run on its own. */
    w->player.invuln = 1.0e9f;
    gv_input in = gv_idle_input(w);

    bool saw_start = gv_run_until_event(w, &in, GV_EV_WAVE_START, 2000);
    GV_CHECK(saw_start);
    GV_CHECK_EQ_I(w->wave.index, 1);
    GV_CHECK(w->wave.state == GV_WAVE_SPAWNING || w->wave.state == GV_WAVE_FIGHTING);

    /* Wave 1 must actually put something in the arena. */
    gv_run(w, &in, 600);
    GV_CHECKF(w->enemy_count > 0, "wave 1 spawned nothing");
    free(w);
}

GV_TEST(progress_wave_budget_grows)
{
    /* Later waves must be meaningfully bigger than earlier ones, or the game
     * has no curve at all. Cleared instantly by fiat so the test measures the
     * director rather than the combat. */
    gv_world *w = gv_new_world(4002u, GV_DIFF_PILOT);
    GV_CHECK(w != NULL);
    if (!w) return;
    w->player.invuln = 1.0e9f;

    int spawned_early = 0, spawned_late = 0;
    gv_input in = gv_idle_input(w);

    for (int wave = 1; wave <= 8; ++wave) {
        int before = w->score.kills;
        (void)before;
        int count = 0;
        /* Let the wave spawn out, counting arrivals, then wipe it. */
        for (int i = 0; i < 4000; ++i) {
            gv_world_step(w, &in, GV_FIXED_DT);
            for (int e = 0; e < w->event_count; ++e) {
                if (w->events[e].kind == GV_EV_ENEMY_SPAWN) count++;
            }
            gv_world_clear_events(w);
            if (w->wave.state == GV_WAVE_FIGHTING) break;
        }
        for (int i = 0; i < GV_MAX_ENEMIES; ++i) {
            if (w->enemies[i].alive) {
                gv_kill_enemy(w, &w->enemies[i], GV_KILL_CULL, gv_v2_zero(), 0.0f);
            }
        }
        gv_world_clear_events(w);
        if (wave <= 2) spawned_early += count;
        if (wave >= 7) spawned_late += count;

        /* Advance past the clear/intro beats into the next wave. */
        for (int i = 0; i < 4000 && w->wave.index == wave; ++i) {
            gv_world_step(w, &in, GV_FIXED_DT);
            gv_world_clear_events(w);
        }
    }

    GV_CHECKF(spawned_late > spawned_early,
              "wave budget did not grow (early %d, late %d)", spawned_early, spawned_late);
    free(w);
}

GV_TEST(progress_hunt_escalates_against_stragglers)
{
    /* The single worst failure mode of a wave shooter is one evasive enemy
     * turning a fight into a stalemate. Escalation must engage and must
     * actually bring the straggler to the player. */
    gv_world *w = gv_new_world(4003u, GV_DIFF_PILOT);
    GV_CHECK(w != NULL);
    if (!w) return;
    w->player.invuln = 1.0e9f;
    w->player.vel = gv_v2_zero();

    /* Force a fighting wave with exactly one distant, evasive enemy. */
    w->wave.index = 5;
    w->wave.state = GV_WAVE_FIGHTING;
    w->wave.budget = 0.0f;
    w->wave.fight_time = 0.0f;
    w->wave.hunt = 0.0f;

    gv_enemy *e = gv_place_enemy(w, GV_ENEMY_SENTINEL,
                                 gv_v2_make(w->arena_w - 120.0f, w->arena_h - 120.0f));
    GV_CHECK(e != NULL);
    if (!e) { free(w); return; }
    w->player.pos = gv_v2_make(120.0f, 120.0f);

    float start_dist = gv_v2_dist(e->pos, w->player.pos);
    gv_input in = gv_idle_input(w);

    /* Well inside the grace period: no escalation yet. */
    gv_run(w, &in, (int)(4.0f / GV_FIXED_DT));
    GV_CHECKF(w->wave.hunt <= 0.0f, "escalation started during the grace period");

    /* Long enough for the ramp to top out. */
    gv_run(w, &in, (int)(30.0f / GV_FIXED_DT));
    GV_CHECKF(w->wave.hunt > 0.9f, "escalation only reached %.2f", w->wave.hunt);

    float end_dist = gv_v2_dist(e->pos, w->player.pos);
    GV_CHECKF(end_dist < start_dist * 0.5f,
              "escalated sentinel kept its distance (%.0f -> %.0f)", start_dist, end_dist);
    free(w);
}

GV_TEST(progress_wave_clear_pays_a_bonus)
{
    gv_world *w = gv_new_world(4004u, GV_DIFF_PILOT);
    GV_CHECK(w != NULL);
    if (!w) return;
    w->player.invuln = 1.0e9f;

    gv_input in = gv_idle_input(w);
    GV_CHECK(gv_run_until_event(w, &in, GV_EV_WAVE_START, 2000));
    /* Let the wave commit, then wipe it. */
    for (int i = 0; i < 4000 && w->wave.state != GV_WAVE_FIGHTING; ++i) {
        gv_world_step(w, &in, GV_FIXED_DT);
        gv_world_clear_events(w);
    }
    for (int i = 0; i < GV_MAX_ENEMIES; ++i) {
        if (w->enemies[i].alive) {
            gv_kill_enemy(w, &w->enemies[i], GV_KILL_CULL, gv_v2_zero(), 0.0f);
        }
    }
    gv_world_clear_events(w);

    int64_t before = w->score.score;
    GV_CHECK(gv_run_until_event(w, &in, GV_EV_WAVE_CLEAR, 2000));
    GV_CHECKF(w->score.score > before, "clearing a wave paid no bonus");
    GV_CHECK_EQ_I(w->wave.state, GV_WAVE_CLEARED);
    free(w);
}

/* ------------------------------------------------------------------ scoring */

GV_TEST(progress_multiplier_climbs_and_decays)
{
    gv_world *w = gv_new_world(4005u, GV_DIFF_PILOT);
    GV_CHECK(w != NULL);
    if (!w) return;
    gv_park_waves(w);
    w->player.invuln = 1.0e9f;

    GV_CHECK_EQ_I(w->score.multiplier, 1);

    for (int i = 0; i < 5; ++i) {
        gv_enemy *e = gv_place_enemy(w, GV_ENEMY_DRONE, gv_v2_make(400.0f, 400.0f));
        GV_CHECK(e != NULL);
        if (!e) break;
        gv_kill_enemy(w, e, GV_KILL_SLAM, gv_v2_make(1.0f, 0.0f), 900.0f);
    }
    GV_CHECKF(w->score.multiplier == 6, "multiplier reached x%d after 5 kills",
              w->score.multiplier);
    GV_CHECK_EQ_I(w->score.combo, 5);
    GV_CHECK(w->score.mult_timer > 0.0f);

    /* Let the chain window lapse. */
    gv_input in = gv_idle_input(w);
    gv_run(w, &in, (int)((GV_MULT_WINDOW + 0.5f) / GV_FIXED_DT));
    GV_CHECKF(w->score.multiplier == 1, "multiplier did not decay (x%d)", w->score.multiplier);
    GV_CHECK_EQ_I(w->score.combo, 0);
    free(w);
}

GV_TEST(progress_multiplier_is_capped)
{
    gv_world *w = gv_new_world(4006u, GV_DIFF_PILOT);
    GV_CHECK(w != NULL);
    if (!w) return;
    gv_park_waves(w);
    w->player.invuln = 1.0e9f;

    for (int i = 0; i < GV_MULT_MAX + 40; ++i) {
        gv_enemy *e = gv_place_enemy(w, GV_ENEMY_DRONE, gv_v2_make(400.0f, 400.0f));
        if (!e) break;
        gv_kill_enemy(w, e, GV_KILL_SLAM, gv_v2_make(1.0f, 0.0f), 900.0f);
        GV_CHECKF(w->score.multiplier <= GV_MULT_MAX, "multiplier exceeded the cap (x%d)",
                  w->score.multiplier);
        if (w->score.multiplier > GV_MULT_MAX) break;
    }
    GV_CHECK_EQ_I(w->score.multiplier, GV_MULT_MAX);
    GV_CHECK(w->score.score > 0);
    free(w);
}

GV_TEST(progress_taking_a_hit_resets_the_chain)
{
    gv_world *w = gv_new_world(4007u, GV_DIFF_PILOT);
    GV_CHECK(w != NULL);
    if (!w) return;
    gv_park_waves(w);
    w->player.invuln = 0.0f;

    for (int i = 0; i < 4; ++i) {
        gv_enemy *e = gv_place_enemy(w, GV_ENEMY_DRONE, gv_v2_make(400.0f, 400.0f));
        if (!e) break;
        gv_kill_enemy(w, e, GV_KILL_SLAM, gv_v2_make(1.0f, 0.0f), 900.0f);
    }
    GV_CHECK(w->score.multiplier > 1);

    gv_damage_player(w, gv_v2_add(w->player.pos, gv_v2_make(20.0f, 0.0f)));
    GV_CHECK_EQ_I(w->score.multiplier, 1);
    GV_CHECK_EQ_I(w->score.combo, 0);
    /* Score already banked is never taken away. */
    GV_CHECK(w->score.score > 0);
    free(w);
}

GV_TEST(progress_score_never_decreases)
{
    gv_world *w = gv_new_world(4008u, GV_DIFF_ACE);
    GV_CHECK(w != NULL);
    if (!w) return;

    gv_rng script;
    gv_rng_seed(&script, 63u, 2u);
    int64_t last = 0;
    for (int i = 0; i < 30000; ++i) {
        gv_input in;
        memset(&in, 0, sizeof(in));
        in.move = gv_v2_polar(gv_rng_angle(&script), gv_rng_f01(&script));
        in.aim_point = gv_v2_make(gv_rng_range_f(&script, 0.0f, GV_ARENA_W),
                                  gv_rng_range_f(&script, 0.0f, GV_ARENA_H));
        in.tether_held = gv_rng_chance(&script, 0.6f);
        in.tether_pressed = gv_rng_chance(&script, 0.03f);
        in.pulse_pressed = gv_rng_chance(&script, 0.01f);
        gv_world_step(w, &in, GV_FIXED_DT);
        gv_world_clear_events(w);

        GV_CHECKF(w->score.score >= last, "score went backwards: %lld -> %lld", (long long)last,
                  (long long)w->score.score);
        if (w->score.score < last) break;
        last = w->score.score;
        if (gv_world_is_over(w)) break;
    }
    GV_CHECK(gv_world_final_score(w) == w->score.score);
    free(w);
}

/* ------------------------------------------------------------------ pickups */

GV_TEST(progress_pickups_are_collected)
{
    gv_world *w = gv_new_world(4009u, GV_DIFF_PILOT);
    GV_CHECK(w != NULL);
    if (!w) return;
    gv_park_waves(w);
    w->player.invuln = 1.0e9f;
    w->player.vel = gv_v2_zero();

    /* Core: pays out and refreshes the chain window. */
    w->score.multiplier = 4;
    int64_t before = w->score.score;
    gv_spawn_pickup(w, GV_PICKUP_CORE, gv_v2_add(w->player.pos, gv_v2_make(20.0f, 0.0f)),
                    gv_v2_zero());
    gv_input in = gv_idle_input(w);
    gv_run(w, &in, 10);
    GV_CHECKF(w->score.score > before, "collecting a core paid nothing");
    GV_CHECK(w->score.mult_timer > 0.0f);
    GV_CHECK_EQ_I(gv_count_live_pickups(w), 0);

    /* Pulse: tops the meter up. */
    w->player.pulse_charge = 0.0f;
    gv_spawn_pickup(w, GV_PICKUP_PULSE, gv_v2_add(w->player.pos, gv_v2_make(20.0f, 0.0f)),
                    gv_v2_zero());
    gv_run(w, &in, 10);
    GV_CHECK(w->player.pulse_charge >= 1.0f);

    /* Life: restores one, up to the cap. */
    w->player.lives = 2;
    gv_spawn_pickup(w, GV_PICKUP_LIFE, gv_v2_add(w->player.pos, gv_v2_make(20.0f, 0.0f)),
                    gv_v2_zero());
    gv_run(w, &in, 10);
    GV_CHECK_EQ_I(w->player.lives, 3);
    free(w);
}

GV_TEST(progress_life_pickup_respects_the_cap)
{
    gv_world *w = gv_new_world(4010u, GV_DIFF_PILOT);
    GV_CHECK(w != NULL);
    if (!w) return;
    gv_park_waves(w);
    w->player.invuln = 1.0e9f;
    w->player.vel = gv_v2_zero();
    w->player.lives = GV_PLAYER_MAX_LIVES;

    int64_t before = w->score.score;
    gv_spawn_pickup(w, GV_PICKUP_LIFE, gv_v2_add(w->player.pos, gv_v2_make(20.0f, 0.0f)),
                    gv_v2_zero());
    gv_input in = gv_idle_input(w);
    gv_run(w, &in, 10);

    GV_CHECKF(w->player.lives == GV_PLAYER_MAX_LIVES, "lives exceeded the cap (%d)",
              w->player.lives);
    /* A wasted life pickup converts to score rather than vanishing. */
    GV_CHECKF(w->score.score > before, "a capped life pickup paid nothing");
    free(w);
}

GV_TEST(progress_pickups_expire)
{
    gv_world *w = gv_new_world(4011u, GV_DIFF_PILOT);
    GV_CHECK(w != NULL);
    if (!w) return;
    gv_park_waves(w);
    w->player.invuln = 1.0e9f;
    /* Far away, so magnetism cannot reach it. */
    w->player.pos = gv_v2_make(100.0f, 100.0f);
    w->player.vel = gv_v2_zero();
    gv_spawn_pickup(w, GV_PICKUP_CORE, gv_v2_make(2400.0f, 1500.0f), gv_v2_zero());
    GV_CHECK_EQ_I(gv_count_live_pickups(w), 1);

    gv_input in = gv_idle_input(w);
    gv_run(w, &in, 120 * 20);
    GV_CHECKF(gv_count_live_pickups(w) == 0, "pickups never expire");
    free(w);
}

/* -------------------------------------------------------------------- death */

GV_TEST(progress_run_ends_after_the_last_life)
{
    gv_world *w = gv_new_world(4012u, GV_DIFF_PILOT);
    GV_CHECK(w != NULL);
    if (!w) return;
    gv_park_waves(w);

    for (int life = 3; life > 0; --life) {
        w->player.invuln = 0.0f;
        GV_CHECK_EQ_I(w->player.lives, life);
        gv_damage_player(w, gv_v2_add(w->player.pos, gv_v2_make(20.0f, 0.0f)));
    }

    GV_CHECK_EQ_I(w->player.lives, 0);
    GV_CHECK(!w->player.alive);
    GV_CHECK_EQ_I(w->run_state, GV_RUN_DYING);
    GV_CHECK(!gv_world_is_over(w));

    /* The death beat plays out, then the run is over and stays over. */
    gv_input in = gv_idle_input(w);
    gv_run(w, &in, 120 * 5);
    GV_CHECK_EQ_I(w->run_state, GV_RUN_GAME_OVER);
    GV_CHECK(gv_world_is_over(w));

    int64_t final = gv_world_final_score(w);
    gv_run(w, &in, 120 * 5);
    GV_CHECKF(gv_world_final_score(w) == final, "score changed after the run ended");
    GV_CHECK(gv_world_is_over(w));
    GV_CHECK(gv_world_all_finite(w));
    free(w);
}

GV_TEST(progress_damage_is_ignored_once_dead)
{
    gv_world *w = gv_new_world(4013u, GV_DIFF_PILOT);
    GV_CHECK(w != NULL);
    if (!w) return;
    gv_park_waves(w);

    for (int i = 0; i < 3; ++i) {
        w->player.invuln = 0.0f;
        gv_damage_player(w, gv_v2_make(0.0f, 0.0f));
    }
    GV_CHECK_EQ_I(w->player.lives, 0);

    /* Further damage must not drive lives negative. */
    for (int i = 0; i < 20; ++i) {
        w->player.invuln = 0.0f;
        gv_damage_player(w, gv_v2_make(0.0f, 0.0f));
    }
    GV_CHECKF(w->player.lives == 0, "lives went to %d after death", w->player.lives);
    free(w);
}

GV_TEST(progress_events_do_not_overflow_in_normal_play)
{
    /* The event queue is fixed-size and drops on overflow. Under real play it
     * must never actually drop, or the audio layer starts missing cues.
     *
     * The queue is drained every step, so the figure that matters is the
     * worst single step — which happens during a warden death or a long chain,
     * states only competent play reaches. Hence the bot rather than fuzz. */
    int peak = 0;
    long total_events = 0;
    uint32_t kinds_seen = 0;

    for (int d = 0; d < GV_DIFF_COUNT; ++d) {
        gv_world *w = gv_new_world(4014u + (uint64_t)d, d);
        GV_CHECK(w != NULL);
        if (!w) return;

        gv_rng script;
        gv_rng_seed(&script, 88u + (uint64_t)d, 6u);
        for (int i = 0; i < 30000; ++i) {
            gv_input in = gv_autopilot_think(w, &script);
            gv_world_step(w, &in, GV_FIXED_DT);
            if (w->event_count > peak) peak = w->event_count;
            total_events += w->event_count;
            for (int e = 0; e < w->event_count; ++e) {
                if (w->events[e].kind < 32) kinds_seen |= 1u << w->events[e].kind;
            }
            GV_CHECKF(!w->events_dropped, "event queue overflowed (peak %d of %d)", peak,
                      GV_MAX_EVENTS);
            if (w->events_dropped) break;
            gv_world_clear_events(w);
            if (gv_world_is_over(w)) gv_world_init(w, (uint64_t)i, d);
        }
        free(w);
    }

    /* Confirm the runs actually exercised the game, so a silent simulation
     * cannot pass this test by producing nothing at all. Breadth of event
     * kinds is the stronger signal here: a raw count can be inflated by one
     * chatty event, whereas hitting most of the table means the bot really did
     * fight, swing, get hit, clear waves and collect drops. */
    int distinct = 0;
    for (int k = 0; k < GV_EV_KIND_COUNT; ++k) {
        if (kinds_seen & (1u << k)) distinct++;
    }
    GV_CHECKF(distinct >= GV_EV_KIND_COUNT - 3, "only %d of %d event kinds occurred", distinct,
              GV_EV_KIND_COUNT);
    GV_CHECKF(total_events > 1000, "only %ld events across three full runs", total_events);

    /* And confirm real headroom remains, rather than merely not overflowing. */
    GV_CHECKF(peak < GV_MAX_EVENTS / 2, "peak burst %d is uncomfortably close to the %d cap",
              peak, GV_MAX_EVENTS);
}

GV_TEST(progress_event_overflow_degrades_safely)
{
    /* If the queue ever does fill, it must drop cleanly and flag itself rather
     * than writing past the array. */
    gv_world *w = gv_new_world(4015u, GV_DIFF_PILOT);
    GV_CHECK(w != NULL);
    if (!w) return;

    for (int i = 0; i < GV_MAX_EVENTS * 3; ++i) {
        gv_emit(w, GV_EV_ENEMY_HIT, gv_v2_make((float)i, 0.0f), 1.0f, i);
    }
    GV_CHECK_EQ_I(w->event_count, GV_MAX_EVENTS);
    GV_CHECK(w->events_dropped);

    /* The retained events must be the first ones, intact and in order. */
    for (int i = 0; i < GV_MAX_EVENTS; ++i) {
        GV_CHECK_EQ_I(w->events[i].a, i);
        if (w->events[i].a != i) break;
    }

    gv_world_clear_events(w);
    GV_CHECK_EQ_I(w->event_count, 0);
    GV_CHECK(!w->events_dropped);

    gv_input in = gv_idle_input(w);
    gv_run(w, &in, 240);
    GV_CHECK(gv_world_all_finite(w));
    free(w);
}
