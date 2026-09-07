/* test_combat.c — the overdrive rule.
 *
 * "Fast enough and you break it, too slow and it breaks you" is the whole
 * combat model, so the threshold, the armour gate and the i-frames around it
 * are pinned down here.
 */
#include "gv_fixture.h"
#include "gv_test.h"

/* Drop the ship in front of `target` moving at `speed`, then step until they
 * meet (or the budget expires). Returns the number of steps taken.
 *
 * The stopping condition is measured against the lives the player had on
 * entry, not the difficulty's starting lives: these scenarios often charge the
 * same target several times, and an already-damaged player would otherwise
 * make every later charge return before it began. */
static int gv_charge_at(gv_world *w, gv_v2 target, float speed, int max_steps)
{
    gv_v2 dir = gv_v2_norm_or(gv_v2_sub(target, w->player.pos), gv_v2_make(1.0f, 0.0f));
    w->player.vel = gv_v2_mul(dir, speed);
    w->player.speed = speed;

    int enemies_before = w->enemy_count;
    int lives_before = w->player.lives;

    gv_input in = gv_idle_input(w);
    in.aim_point = target;

    for (int i = 0; i < max_steps; ++i) {
        gv_world_step(w, &in, GV_FIXED_DT);
        gv_world_clear_events(w);
        if (w->enemy_count < enemies_before || w->player.lives < lives_before) return i;
    }
    return max_steps;
}

/* Launch speed that still clears `arrival` after drag over `distance`.
 * Exponential drag means a charge from across the arena arrives far slower
 * than it left, which is easy to forget when writing a threshold test. */
static float gv_launch_speed_for(float arrival, float distance)
{
    /* Solving the drag integral exactly is unnecessary; over these short
     * distances the average speed is close enough, and erring high is safe. */
    float travel = distance / gv_maxf(arrival, 1.0f);
    return arrival * expf(GV_PLAYER_DRAG * travel) + 4.0f;
}

GV_TEST(combat_overdrive_slam_destroys_a_drone)
{
    gv_world *w = gv_new_world(3001u, GV_DIFF_PILOT);
    GV_CHECK(w != NULL);
    if (!w) return;
    gv_park_waves(w);
    gv_clear_arena(w);
    w->player.invuln = 0.0f;

    gv_v2 target = gv_v2_add(w->player.pos, gv_v2_make(200.0f, 0.0f));
    gv_enemy *e = gv_place_enemy(w, GV_ENEMY_DRONE, target);
    GV_CHECK(e != NULL);
    if (!e) { free(w); return; }
    e->stagger = 100.0f; /* hold it still so the test is about the threshold */

    gv_charge_at(w, target, GV_OVERDRIVE_SPEED + 200.0f, 120);

    GV_CHECKF(!e->alive, "an overdrive slam failed to destroy a drone");
    GV_CHECK_EQ_I(w->player.lives, 3);
    GV_CHECK_EQ_I(w->score.kills, 1);
    GV_CHECK_EQ_I(w->score.slam_kills, 1);
    GV_CHECK(w->score.score > 0);
    free(w);
}

GV_TEST(combat_slow_contact_costs_a_life)
{
    gv_world *w = gv_new_world(3002u, GV_DIFF_PILOT);
    GV_CHECK(w != NULL);
    if (!w) return;
    gv_park_waves(w);
    gv_clear_arena(w);
    w->player.invuln = 0.0f;

    gv_v2 target = gv_v2_add(w->player.pos, gv_v2_make(120.0f, 0.0f));
    gv_enemy *e = gv_place_enemy(w, GV_ENEMY_DRONE, target);
    GV_CHECK(e != NULL);
    if (!e) { free(w); return; }
    e->stagger = 100.0f;

    /* Comfortably below the threshold. */
    gv_charge_at(w, target, GV_OVERDRIVE_SPEED * 0.4f, 200);

    GV_CHECKF(w->player.lives == 2, "slow contact did not cost a life (lives=%d)",
              w->player.lives);
    GV_CHECK(e->alive);
    GV_CHECK(w->player.invuln > 0.0f);
    /* Getting hit resets the chain. */
    GV_CHECK_EQ_I(w->score.multiplier, 1);
    free(w);
}

