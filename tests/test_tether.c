/* test_tether.c — the grapple is the game's whole movement system, so its
 * state machine and its rope constraint get their own suite. */
#include "gv_fixture.h"
#include "gv_test.h"

/* Build an isolated pendulum: one anchor at the arena centre, nothing else.
 * Centring it guarantees the swing arc clears the walls whatever the arena
 * dimensions become. */
static gv_pylon *gv_rig_pendulum(gv_world *w)
{
    gv_clear_arena(w);
    gv_pylon *anchor = &w->pylons[0];
    anchor->alive = true;
    anchor->tethered = false;
    anchor->radius = 20.0f;
    anchor->pos = gv_v2_make(w->arena_w * 0.5f, w->arena_h * 0.5f);
    return anchor;
}

/* Put the ship next to a pylon and fire at it. Returns the pylon used. */
static const gv_pylon *gv_attach_to_pylon(gv_world *w, int *out_steps)
{
    const gv_pylon *py = gv_nearest_pylon(w);
    if (!py) return NULL;

    /* Sit at a comfortable rope length from the anchor. */
    w->player.pos = gv_v2_madd(py->pos, gv_v2_make(1.0f, 0.0f), 300.0f);
    w->player.vel = gv_v2_zero();

    gv_input in = gv_idle_input(w);
    in.aim_point = py->pos;
    in.tether_pressed = true;
    in.tether_held = true;

    int steps = 0;
    for (; steps < 200; ++steps) {
        gv_world_step(w, &in, GV_FIXED_DT);
        gv_world_clear_events(w);
        in.tether_pressed = false;
        if (w->player.tether.state == GV_TETHER_ATTACHED) break;
    }
    if (out_steps) *out_steps = steps;
    return py;
}

GV_TEST(tether_attaches_to_a_pylon)
{
    gv_world *w = gv_new_world(2001u, GV_DIFF_PILOT);
    GV_CHECK(w != NULL);
    if (!w) return;
    gv_park_waves(w);

    int steps = 0;
    const gv_pylon *py = gv_attach_to_pylon(w, &steps);
    GV_CHECK(py != NULL);
    GV_CHECK_EQ_I(w->player.tether.state, GV_TETHER_ATTACHED);
    GV_CHECK_EQ_I(w->player.tether.anchor_kind, GV_ANCHOR_PYLON);
    /* The hook travels at 3100 px/s, so 300 px should take well under 20 steps. */
    GV_CHECKF(steps < 30, "hook took %d steps to cross 300 px", steps);
    GV_CHECK(w->player.tether.length >= GV_TETHER_MIN_LEN);
    GV_CHECK(w->player.tether.length <= GV_TETHER_MAX_LEN);
    free(w);
}

GV_TEST(tether_rope_is_inextensible)
{
    gv_world *w = gv_new_world(2002u, GV_DIFF_PILOT);
    GV_CHECK(w != NULL);
    if (!w) return;
    gv_park_waves(w);

    gv_rig_pendulum(w);
    const gv_pylon *py = gv_attach_to_pylon(w, NULL);
    GV_CHECK(py != NULL);
    if (!py || w->player.tether.state != GV_TETHER_ATTACHED) { free(w); return; }

    /* Thrust hard directly away from the anchor for several seconds. The rope
     * must hold: this is the constraint's worst case. */
    gv_input in = gv_idle_input(w);
    in.tether_held = true;
    float worst = 0.0f;
    for (int i = 0; i < 900; ++i) {
        gv_v2 away = gv_v2_norm_or(gv_v2_sub(w->player.pos, py->pos), gv_v2_make(1.0f, 0.0f));
        in.move = away;
        in.aim_point = gv_v2_add(w->player.pos, away);
        gv_world_step(w, &in, GV_FIXED_DT);
        gv_world_clear_events(w);
        if (w->player.tether.state != GV_TETHER_ATTACHED) break;

        float d = gv_v2_dist(w->player.pos, py->pos);
        float over = d - w->player.tether.length;
        if (over > worst) worst = over;
    }
    GV_CHECK_EQ_I(w->player.tether.state, GV_TETHER_ATTACHED);
    /* A little stretch is inherent to a soft positional solver; a lot means
     * the rope is not doing its job. */
    GV_CHECKF(worst < 30.0f, "rope stretched %.1f px past its length", worst);
    free(w);
}

