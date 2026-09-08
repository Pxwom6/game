/* test_math.c — vector/scalar maths, with emphasis on the degenerate inputs
 * that a game reliably produces at some point (zero-length vectors, NaN from a
 * bad division, rays that start inside a circle, zero-length sweeps). */
#include "../src/core/gv_math.h"
#include "gv_test.h"

GV_TEST(math_vector_basics)
{
    gv_v2 a = gv_v2_make(3.0f, 4.0f);
    GV_CHECK_NEAR(gv_v2_len(a), 5.0f, 1e-5);
    GV_CHECK_NEAR(gv_v2_len_sq(a), 25.0f, 1e-4);

    gv_v2 b = gv_v2_make(-1.0f, 2.0f);
    GV_CHECK_NEAR(gv_v2_dot(a, b), 5.0f, 1e-5);
    GV_CHECK_NEAR(gv_v2_cross(a, b), 10.0f, 1e-5);

    gv_v2 sum = gv_v2_add(a, b);
    GV_CHECK_NEAR(sum.x, 2.0f, 1e-6);
    GV_CHECK_NEAR(sum.y, 6.0f, 1e-6);

    gv_v2 madd = gv_v2_madd(a, b, 2.0f);
    GV_CHECK_NEAR(madd.x, 1.0f, 1e-6);
    GV_CHECK_NEAR(madd.y, 8.0f, 1e-6);

    GV_CHECK_NEAR(gv_v2_dist(a, b), sqrtf(16.0f + 4.0f), 1e-5);
}

GV_TEST(math_normalise_degenerate)
{
    /* The single most common source of NaN in a game: normalising nothing. */
    gv_v2 z = gv_v2_norm(gv_v2_zero());
    GV_CHECK_NEAR(z.x, 0.0f, 1e-9);
    GV_CHECK_NEAR(z.y, 0.0f, 1e-9);

    gv_v2 tiny = gv_v2_norm(gv_v2_make(1e-12f, -1e-12f));
    GV_CHECK_NEAR(gv_v2_len(tiny), 0.0f, 1e-9);

    gv_v2 fb = gv_v2_norm_or(gv_v2_zero(), gv_v2_make(0.0f, 1.0f));
    GV_CHECK_NEAR(fb.y, 1.0f, 1e-6);

    gv_v2 big = gv_v2_norm(gv_v2_make(1e18f, 0.0f));
    GV_CHECK_NEAR(gv_v2_len(big), 1.0f, 1e-4);
    GV_CHECK(gv_v2_finite(big));
}

GV_TEST(math_clamp_len)
{
    gv_v2 v = gv_v2_make(30.0f, 40.0f); /* length 50 */
    gv_v2 c = gv_v2_clamp_len(v, 10.0f);
    GV_CHECK_NEAR(gv_v2_len(c), 10.0f, 1e-4);

    gv_v2 under = gv_v2_clamp_len(v, 100.0f);
    GV_CHECK_NEAR(gv_v2_len(under), 50.0f, 1e-4);

    /* A zero or negative cap must collapse the vector, not divide by it. */
    gv_v2 zero_cap = gv_v2_clamp_len(v, 0.0f);
    GV_CHECK_NEAR(gv_v2_len(zero_cap), 0.0f, 1e-6);
    gv_v2 neg_cap = gv_v2_clamp_len(v, -5.0f);
    GV_CHECK_NEAR(gv_v2_len(neg_cap), 0.0f, 1e-6);

    gv_v2 clamped_zero = gv_v2_clamp_len(gv_v2_zero(), 10.0f);
    GV_CHECK(gv_v2_finite(clamped_zero));
}