GV_TEST(combat_threshold_is_exact)
{
    /* Just under the line must bounce; just over must break. An off-by-one
     * here would make the core rule feel arbitrary. */
    for (int over = 0; over < 2; ++over) {
        gv_world *w = gv_new_world(3003u + (uint64_t)over, GV_DIFF_PILOT);
        GV_CHECK(w != NULL);
        if (!w) return;
        gv_park_waves(w);
        gv_clear_arena(w);
        w->player.invuln = 0.0f;

        const float distance = 150.0f;
        gv_v2 target = gv_v2_add(w->player.pos, gv_v2_make(distance, 0.0f));
        gv_enemy *e = gv_place_enemy(w, GV_ENEMY_DRONE, target);
        if (!e) { free(w); return; }
        e->stagger = 100.0f;

        /* Drones have no armour, so the requirement is the bare threshold.
         * What matters is the speed *at contact*, so the launch speed is
         * chosen to land just above or just below it after drag. */
        float arrival = GV_OVERDRIVE_SPEED + (over ? 40.0f : -40.0f);
        gv_charge_at(w, target, gv_launch_speed_for(arrival, distance), 120);

        if (over) {
            GV_CHECKF(!e->alive, "speed above the threshold failed to kill");
            GV_CHECK_EQ_I(w->player.lives, 3);
        } else {
            GV_CHECKF(e->alive, "speed below the threshold still killed");
            GV_CHECK_EQ_I(w->player.lives, 2);
        }
        free(w);
    }
}

GV_TEST(combat_armour_gates_the_warden)
{
    /* A warden needs a much bigger run-up. At merely "fast", the ship should
     * bounce off without dying — the armour gate is a skill check, not a
     * death sentence. */
    gv_world *w = gv_new_world(3005u, GV_DIFF_PILOT);
    GV_CHECK(w != NULL);
    if (!w) return;
    gv_park_waves(w);
    gv_clear_arena(w);
    w->player.invuln = 0.0f;

    gv_v2 target = gv_v2_add(w->player.pos, gv_v2_make(260.0f, 0.0f));
    gv_enemy *e = gv_place_enemy(w, GV_ENEMY_WARDEN, target);
    GV_CHECK(e != NULL);
    if (!e) { free(w); return; }
    e->stagger = 1000.0f;

    float armour = gv_enemy_armour(GV_ENEMY_WARDEN);
    GV_CHECK(armour > 300.0f);

    /* Over the base threshold, under the warden's requirement. */
    gv_charge_at(w, target, gv_launch_speed_for(GV_OVERDRIVE_SPEED + armour * 0.4f, 260.0f), 120);
    GV_CHECKF(e->alive, "a warden died to an under-powered slam");
    GV_CHECKF(w->player.lives == 2, "bouncing off a warden should still cost a life");

    /* Now hit it properly, repeatedly: it has 3 HP. Invulnerability is cleared
     * before each run so the charge actually connects. */
    int hits = 0;
    for (int attempt = 0; attempt < 8 && e->alive; ++attempt) {
        w->player.pos = gv_v2_sub(target, gv_v2_make(300.0f, 0.0f));
        w->player.invuln = 0.0f;
        e->stagger = 1000.0f;
        e->vel = gv_v2_zero();
        e->pos = target;
        float before_hp = e->hp;
        gv_charge_at(w, target, gv_launch_speed_for(GV_OVERDRIVE_SPEED + armour + 220.0f, 300.0f),
                     120);
        if (!e->alive || e->hp < before_hp) hits++;
    }
    GV_CHECKF(hits >= 3, "only %d of the full-power slams connected", hits);
    GV_CHECKF(!e->alive, "a warden survived repeated full-power slams");
    GV_CHECK(w->score.score > 1000);
    free(w);
}