GV_TEST(tether_swing_conserves_energy)
{
    gv_world *w = gv_new_world(2003u, GV_DIFF_PILOT);
    GV_CHECK(w != NULL);
    if (!w) return;
    gv_park_waves(w);

    gv_pylon *anchor = gv_rig_pendulum(w);
    const gv_pylon *py = gv_attach_to_pylon(w, NULL);
    GV_CHECK(py == anchor);
    if (w->player.tether.state != GV_TETHER_ATTACHED) { free(w); return; }

    /* Confirm the arc really is clear of the walls, so a future arena resize
     * cannot silently reintroduce the wall bounce this test was written for. */
    float arc = w->player.tether.length + w->player.radius;
    GV_CHECK(anchor->pos.x - arc > 0.0f && anchor->pos.x + arc < w->arena_w);
    GV_CHECK(anchor->pos.y - arc > 0.0f && anchor->pos.y + arc < w->arena_h);

    /* Launch tangentially and coast: no thrust, no reeling. A frictionless
     * pendulum would hold its speed exactly; this ship also has drag, so the
     * reference is the analytic drag curve rather than a constant.
     *
     * The upper bound is the assertion that matters. A positional constraint
     * solver that injects energy would make the entire game unstable, and the
     * failure mode — a ship that accelerates by holding still on a rope — is
     * exactly the kind of exploit players find first. */
    gv_v2 radial = gv_v2_norm_or(gv_v2_sub(w->player.pos, py->pos), gv_v2_make(1.0f, 0.0f));
    const float start_speed = 700.0f;
    w->player.vel = gv_v2_mul(gv_v2_perp(radial), start_speed);

    gv_input in = gv_idle_input(w);
    in.tether_held = true;

    const int steps = 240; /* 2 seconds */
    float peak = 0.0f;
    for (int i = 0; i < steps; ++i) {
        in.aim_point = gv_v2_add(w->player.pos, gv_v2_make(1.0f, 0.0f));
        gv_world_step(w, &in, GV_FIXED_DT);
        gv_world_clear_events(w);
        float speed = gv_v2_len(w->player.vel);
        if (speed > peak) peak = speed;
        GV_CHECK_FINITE(speed);
        GV_CHECK(w->player.tether.state == GV_TETHER_ATTACHED);
    }

    GV_CHECKF(peak <= start_speed * 1.02f, "swing injected energy: %.1f -> %.1f", start_speed,
              peak);

    /* Drag alone predicts this much decay over the window. */
    float elapsed = (float)steps * GV_FIXED_DT;
    float expected = start_speed * expf(-GV_PLAYER_DRAG * elapsed);
    float actual = gv_v2_len(w->player.vel);
    GV_CHECKF(actual > expected * 0.70f,
              "constraint bled the swing dry: %.1f, drag alone predicts %.1f", actual, expected);
    GV_CHECKF(actual < expected * 1.05f, "swing outran the drag curve: %.1f vs %.1f", actual,
              expected);
    free(w);
}

GV_TEST(tether_reel_shortens_within_limits)
{
    gv_world *w = gv_new_world(2004u, GV_DIFF_PILOT);
    GV_CHECK(w != NULL);
    if (!w) return;
    gv_park_waves(w);

    gv_rig_pendulum(w);
    const gv_pylon *py = gv_attach_to_pylon(w, NULL);
    GV_CHECK(py != NULL);
    if (!py || w->player.tether.state != GV_TETHER_ATTACHED) { free(w); return; }

    float initial = w->player.tether.length;
    gv_input in = gv_idle_input(w);
    in.tether_held = true;
    in.reel = 1.0f;

    for (int i = 0; i < 1200; ++i) {
        gv_world_step(w, &in, GV_FIXED_DT);
        gv_world_clear_events(w);
        if (w->player.tether.state != GV_TETHER_ATTACHED) break;
        GV_CHECKF(w->player.tether.length >= GV_TETHER_MIN_LEN - 1e-3f,
                  "rope reeled past the minimum: %.2f", w->player.tether.length);
        GV_CHECKF(w->player.tether.length <= GV_TETHER_MAX_LEN + 1e-3f,
                  "rope exceeded the maximum: %.2f", w->player.tether.length);
    }
    GV_CHECK(w->player.tether.length < initial);
    GV_CHECK_NEAR(w->player.tether.length, GV_TETHER_MIN_LEN, 1.0f);

    /* Paying out again returns to the maximum and stops there. */
    in.reel = -1.0f;
    for (int i = 0; i < 3000; ++i) {
        gv_world_step(w, &in, GV_FIXED_DT);
        gv_world_clear_events(w);
        if (w->player.tether.state != GV_TETHER_ATTACHED) break;
        GV_CHECK(w->player.tether.length <= GV_TETHER_MAX_LEN + 1e-3f);
    }
    free(w);
}