GV_TEST(math_normalise_never_yields_nan)
{
    /* Normalisation is relied on as a hard guarantee throughout the
     * simulation: whatever goes in, something finite comes out. A NaN aim
     * vector once propagated through the tether into every entity in the
     * world, so these cases are pinned down explicitly. */
    const gv_v2 poison[] = {
        { NAN, 0.0f },        { 0.0f, NAN },        { NAN, NAN },
        { INFINITY, 0.0f },   { 0.0f, -INFINITY },  { INFINITY, INFINITY },
        { INFINITY, -INFINITY }, { 1e30f, 1e30f },  { -1e38f, 1e38f },
    };
    const gv_v2 fallback = gv_v2_make(0.0f, 1.0f);

    for (size_t i = 0; i < sizeof(poison) / sizeof(poison[0]); ++i) {
        gv_v2 n = gv_v2_norm(poison[i]);
        GV_CHECKF(gv_v2_finite(n), "gv_v2_norm case %zu produced (%g, %g)", i, n.x, n.y);

        gv_v2 nf = gv_v2_norm_or(poison[i], fallback);
        GV_CHECKF(gv_v2_finite(nf), "gv_v2_norm_or case %zu produced (%g, %g)", i, nf.x, nf.y);

        gv_v2 c = gv_v2_clamp_len(poison[i], 100.0f);
        GV_CHECKF(gv_v2_finite(c), "gv_v2_clamp_len case %zu produced (%g, %g)", i, c.x, c.y);
        GV_CHECKF(gv_v2_len(c) <= 100.0f + 1e-3f, "clamp_len case %zu exceeded the cap", i);
    }

    /* A merely enormous (but finite) vector keeps its direction rather than
     * collapsing: len_sq overflows, so this exercises the rescaling path. */
    gv_v2 huge = gv_v2_clamp_len(gv_v2_make(3e30f, 0.0f), 10.0f);
    GV_CHECK_NEAR(huge.x, 10.0f, 1e-2);
    GV_CHECK_NEAR(huge.y, 0.0f, 1e-3);
}

GV_TEST(math_clamp_scalar_nan_safe)
{
    GV_CHECK_NEAR(gv_clampf(5.0f, 0.0f, 10.0f), 5.0f, 1e-6);
    GV_CHECK_NEAR(gv_clampf(-5.0f, 0.0f, 10.0f), 0.0f, 1e-6);
    GV_CHECK_NEAR(gv_clampf(50.0f, 0.0f, 10.0f), 10.0f, 1e-6);

    /* NaN must resolve to a bound rather than propagating. */
    float nan_v = gv_clampf(NAN, 2.0f, 8.0f);
    GV_CHECK_FINITE(nan_v);
    GV_CHECK(nan_v >= 2.0f && nan_v <= 8.0f);

    GV_CHECK_EQ_I(gv_clampi(-3, 0, 5), 0);
    GV_CHECK_EQ_I(gv_clampi(9, 0, 5), 5);
    GV_CHECK_EQ_I(gv_clampi(3, 0, 5), 3);
}

GV_TEST(math_remap_and_smoothstep)
{
    GV_CHECK_NEAR(gv_remap(5.0f, 0.0f, 10.0f, 0.0f, 100.0f), 50.0f, 1e-4);
    /* Out-of-range inputs clamp rather than extrapolate. */
    GV_CHECK_NEAR(gv_remap(-5.0f, 0.0f, 10.0f, 0.0f, 100.0f), 0.0f, 1e-4);
    GV_CHECK_NEAR(gv_remap(15.0f, 0.0f, 10.0f, 0.0f, 100.0f), 100.0f, 1e-4);
    /* A degenerate source range must not divide by zero. */
    GV_CHECK_NEAR(gv_remap(5.0f, 3.0f, 3.0f, 7.0f, 9.0f), 7.0f, 1e-4);

    GV_CHECK_NEAR(gv_smoothstep(0.0f, 1.0f, 0.0f), 0.0f, 1e-6);
    GV_CHECK_NEAR(gv_smoothstep(0.0f, 1.0f, 1.0f), 1.0f, 1e-6);
    GV_CHECK_NEAR(gv_smoothstep(0.0f, 1.0f, 0.5f), 0.5f, 1e-6);
    GV_CHECK_FINITE(gv_smoothstep(2.0f, 2.0f, 1.0f));
}