GV_TEST(combat_invulnerability_blocks_repeat_hits)
{
    gv_world *w = gv_new_world(3006u, GV_DIFF_PILOT);
    GV_CHECK(w != NULL);
    if (!w) return;
    gv_park_waves(w);
    gv_clear_arena(w);
    w->player.invuln = 0.0f;
    w->player.vel = gv_v2_zero();

    /* Surround the ship with enemies it cannot break. Exactly one life should
     * be lost, however many are touching. */
    for (int i = 0; i < 6; ++i) {
        float a = (float)i / 6.0f * GV_TAU;
        gv_enemy *e = gv_place_enemy(w, GV_ENEMY_DRONE,
                                     gv_v2_madd(w->player.pos, gv_v2_from_angle(a), 18.0f));
        if (e) e->stagger = 1000.0f;
    }

    gv_input in = gv_idle_input(w);
    gv_world_step(w, &in, GV_FIXED_DT);
    gv_world_clear_events(w);
    GV_CHECK_EQ_I(w->player.lives, 2);

    /* Ride out most of the i-frames: still no further loss. */
    for (int i = 0; i < 100; ++i) {
        gv_world_step(w, &in, GV_FIXED_DT);
        gv_world_clear_events(w);
        GV_CHECKF(w->player.lives == 2, "took a second hit during invulnerability");
        if (w->player.lives != 2) break;
    }
    GV_CHECK(w->player.invuln > 0.0f);
    free(w);
}

GV_TEST(combat_spawn_grace_protects_the_player)
{
    gv_world *w = gv_new_world(3007u, GV_DIFF_PILOT);
    GV_CHECK(w != NULL);
    if (!w) return;
    gv_park_waves(w);
    gv_clear_arena(w);

    GV_CHECK(w->player.invuln >= GV_PLAYER_SPAWN_GRACE - 1e-3f);
    for (int i = 0; i < 4; ++i) {
        gv_enemy *e = gv_place_enemy(w, GV_ENEMY_DRONE,
                                     gv_v2_add(w->player.pos, gv_v2_make(15.0f * (float)i, 0.0f)));
        if (e) e->stagger = 1000.0f;
    }

    gv_input in = gv_idle_input(w);
    /* Slightly less than the full grace period. */
    gv_run(w, &in, (int)(GV_PLAYER_SPAWN_GRACE * 0.8f / GV_FIXED_DT));
    GV_CHECKF(w->player.lives == 3, "lost a life during the spawn grace period");
    free(w);
}

GV_TEST(combat_chains_through_a_line_of_drones)
{
    /* Ploughing through a row of fragile enemies is the game's core fantasy.
     * If speed bleed were too aggressive the chain would stall after one or
     * two, and the multiplier would be unreachable. */
    gv_world *w = gv_new_world(3008u, GV_DIFF_PILOT);
    GV_CHECK(w != NULL);
    if (!w) return;
    gv_park_waves(w);
    w->player.invuln = 1000.0f; /* isolate the chain from incidental contact */
    gv_clear_arena(w);

    w->player.pos = gv_v2_make(200.0f, w->arena_h * 0.5f);
    const int line = 8;
    for (int i = 0; i < line; ++i) {
        gv_enemy *e = gv_place_enemy(
            w, GV_ENEMY_DRONE,
            gv_v2_make(500.0f + 140.0f * (float)i, w->arena_h * 0.5f));
        if (e) e->stagger = 1000.0f;
    }
    GV_CHECK_EQ_I(w->enemy_count, line);

    w->player.vel = gv_v2_make(1500.0f, 0.0f);
    w->player.speed = 1500.0f;

    gv_input in = gv_idle_input(w);
    in.move = gv_v2_make(1.0f, 0.0f);
    for (int i = 0; i < 300 && w->enemy_count > 0; ++i) {
        in.aim_point = gv_v2_add(w->player.pos, gv_v2_make(100.0f, 0.0f));
        gv_world_step(w, &in, GV_FIXED_DT);
        gv_world_clear_events(w);
    }

    GV_CHECKF(w->score.kills >= line, "chain stalled after %d of %d drones", w->score.kills, line);
    GV_CHECKF(w->score.multiplier > 4, "chaining %d kills only reached x%d", w->score.kills,
              w->score.multiplier);
    GV_CHECK(w->score.best_combo >= line);
    free(w);
}