GV_TEST(tether_reel_in_builds_speed)
{
    /* Angular momentum is conserved as the rope shortens, which is the main
     * skill expression in the game: reel in on a swing and you accelerate. */
    gv_world *w = gv_new_world(2005u, GV_DIFF_PILOT);
    GV_CHECK(w != NULL);
    if (!w) return;
    gv_park_waves(w);

    gv_rig_pendulum(w);
    const gv_pylon *py = gv_attach_to_pylon(w, NULL);
    GV_CHECK(py != NULL);
    if (!py || w->player.tether.state != GV_TETHER_ATTACHED) { free(w); return; }

    gv_v2 radial = gv_v2_norm_or(gv_v2_sub(w->player.pos, py->pos), gv_v2_make(1.0f, 0.0f));
    w->player.vel = gv_v2_mul(gv_v2_perp(radial), 400.0f);

    gv_input in = gv_idle_input(w);
    in.tether_held = true;
    in.reel = 1.0f;

    float peak = 0.0f;
    for (int i = 0; i < 400; ++i) {
        in.aim_point = gv_v2_add(w->player.pos, gv_v2_make(1.0f, 0.0f));
        gv_world_step(w, &in, GV_FIXED_DT);
        gv_world_clear_events(w);
        if (w->player.speed > peak) peak = w->player.speed;
    }
    GV_CHECKF(peak > 450.0f, "reeling in did not accelerate the ship (peak %.1f)", peak);
    GV_CHECKF(peak <= GV_PLAYER_MAX_SPEED + 1.0f, "reeling in broke the speed cap (%.1f)", peak);
    free(w);
}

GV_TEST(tether_missed_shot_retracts_to_idle)
{
    gv_world *w = gv_new_world(2006u, GV_DIFF_PILOT);
    GV_CHECK(w != NULL);
    if (!w) return;
    gv_park_waves(w);

    /* Clear the arena of anything the hook could catch, then fire into space. */
    for (int i = 0; i < GV_MAX_ROCKS; ++i) w->rocks[i].alive = false;
    for (int i = 0; i < w->pylon_count; ++i) w->pylons[i].alive = false;

    gv_input in = gv_idle_input(w);
    in.aim_point = gv_v2_add(w->player.pos, gv_v2_make(1.0f, 0.0f));
    in.tether_pressed = true;
    in.tether_held = true;

    bool saw_flying = false, saw_miss = false;
    for (int i = 0; i < 600; ++i) {
        gv_world_step(w, &in, GV_FIXED_DT);
        if (gv_saw_event(w, GV_EV_TETHER_MISS)) saw_miss = true;
        gv_world_clear_events(w);
        in.tether_pressed = false;
        if (w->player.tether.state == GV_TETHER_FLYING) saw_flying = true;
        if (saw_flying && w->player.tether.state == GV_TETHER_IDLE) break;
    }
    GV_CHECK(saw_flying);
    GV_CHECK(saw_miss);
    GV_CHECK_EQ_I(w->player.tether.state, GV_TETHER_IDLE);
    GV_CHECK_EQ_I(w->player.tether.anchor_kind, GV_ANCHOR_NONE);
    free(w);
}

GV_TEST(tether_releases_when_button_released)
{
    gv_world *w = gv_new_world(2007u, GV_DIFF_PILOT);
    GV_CHECK(w != NULL);
    if (!w) return;
    gv_park_waves(w);

    const gv_pylon *py = gv_attach_to_pylon(w, NULL);
    GV_CHECK(py != NULL);
    if (!py || w->player.tether.state != GV_TETHER_ATTACHED) { free(w); return; }

    gv_input in = gv_idle_input(w);
    in.tether_held = false;
    gv_world_step(w, &in, GV_FIXED_DT);
    GV_CHECK(gv_saw_event(w, GV_EV_TETHER_RELEASE));
    gv_world_clear_events(w);
    GV_CHECK(w->player.tether.state != GV_TETHER_ATTACHED);
    GV_CHECK_EQ_I(w->player.tether.anchor_kind, GV_ANCHOR_NONE);
    free(w);
}