GV_TEST(math_angles)
{
    GV_CHECK_NEAR(gv_wrap_angle(0.0f), 0.0f, 1e-6);
    GV_CHECK_NEAR(gv_wrap_angle(GV_TAU), 0.0f, 1e-5);
    GV_CHECK_NEAR(gv_wrap_angle(GV_TAU * 5.0f + 1.0f), 1.0f, 1e-4);
    GV_CHECK(gv_absf(gv_wrap_angle(100.0f)) <= GV_PI + 1e-5f);
    /* Non-finite input must not escape into the rest of the frame. */
    GV_CHECK_FINITE(gv_wrap_angle(NAN));
    GV_CHECK_FINITE(gv_wrap_angle(INFINITY));

    /* Shortest path across the wrap point, not the long way round. */
    float d = gv_angle_delta(3.0f, -3.0f);
    GV_CHECK(gv_absf(d) < 0.6f);

    float stepped = gv_angle_approach(0.0f, GV_PI * 0.5f, 0.1f);
    GV_CHECK_NEAR(stepped, 0.1f, 1e-5);
    /* Overshoot snaps to the target instead of oscillating. */
    GV_CHECK_NEAR(gv_angle_approach(0.0f, 0.05f, 0.1f), 0.05f, 1e-5);

    gv_v2 dir = gv_v2_from_angle(GV_PI * 0.5f);
    GV_CHECK_NEAR(dir.x, 0.0f, 1e-5);
    GV_CHECK_NEAR(dir.y, 1.0f, 1e-5);
    GV_CHECK_NEAR(gv_v2_angle(gv_v2_make(0.0f, 1.0f)), GV_PI * 0.5f, 1e-5);
}

GV_TEST(math_rotate_and_perp)
{
    gv_v2 v = gv_v2_make(1.0f, 0.0f);
    gv_v2 r = gv_v2_rotate(v, GV_PI * 0.5f);
    GV_CHECK_NEAR(r.x, 0.0f, 1e-5);
    GV_CHECK_NEAR(r.y, 1.0f, 1e-5);

    gv_v2 p = gv_v2_perp(v);
    GV_CHECK_NEAR(gv_v2_dot(p, v), 0.0f, 1e-6);
    /* Rotation preserves length. */
    GV_CHECK_NEAR(gv_v2_len(gv_v2_rotate(gv_v2_make(3.0f, 4.0f), 1.234f)), 5.0f, 1e-4);
}

GV_TEST(math_approach)
{
    /* Exponential approach never overshoots and always converges. */
    float v = 0.0f;
    for (int i = 0; i < 500; ++i) v = gv_approach_exp(v, 10.0f, 5.0f, 1.0f / 60.0f);
    GV_CHECK_NEAR(v, 10.0f, 1e-3);

    float never = gv_approach_exp(3.0f, 10.0f, 0.0f, 0.1f);
    GV_CHECK_NEAR(never, 3.0f, 1e-6);

    GV_CHECK_NEAR(gv_approach_linear(0.0f, 1.0f, 0.25f), 0.25f, 1e-6);
    GV_CHECK_NEAR(gv_approach_linear(0.0f, 0.1f, 0.25f), 0.1f, 1e-6);
    GV_CHECK_NEAR(gv_approach_linear(0.0f, -1.0f, 0.25f), -0.25f, 1e-6);
}

GV_TEST(math_closest_point_on_segment)
{
    gv_v2 a = gv_v2_make(0.0f, 0.0f), b = gv_v2_make(10.0f, 0.0f);

    gv_v2 mid = gv_closest_point_on_segment(a, b, gv_v2_make(5.0f, 5.0f));
    GV_CHECK_NEAR(mid.x, 5.0f, 1e-5);
    GV_CHECK_NEAR(mid.y, 0.0f, 1e-5);

    /* Points beyond the ends clamp to the endpoints. */
    gv_v2 before = gv_closest_point_on_segment(a, b, gv_v2_make(-20.0f, 3.0f));
    GV_CHECK_NEAR(before.x, 0.0f, 1e-5);
    gv_v2 after = gv_closest_point_on_segment(a, b, gv_v2_make(99.0f, 3.0f));
    GV_CHECK_NEAR(after.x, 10.0f, 1e-5);

    /* Degenerate segment: both endpoints identical. */
    gv_v2 degen = gv_closest_point_on_segment(a, a, gv_v2_make(4.0f, 4.0f));
    GV_CHECK_NEAR(degen.x, 0.0f, 1e-6);
    GV_CHECK_NEAR(degen.y, 0.0f, 1e-6);

    GV_CHECK_NEAR(gv_point_segment_dist_sq(a, b, gv_v2_make(5.0f, 2.0f)), 4.0f, 1e-4);
}