GV_TEST(combat_rock_whip_kills_and_scores_double)
{
    gv_world *w = gv_new_world(3009u, GV_DIFF_PILOT);
    GV_CHECK(w != NULL);
    if (!w) return;
    gv_park_waves(w);
    w->player.invuln = 1000.0f;
    gv_clear_arena(w);

    /* Park the ship far away so only the rock can do the killing. */
    w->player.pos = gv_v2_make(200.0f, 200.0f);
    w->player.vel = gv_v2_zero();

    gv_v2 rock_pos = gv_v2_make(1000.0f, 1000.0f);
    gv_v2 enemy_pos = gv_v2_make(1400.0f, 1000.0f);
    gv_rock *r = gv_spawn_rock(w, rock_pos, 28.0f);
    gv_enemy *e = gv_place_enemy(w, GV_ENEMY_DRONE, enemy_pos);
    GV_CHECK(r && e);
    if (!r || !e) { free(w); return; }
    e->stagger = 1000.0f;

    /* Fling the rock hard enough to be lethal. */
    r->vel = gv_v2_make(900.0f, 0.0f);

    gv_input in = gv_idle_input(w);
    gv_run(w, &in, 120);

    GV_CHECKF(!e->alive, "a fast rock failed to destroy a drone");
    GV_CHECK_EQ_I(w->score.whip_kills, 1);
    GV_CHECK_EQ_I(w->score.slam_kills, 0);
    /* Whip kills pay double the base value at x1 multiplier. */
    GV_CHECKF(w->score.score >= (int64_t)(gv_enemy_base_score(GV_ENEMY_DRONE) * 2.0f),
              "whip kill scored only %lld", (long long)w->score.score);
    free(w);
}

GV_TEST(combat_slow_rock_is_harmless)
{
    gv_world *w = gv_new_world(3010u, GV_DIFF_PILOT);
    GV_CHECK(w != NULL);
    if (!w) return;
    gv_park_waves(w);
    w->player.invuln = 1000.0f;
    gv_clear_arena(w);
    w->player.pos = gv_v2_make(200.0f, 200.0f);
    w->player.vel = gv_v2_zero();

    gv_rock *r = gv_spawn_rock(w, gv_v2_make(1000.0f, 1000.0f), 28.0f);
    gv_enemy *e = gv_place_enemy(w, GV_ENEMY_DRONE, gv_v2_make(1120.0f, 1000.0f));
    GV_CHECK(r && e);
    if (!r || !e) { free(w); return; }
    e->stagger = 1000.0f;
    r->vel = gv_v2_make(90.0f, 0.0f); /* drifting, well under the lethal speed */

    gv_input in = gv_idle_input(w);
    gv_run(w, &in, 240);

    GV_CHECKF(e->alive, "a drifting rock killed a drone");
    GV_CHECK_EQ_I(w->score.kills, 0);
    free(w);
}

GV_TEST(combat_rocks_never_hurt_the_player)
{
    /* Rocks are the player's tool. Making them harmless is what allows the
     * fling-them-around fantasy to work without punishing the player for it. */
    gv_world *w = gv_new_world(3011u, GV_DIFF_PILOT);
    GV_CHECK(w != NULL);
    if (!w) return;
    gv_park_waves(w);
    w->player.invuln = 0.0f;
    gv_clear_arena(w);

    w->player.pos = gv_v2_make(1000.0f, 1000.0f);
    w->player.vel = gv_v2_zero();
    gv_rock *r = gv_spawn_rock(w, gv_v2_make(1400.0f, 1000.0f), 40.0f);
    GV_CHECK(r != NULL);
    if (!r) { free(w); return; }
    r->vel = gv_v2_make(-2000.0f, 0.0f); /* straight at the ship, as fast as rocks go */

    gv_input in = gv_idle_input(w);
    gv_run(w, &in, 240);
    GV_CHECKF(w->player.lives == 3, "a rock cost the player a life");
    GV_CHECK(gv_world_all_finite(w));
    free(w);
}