GV_TEST(tether_release_boost_is_bounded)
{
    /* The release boost must never become a speed exploit: repeatedly tapping
     * the tether should not ratchet the ship's speed upward. */
    gv_world *w = gv_new_world(2008u, GV_DIFF_PILOT);
    GV_CHECK(w != NULL);
    if (!w) return;
    gv_park_waves(w);

    gv_rig_pendulum(w);
    const gv_pylon *py = gv_attach_to_pylon(w, NULL);
    GV_CHECK(py != NULL);
    if (!py || w->player.tether.state != GV_TETHER_ATTACHED) { free(w); return; }

    gv_v2 radial = gv_v2_norm_or(gv_v2_sub(w->player.pos, py->pos), gv_v2_make(1.0f, 0.0f));
    w->player.vel = gv_v2_mul(gv_v2_perp(radial), 500.0f);

    /* Swing long enough for the release bonus to reach full strength,
     * otherwise the cap is never approached and the test proves nothing. */
    gv_input hold = gv_idle_input(w);
    hold.tether_held = true;
    for (int i = 0; i < 90; ++i) {
        hold.aim_point = gv_v2_add(w->player.pos, gv_v2_make(1.0f, 0.0f));
        gv_world_step(w, &hold, GV_FIXED_DT);
        gv_world_clear_events(w);
    }
    GV_CHECK_EQ_I(w->player.tether.state, GV_TETHER_ATTACHED);
    GV_CHECK(w->player.tether.attach_time > 0.45f);

    /* Read the velocity directly: player.speed is a per-step cache, so it is
     * only trustworthy at the point the step left it. */
    float before = gv_v2_len(w->player.vel);
    gv_input in = gv_idle_input(w);
    in.tether_held = false;
    gv_world_step(w, &in, GV_FIXED_DT);
    gv_world_clear_events(w);

    float gained = gv_v2_len(w->player.vel) - before;
    GV_CHECKF(gained > 0.0f, "a full-quality release granted no boost at all (%.1f)", gained);
    GV_CHECKF(gained <= GV_TETHER_RELEASE_MAX_BOOST + 5.0f,
              "release granted %.1f px/s, above the %.1f cap", gained,
              (double)GV_TETHER_RELEASE_MAX_BOOST);

    /* Now hammer fire-and-release for a long time and confirm the speed does
     * not run away. */
    float peak = 0.0f;
    for (int i = 0; i < 4000; ++i) {
        gv_input tap = gv_idle_input(w);
        tap.aim_point = py->pos;
        tap.tether_pressed = (i % 6) == 0;
        tap.tether_held = (i % 6) < 3;
        gv_world_step(w, &tap, GV_FIXED_DT);
        gv_world_clear_events(w);
        if (w->player.speed > peak) peak = w->player.speed;
    }
    GV_CHECKF(peak <= GV_PLAYER_MAX_SPEED + 1.0f, "tap-firing broke the speed cap (%.1f)", peak);
    free(w);
}