GV_TEST(math_ray_circle)
{
    float t = -1.0f;
    /* Straight-on hit at distance 9 (circle at x=10, radius 1). */
    GV_CHECK(gv_ray_circle(gv_v2_zero(), gv_v2_make(1.0f, 0.0f), gv_v2_make(10.0f, 0.0f), 1.0f,
                           100.0f, &t));
    GV_CHECK_NEAR(t, 9.0f, 1e-4);

    /* Miss: passes above the circle. */
    GV_CHECK(!gv_ray_circle(gv_v2_zero(), gv_v2_make(1.0f, 0.0f), gv_v2_make(10.0f, 5.0f), 1.0f,
                            100.0f, &t));

    /* Hit exists but lies beyond max_dist. */
    GV_CHECK(!gv_ray_circle(gv_v2_zero(), gv_v2_make(1.0f, 0.0f), gv_v2_make(10.0f, 0.0f), 1.0f,
                            5.0f, &t));

    /* Pointing away from a circle behind us. */
    GV_CHECK(!gv_ray_circle(gv_v2_zero(), gv_v2_make(-1.0f, 0.0f), gv_v2_make(10.0f, 0.0f), 1.0f,
                            100.0f, &t));

    /* Origin inside the circle registers an immediate hit. */
    t = -1.0f;
    GV_CHECK(gv_ray_circle(gv_v2_make(10.0f, 0.0f), gv_v2_make(1.0f, 0.0f), gv_v2_make(10.0f, 0.0f),
                           3.0f, 100.0f, &t));
    GV_CHECK_NEAR(t, 0.0f, 1e-6);

    /* A NULL out-parameter is allowed. */
    GV_CHECK(gv_ray_circle(gv_v2_zero(), gv_v2_make(1.0f, 0.0f), gv_v2_make(10.0f, 0.0f), 1.0f,
                           100.0f, NULL));
}

GV_TEST(math_sweep_circle)
{
    float t = -1.0f;

    /* Moving right into a circle at x=10: contact when the gap closes. */
    GV_CHECK(gv_sweep_circle_circle(gv_v2_zero(), 1.0f, gv_v2_make(20.0f, 0.0f),
                                    gv_v2_make(10.0f, 0.0f), 1.0f, &t));
    GV_CHECK_NEAR(t, 8.0f / 20.0f, 1e-4);

    /* Already overlapping resolves at t = 0. */
    t = -1.0f;
    GV_CHECK(gv_sweep_circle_circle(gv_v2_zero(), 5.0f, gv_v2_make(1.0f, 0.0f),
                                    gv_v2_make(2.0f, 0.0f), 5.0f, &t));
    GV_CHECK_NEAR(t, 0.0f, 1e-6);

    /* Not moving and not touching: no hit, and no division by zero. */
    GV_CHECK(!gv_sweep_circle_circle(gv_v2_zero(), 1.0f, gv_v2_zero(), gv_v2_make(10.0f, 0.0f),
                                     1.0f, &t));

    /* Moving away. */
    GV_CHECK(!gv_sweep_circle_circle(gv_v2_zero(), 1.0f, gv_v2_make(-20.0f, 0.0f),
                                     gv_v2_make(10.0f, 0.0f), 1.0f, &t));

    /* Motion stops just short of contact. */
    GV_CHECK(!gv_sweep_circle_circle(gv_v2_zero(), 1.0f, gv_v2_make(5.0f, 0.0f),
                                     gv_v2_make(10.0f, 0.0f), 1.0f, &t));

    /* This is the tunnelling case the sweep exists for: a step long enough to
     * jump clean over the target would be missed by a discrete overlap test. */
    t = -1.0f;
    GV_CHECK(gv_sweep_circle_circle(gv_v2_zero(), 2.0f, gv_v2_make(400.0f, 0.0f),
                                    gv_v2_make(200.0f, 0.0f), 2.0f, &t));
    GV_CHECK(t > 0.0f && t < 1.0f);
    GV_CHECK(!gv_circles_overlap(gv_v2_zero(), 2.0f, gv_v2_make(200.0f, 0.0f), 2.0f));
}

GV_TEST(math_circles_overlap)
{
    GV_CHECK(gv_circles_overlap(gv_v2_zero(), 5.0f, gv_v2_make(9.0f, 0.0f), 5.0f));
    GV_CHECK(!gv_circles_overlap(gv_v2_zero(), 5.0f, gv_v2_make(11.0f, 0.0f), 5.0f));
    /* Exactly touching counts as overlapping. */
    GV_CHECK(gv_circles_overlap(gv_v2_zero(), 5.0f, gv_v2_make(10.0f, 0.0f), 5.0f));
}