/* --------------------------------------------------------------- projectiles */

GV_TEST(combat_bullets_hurt_the_player)
{
    gv_world *w = gv_new_world(3012u, GV_DIFF_PILOT);
    GV_CHECK(w != NULL);
    if (!w) return;
    gv_park_waves(w);
    w->player.invuln = 0.0f;
    w->player.vel = gv_v2_zero();

    gv_bullet *b = gv_spawn_bullet(w, gv_v2_sub(w->player.pos, gv_v2_make(100.0f, 0.0f)),
                                   gv_v2_make(400.0f, 0.0f), 5.0f, 4.0f);
    GV_CHECK(b != NULL);

    gv_input in = gv_idle_input(w);
    gv_run(w, &in, 60);
    GV_CHECKF(w->player.lives == 2, "a bullet did not cost a life");
    GV_CHECK(gv_count_live_bullets(w) == 0);
    free(w);
}

GV_TEST(combat_rocks_and_pylons_block_bullets)
{
    /* Cover is a real tactic, so shots must actually stop at solid geometry. */
    gv_world *w = gv_new_world(3013u, GV_DIFF_PILOT);
    GV_CHECK(w != NULL);
    if (!w) return;
    gv_park_waves(w);
    gv_clear_arena(w);

    gv_rock *r = gv_spawn_rock(w, gv_v2_make(900.0f, 400.0f), 40.0f);
    GV_CHECK(r != NULL);
    if (!r) { free(w); return; }
    r->vel = gv_v2_zero();
    gv_spawn_bullet(w, gv_v2_make(700.0f, 400.0f), gv_v2_make(500.0f, 0.0f), 5.0f, 4.0f);

    gv_input in = gv_idle_input(w);
    gv_run(w, &in, 90);
    GV_CHECKF(gv_count_live_bullets(w) == 0, "a bullet passed through a rock");

    w->pylons[0].alive = true;
    w->pylons[0].pos = gv_v2_make(900.0f, 900.0f);
    gv_spawn_bullet(w, gv_v2_make(700.0f, 900.0f), gv_v2_make(500.0f, 0.0f), 5.0f, 4.0f);
    gv_run(w, &in, 90);
    GV_CHECKF(gv_count_live_bullets(w) == 0, "a bullet passed through a pylon");
    free(w);
}

GV_TEST(combat_bullets_expire_and_leave_the_arena)
{
    gv_world *w = gv_new_world(3014u, GV_DIFF_PILOT);
    GV_CHECK(w != NULL);
    if (!w) return;
    gv_park_waves(w);
    gv_clear_arena(w);
    w->player.pos = gv_v2_make(50.0f, 50.0f);

    /* One heading out of bounds, one that simply times out. */
    gv_spawn_bullet(w, gv_v2_make(1200.0f, 800.0f), gv_v2_make(3000.0f, 0.0f), 5.0f, 100.0f);
    gv_spawn_bullet(w, gv_v2_make(1200.0f, 800.0f), gv_v2_zero(), 5.0f, 0.5f);
    GV_CHECK_EQ_I(gv_count_live_bullets(w), 2);

    gv_input in = gv_idle_input(w);
    gv_run(w, &in, 240);
    GV_CHECKF(gv_count_live_bullets(w) == 0, "bullets leaked: %d still alive",
              gv_count_live_bullets(w));
    free(w);
}

/* --------------------------------------------------------------------- pulse */