GV_TEST(tether_detaches_when_anchor_dies)
{
    gv_world *w = gv_new_world(2009u, GV_DIFF_PILOT);
    GV_CHECK(w != NULL);
    if (!w) return;
    gv_park_waves(w);
    /* Nothing else the hook could catch on the way to the target. */
    gv_clear_arena(w);

    /* Anchor onto an enemy, then destroy it and confirm the rope lets go
     * rather than dangling from a recycled pool slot. */
    gv_v2 target_pos = gv_v2_add(w->player.pos, gv_v2_make(260.0f, 0.0f));
    gv_enemy *e = gv_place_enemy(w, GV_ENEMY_SENTINEL, target_pos);
    GV_CHECK(e != NULL);
    if (!e) { free(w); return; }
    /* Freeze it so it stays put while the hook crosses. */
    e->stagger = 100.0f;

    gv_input in = gv_idle_input(w);
    in.aim_point = target_pos;
    in.tether_pressed = true;
    in.tether_held = true;
    for (int i = 0; i < 120; ++i) {
        gv_world_step(w, &in, GV_FIXED_DT);
        gv_world_clear_events(w);
        in.tether_pressed = false;
        if (w->player.tether.state == GV_TETHER_ATTACHED) break;
    }
    GV_CHECK_EQ_I(w->player.tether.state, GV_TETHER_ATTACHED);
    GV_CHECK_EQ_I(w->player.tether.anchor_kind, GV_ANCHOR_ENEMY);

    gv_kill_enemy(w, e, GV_KILL_SLAM, gv_v2_make(1.0f, 0.0f), 900.0f);
    GV_CHECK(!e->alive);
    GV_CHECKF(w->player.tether.anchor_kind == GV_ANCHOR_NONE,
              "tether still anchored to a destroyed enemy");
    GV_CHECK(w->player.tether.state != GV_TETHER_ATTACHED);

    /* Keep simulating: a stale anchor would show up as NaN or a wild snap. */
    gv_run(w, &in, 300);
    GV_CHECK(gv_world_all_finite(w));
    free(w);
}

GV_TEST(tether_stale_anchor_slot_is_not_reused)
{
    /* Generation counters exist so that a recycled pool slot cannot be
     * mistaken for the original anchor. Force exactly that collision. */
    gv_world *w = gv_new_world(2010u, GV_DIFF_PILOT);
    GV_CHECK(w != NULL);
    if (!w) return;
    gv_park_waves(w);
    /* Nothing else the hook could catch on the way to the target. */
    gv_clear_arena(w);

    gv_v2 target_pos = gv_v2_add(w->player.pos, gv_v2_make(240.0f, 0.0f));
    gv_enemy *e = gv_place_enemy(w, GV_ENEMY_DRONE, target_pos);
    GV_CHECK(e != NULL);
    if (!e) { free(w); return; }
    e->stagger = 100.0f;

    gv_input in = gv_idle_input(w);
    in.aim_point = target_pos;
    in.tether_pressed = true;
    in.tether_held = true;
    for (int i = 0; i < 120; ++i) {
        gv_world_step(w, &in, GV_FIXED_DT);
        gv_world_clear_events(w);
        in.tether_pressed = false;
        if (w->player.tether.state == GV_TETHER_ATTACHED) break;
    }
    GV_CHECK_EQ_I(w->player.tether.state, GV_TETHER_ATTACHED);

    int slot = w->player.tether.anchor_index;
    uint16_t old_gen = w->player.tether.anchor_gen;
    GV_CHECK(slot >= 0);

    /* Revive the same slot as a different enemy, exactly as the pool would. */
    w->enemies[slot].alive = false;
    w->enemy_count--;
    gv_enemy *reused = gv_place_enemy(w, GV_ENEMY_LANCER, target_pos);
    GV_CHECK(reused == &w->enemies[slot]);
    GV_CHECKF(reused->gen != old_gen, "pool slot reused without bumping the generation");

    gv_v2 pos, vel;
    float im;
    bool resolved = gv_anchor_resolve(w, &w->player.tether, &pos, &vel, &im);
    GV_CHECKF(!resolved, "stale anchor resolved to the enemy occupying its old slot");
    free(w);
}

GV_TEST(tether_anchor_resolve_rejects_bad_handles)
{
    gv_world *w = gv_new_world(2011u, GV_DIFF_PILOT);
    GV_CHECK(w != NULL);
    if (!w) return;

    gv_tether t;
    memset(&t, 0, sizeof(t));

    /* Out-of-range indices in every direction must be refused, not indexed. */
    const int kinds[] = { GV_ANCHOR_PYLON, GV_ANCHOR_ROCK, GV_ANCHOR_ENEMY };
    const int bad[] = { -1, -12345, GV_MAX_ENEMIES, GV_MAX_ROCKS, GV_MAX_PYLONS, 100000 };
    for (size_t k = 0; k < sizeof(kinds) / sizeof(kinds[0]); ++k) {
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
            t.anchor_kind = (uint8_t)kinds[k];
            t.anchor_index = (int16_t)bad[i];
            t.anchor_gen = 1u;
            GV_CHECK(!gv_anchor_resolve(w, &t, NULL, NULL, NULL));
            /* Applying an impulse through a bad handle must also be inert. */
            gv_anchor_apply_impulse(w, &t, gv_v2_make(100.0f, 100.0f));
        }
    }

    t.anchor_kind = GV_ANCHOR_NONE;
    t.anchor_index = 0;
    GV_CHECK(!gv_anchor_resolve(w, &t, NULL, NULL, NULL));

    /* An out-of-band kind value must not fall into a live branch. */
    t.anchor_kind = 200;
    GV_CHECK(!gv_anchor_resolve(w, &t, NULL, NULL, NULL));
    GV_CHECK(gv_world_all_finite(w));
    free(w);
}