GV_TEST(combat_pulse_clears_bullets_and_shoves_enemies)
{
    gv_world *w = gv_new_world(3015u, GV_DIFF_PILOT);
    GV_CHECK(w != NULL);
    if (!w) return;
    gv_park_waves(w);
    gv_clear_arena(w);
    w->player.invuln = 1000.0f;
    w->player.vel = gv_v2_zero();

    for (int i = 0; i < 5; ++i) {
        gv_spawn_bullet(w, gv_v2_madd(w->player.pos, gv_v2_from_angle((float)i), 80.0f),
                        gv_v2_zero(), 5.0f, 10.0f);
    }
    gv_enemy *e = gv_place_enemy(w, GV_ENEMY_DRONE,
                                 gv_v2_add(w->player.pos, gv_v2_make(90.0f, 0.0f)));
    GV_CHECK(e != NULL);
    if (!e) { free(w); return; }
    e->vel = gv_v2_zero();

    float charge_before = w->player.pulse_charge;
    GV_CHECK(charge_before >= 1.0f);

    gv_input in = gv_idle_input(w);
    in.pulse_pressed = true;
    gv_world_step(w, &in, GV_FIXED_DT);
    GV_CHECK(gv_saw_event(w, GV_EV_PULSE));
    gv_world_clear_events(w);

    GV_CHECKF(gv_count_live_bullets(w) == 0, "pulse left %d bullets alive",
              gv_count_live_bullets(w));
    GV_CHECKF(w->player.pulse_charge < charge_before, "pulse did not consume a charge");
    GV_CHECKF(gv_v2_len(e->vel) > 200.0f, "pulse barely moved a nearby enemy (%.1f px/s)",
              gv_v2_len(e->vel));
    /* A pulse must never kill: it is an escape tool, not a weapon. */
    GV_CHECK(e->alive);
    GV_CHECK_EQ_I(w->score.kills, 0);
    free(w);
}

GV_TEST(combat_pulse_runs_out_and_recharges)
{
    gv_world *w = gv_new_world(3016u, GV_DIFF_PILOT);
    GV_CHECK(w != NULL);
    if (!w) return;
    gv_park_waves(w);
    gv_clear_arena(w);

    /* Spend every charge. */
    int fired = 0;
    for (int i = 0; i < 400; ++i) {
        gv_input in = gv_idle_input(w);
        in.pulse_pressed = true;
        gv_world_step(w, &in, GV_FIXED_DT);
        if (gv_saw_event(w, GV_EV_PULSE)) fired++;
        gv_world_clear_events(w);
        /* Let the inter-shot cooldown lapse. */
        gv_input idle = gv_idle_input(w);
        gv_run(w, &idle, 45);
        if (w->player.pulse_charge < 1.0f) break;
    }
    GV_CHECKF(fired >= 3, "only fired %d pulses from a full meter", fired);
    GV_CHECK(w->player.pulse_charge < 1.0f);

    /* Firing on empty reports rather than silently doing nothing. */
    gv_input in = gv_idle_input(w);
    in.pulse_pressed = true;
    gv_world_step(w, &in, GV_FIXED_DT);
    GV_CHECK(gv_saw_event(w, GV_EV_PULSE_EMPTY));
    GV_CHECK(!gv_saw_event(w, GV_EV_PULSE));
    gv_world_clear_events(w);

    /* And it comes back over time, without ever exceeding the cap. */
    gv_input idle = gv_idle_input(w);
    gv_run(w, &idle, 120 * 60);
    GV_CHECK(w->player.pulse_charge >= 1.0f);
    GV_CHECKF(w->player.pulse_charge <= GV_PULSE_MAX + 1e-4f, "pulse charge overflowed to %.2f",
              w->player.pulse_charge);
    free(w);
}

GV_TEST(combat_focus_drains_and_refills)
{
    gv_world *w = gv_new_world(3017u, GV_DIFF_PILOT);
    GV_CHECK(w != NULL);
    if (!w) return;
    gv_park_waves(w);
    gv_clear_arena(w);

    GV_CHECK_NEAR(w->player.focus, GV_FOCUS_MAX, 1e-4);
    GV_CHECK_NEAR(w->time_scale, 1.0f, 1e-4);

    gv_input in = gv_idle_input(w);
    in.focus_held = true;
    gv_run(w, &in, 60);
    GV_CHECK(w->player.focus_active);
    GV_CHECKF(w->time_scale < 1.0f, "focus did not slow time (scale %.2f)", w->time_scale);
    GV_CHECK(w->player.focus < GV_FOCUS_MAX);

    /* Hold until empty: focus must disengage, not go negative. */
    gv_run(w, &in, 1200);
    GV_CHECKF(w->player.focus >= 0.0f, "focus went negative (%.3f)", w->player.focus);
    GV_CHECK(!w->player.focus_active);
    GV_CHECK_NEAR(w->time_scale, 1.0f, 1e-4);

    /* An empty meter cannot be chattered back on for a permanent slowdown. */
    for (int i = 0; i < 40; ++i) {
        gv_input tap = gv_idle_input(w);
        tap.focus_held = (i % 2) == 0;
        gv_world_step(w, &tap, GV_FIXED_DT);
        gv_world_clear_events(w);
        GV_CHECKF(!w->player.focus_active || w->player.focus > 0.0f,
                  "focus re-engaged on an empty meter");
    }

    gv_input idle = gv_idle_input(w);
    gv_run(w, &idle, 120 * 10);
    GV_CHECKF(w->player.focus <= GV_FOCUS_MAX + 1e-4f, "focus overfilled to %.3f",
              w->player.focus);
    GV_CHECK(w->player.focus > 0.5f);
    free(w);
}

/* ------------------------------------------------------------------ splitting */

GV_TEST(combat_splitter_spawns_bounded_children)
{
    gv_world *w = gv_new_world(3018u, GV_DIFF_PILOT);
    GV_CHECK(w != NULL);
    if (!w) return;
    gv_park_waves(w);
    gv_clear_arena(w);
    w->player.invuln = 1000.0f;

    gv_enemy *e = gv_place_enemy(w, GV_ENEMY_SPLITTER,
                                 gv_v2_add(w->player.pos, gv_v2_make(400.0f, 0.0f)));
    GV_CHECK(e != NULL);
    if (!e) { free(w); return; }

    gv_kill_enemy(w, e, GV_KILL_SLAM, gv_v2_make(1.0f, 0.0f), 900.0f);
    GV_CHECKF(w->enemy_count == 3, "splitter produced %d children, expected 3", w->enemy_count);

    /* Kill every child too: the split must not recurse. */
    for (int guard = 0; guard < 10 && w->enemy_count > 0; ++guard) {
        for (int i = 0; i < GV_MAX_ENEMIES; ++i) {
            if (w->enemies[i].alive) {
                gv_kill_enemy(w, &w->enemies[i], GV_KILL_SLAM, gv_v2_make(1.0f, 0.0f), 900.0f);
            }
        }
    }
    GV_CHECKF(w->enemy_count == 0, "splitting recursed: %d enemies remain", w->enemy_count);
    GV_CHECK_EQ_I(gv_world_alive_enemies(w), 0);
    free(w);
}

GV_TEST(combat_enemy_count_stays_consistent)
{
    /* enemy_count is maintained incrementally for speed; if it ever drifts
     * from the truth, waves stop clearing. */
    gv_world *w = gv_new_world(3019u, GV_DIFF_PILOT);
    GV_CHECK(w != NULL);
    if (!w) return;

    gv_rng script;
    gv_rng_seed(&script, 21u, 8u);
    for (int i = 0; i < 20000; ++i) {
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

        if ((i % 97) == 0) {
            int actual = gv_world_alive_enemies(w);
            GV_CHECKF(actual == w->enemy_count, "enemy_count drifted: cached %d, actual %d",
                      w->enemy_count, actual);
            if (actual != w->enemy_count) break;
        }
    }
    free(w);
}