GV_TEST(tether_cannot_grab_a_materialising_enemy)
{
    /* Spawning enemies are intangible; the hook must pass straight through,
     * otherwise the player can be yanked toward something that cannot yet be
     * hit or hurt them. */
    gv_world *w = gv_new_world(2012u, GV_DIFF_PILOT);
    GV_CHECK(w != NULL);
    if (!w) return;
    gv_park_waves(w);
    gv_clear_arena(w);

    gv_v2 target_pos = gv_v2_add(w->player.pos, gv_v2_make(200.0f, 0.0f));
    gv_enemy *e = gv_spawn_enemy(w, GV_ENEMY_DRONE, target_pos, 0);
    GV_CHECK(e != NULL);
    if (!e) { free(w); return; }
    e->spawn_anim = 0.0f;
    e->stagger = 100.0f;

    gv_input in = gv_idle_input(w);
    in.aim_point = target_pos;
    in.tether_pressed = true;
    in.tether_held = true;

    /* Only a handful of steps, so the enemy is still materialising. */
    for (int i = 0; i < 12; ++i) {
        gv_world_step(w, &in, GV_FIXED_DT);
        gv_world_clear_events(w);
        in.tether_pressed = false;
        GV_CHECKF(w->player.tether.anchor_kind != GV_ANCHOR_ENEMY,
                  "hook latched onto an enemy that had not finished spawning");
    }
    GV_CHECK(e->spawn_anim < 1.0f);
    free(w);
}

GV_TEST(tether_swinging_on_a_rock_moves_the_rock)
{
    /* Swinging on a rock must transfer momentum into it — that is the whole
     * basis of whip kills. */
    gv_world *w = gv_new_world(2013u, GV_DIFF_PILOT);
    GV_CHECK(w != NULL);
    if (!w) return;
    gv_park_waves(w);
    gv_clear_arena(w);

    gv_v2 rock_pos = gv_v2_add(w->player.pos, gv_v2_make(260.0f, 0.0f));
    gv_rock *r = gv_spawn_rock(w, rock_pos, 24.0f);
    GV_CHECK(r != NULL);
    if (!r) { free(w); return; }
    r->vel = gv_v2_zero();

    gv_input in = gv_idle_input(w);
    in.aim_point = rock_pos;
    in.tether_pressed = true;
    in.tether_held = true;
    for (int i = 0; i < 120; ++i) {
        gv_world_step(w, &in, GV_FIXED_DT);
        gv_world_clear_events(w);
        in.tether_pressed = false;
        if (w->player.tether.state == GV_TETHER_ATTACHED) break;
    }
    GV_CHECK_EQ_I(w->player.tether.anchor_kind, GV_ANCHOR_ROCK);

    /* Haul away from the rock at full thrust. */
    float peak_rock_speed = 0.0f;
    for (int i = 0; i < 400; ++i) {
        gv_v2 away = gv_v2_norm_or(gv_v2_sub(w->player.pos, r->pos), gv_v2_make(-1.0f, 0.0f));
        in.move = away;
        in.aim_point = gv_v2_add(w->player.pos, away);
        gv_world_step(w, &in, GV_FIXED_DT);
        gv_world_clear_events(w);
        float rs = gv_v2_len(r->vel);
        if (rs > peak_rock_speed) peak_rock_speed = rs;
        GV_CHECK(gv_v2_finite(r->vel));
    }
    GV_CHECKF(peak_rock_speed > 60.0f, "rope did not drag the rock (peak %.1f px/s)",
              peak_rock_speed);
    free(w);
}