GV_TEST(combat_enemy_pool_exhaustion_is_safe)
{
    /* Fill the pool completely, then keep asking for more. Spawns must fail
     * cleanly rather than overrunning the array. */
    gv_world *w = gv_new_world(3020u, GV_DIFF_PILOT);
    GV_CHECK(w != NULL);
    if (!w) return;
    gv_park_waves(w);
    gv_clear_arena(w);

    int spawned = 0;
    for (int i = 0; i < GV_MAX_ENEMIES + 64; ++i) {
        gv_v2 pos = gv_v2_make(200.0f + (float)(i % 40) * 55.0f,
                               200.0f + (float)(i / 40) * 55.0f);
        if (gv_spawn_enemy(w, GV_ENEMY_DRONE, pos, 0)) spawned++;
    }
    GV_CHECK_EQ_I(spawned, GV_MAX_ENEMIES);
    GV_CHECK_EQ_I(w->enemy_count, GV_MAX_ENEMIES);
    GV_CHECK_EQ_I(gv_world_alive_enemies(w), GV_MAX_ENEMIES);

    /* An out-of-range kind is rejected, not indexed. */
    GV_CHECK(gv_spawn_enemy(w, -1, gv_v2_zero(), 0) == NULL);
    GV_CHECK(gv_spawn_enemy(w, GV_ENEMY_KIND_COUNT, gv_v2_zero(), 0) == NULL);
    GV_CHECK(gv_spawn_pickup(w, -1, gv_v2_zero(), gv_v2_zero()) == NULL);
    GV_CHECK(gv_spawn_pickup(w, GV_PICKUP_KIND_COUNT, gv_v2_zero(), gv_v2_zero()) == NULL);

    /* A fully saturated arena must still simulate without blowing up. */
    w->player.invuln = 1000.0f;
    gv_input in = gv_idle_input(w);
    gv_run(w, &in, 600);
    GV_CHECK(gv_world_all_finite(w));
    GV_CHECK_EQ_I(gv_world_alive_enemies(w), w->enemy_count);
    free(w);
}

GV_TEST(combat_other_pools_saturate_safely)
{
    gv_world *w = gv_new_world(3021u, GV_DIFF_PILOT);
    GV_CHECK(w != NULL);
    if (!w) return;
    gv_park_waves(w);
    gv_clear_arena(w);

    for (int i = 0; i < GV_MAX_BULLETS + 32; ++i) {
        gv_spawn_bullet(w, gv_v2_make(100.0f, 100.0f), gv_v2_make(10.0f, 0.0f), 4.0f, 30.0f);
    }
    GV_CHECK_EQ_I(gv_count_live_bullets(w), GV_MAX_BULLETS);

    for (int i = 0; i < GV_MAX_PICKUPS + 32; ++i) {
        gv_spawn_pickup(w, GV_PICKUP_CORE, gv_v2_make(2000.0f, 1400.0f), gv_v2_zero());
    }
    GV_CHECK_EQ_I(gv_count_live_pickups(w), GV_MAX_PICKUPS);

    for (int i = 0; i < GV_MAX_ROCKS + 32; ++i) {
        gv_spawn_rock(w, gv_v2_make(500.0f, 500.0f), 20.0f);
    }

    /* Far more particles and floaters than the pools hold. */
    for (int i = 0; i < 400; ++i) {
        gv_fx_burst(w, gv_v2_make(600.0f, 600.0f), gv_v2_zero(), 40, 300.0f, 255, 255, 255,
                    GV_PART_SPARK);
        gv_fx_shockwave(w, gv_v2_make(600.0f, 600.0f), 200.0f, 0.5f, 3.0f, 255, 255, 255);
        gv_fx_floater(w, gv_v2_make(600.0f, 600.0f), i, 255, 255, 255);
    }
    GV_CHECK(gv_count_live_particles(w) <= GV_MAX_PARTICLES);

    w->player.invuln = 1000.0f;
    gv_input in = gv_idle_input(w);
    gv_run(w, &in, 900);
    GV_CHECK(gv_world_all_finite(w));
    free(w);
}
