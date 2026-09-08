/* test_rng.c — the RNG has to be exactly reproducible and free of the classic
 * modulo bias, because the wave director and every soak test depend on it. */
#include "../src/core/gv_rng.h"
#include "../src/core/gv_math.h"
#include "gv_test.h"

GV_TEST(rng_is_deterministic)
{
    gv_rng a, b;
    gv_rng_seed(&a, 12345u, 1u);
    gv_rng_seed(&b, 12345u, 1u);
    for (int i = 0; i < 1000; ++i) {
        GV_CHECK_EQ_I(gv_rng_u32(&a), gv_rng_u32(&b));
    }
}

GV_TEST(rng_streams_are_independent)
{
    gv_rng a, b;
    gv_rng_seed(&a, 777u, 1u);
    gv_rng_seed(&b, 777u, 2u);

    /* Same seed, different stream: the sequences must diverge. This is what
     * keeps the cosmetic stream from perturbing gameplay. */
    int same = 0;
    for (int i = 0; i < 256; ++i) {
        if (gv_rng_u32(&a) == gv_rng_u32(&b)) same++;
    }
    GV_CHECKF(same < 8, "streams too correlated (%d/256 identical draws)", same);
}

GV_TEST(rng_different_seeds_diverge)
{
    gv_rng a, b;
    gv_rng_seed(&a, 1u, 1u);
    gv_rng_seed(&b, 2u, 1u);
    int same = 0;
    for (int i = 0; i < 256; ++i) {
        if (gv_rng_u32(&a) == gv_rng_u32(&b)) same++;
    }
    GV_CHECKF(same < 8, "seeds too correlated (%d/256 identical draws)", same);
}

GV_TEST(rng_seed_zero_is_valid)
{
    /* A zero seed must still produce a varied sequence, not a stuck state. */
    gv_rng r;
    gv_rng_seed(&r, 0u, 0u);
    uint32_t first = gv_rng_u32(&r);
    int distinct = 0;
    for (int i = 0; i < 64; ++i) {
        if (gv_rng_u32(&r) != first) distinct++;
    }
    GV_CHECK(distinct > 55);
}

GV_TEST(rng_below_bounds)
{
    gv_rng r;
    gv_rng_seed(&r, 99u, 1u);

    /* A zero bound is defined to return 0 rather than dividing by zero. */
    GV_CHECK_EQ_I(gv_rng_below(&r, 0u), 0u);
    GV_CHECK_EQ_I(gv_rng_below(&r, 1u), 0u);

    for (int i = 0; i < 5000; ++i) {
        uint32_t v = gv_rng_below(&r, 7u);
        GV_CHECKF(v < 7u, "gv_rng_below(7) returned %u", v);
    }
    /* Bound at the extreme of the range must not loop forever. */
    for (int i = 0; i < 64; ++i) {
        GV_CHECK(gv_rng_below(&r, 0xFFFFFFFFu) < 0xFFFFFFFFu);
    }
}

GV_TEST(rng_below_is_unbiased)
{
    /* Rejection sampling should leave the buckets flat. A naive `% bound`
     * over a non-power-of-two would skew the low buckets measurably here. */
    gv_rng r;
    gv_rng_seed(&r, 4242u, 1u);

    const int buckets = 7;
    const int draws = 700000;
    int counts[7] = { 0 };
    for (int i = 0; i < draws; ++i) counts[gv_rng_below(&r, (uint32_t)buckets)]++;

    double expected = (double)draws / (double)buckets;
    double chi2 = 0.0;
    for (int i = 0; i < buckets; ++i) {
        double d = (double)counts[i] - expected;
        chi2 += d * d / expected;
    }
    /* 6 degrees of freedom: the 0.999 critical value is ~22.5. */
    GV_CHECKF(chi2 < 22.5, "chi-square %.2f suggests a biased distribution", chi2);
}

GV_TEST(rng_range_i)
{
    gv_rng r;
    gv_rng_seed(&r, 31337u, 1u);

    for (int i = 0; i < 20000; ++i) {
        int v = gv_rng_range_i(&r, -5, 5);
        GV_CHECKF(v >= -5 && v <= 5, "range_i out of bounds: %d", v);
    }
    /* Inverted bounds are tolerated by swapping. */
    for (int i = 0; i < 1000; ++i) {
        int v = gv_rng_range_i(&r, 10, 3);
        GV_CHECKF(v >= 3 && v <= 10, "swapped range_i out of bounds: %d", v);
    }
    /* A single-value range always returns that value. */
    for (int i = 0; i < 32; ++i) GV_CHECK_EQ_I(gv_rng_range_i(&r, 4, 4), 4);

    /* The full integer range must not overflow the span computation (the span
     * is 2^32, which does not fit in the uint32 bound used for smaller ranges). */
    int seen_negative = 0, seen_positive = 0;
    for (int i = 0; i < 256; ++i) {
        int v = gv_rng_range_i(&r, INT32_MIN, INT32_MAX);
        if (v < 0) seen_negative++;
        if (v > 0) seen_positive++;
    }
    GV_CHECK(seen_negative > 80 && seen_positive > 80);
}

GV_TEST(rng_floats)
{
    gv_rng r;
    gv_rng_seed(&r, 5150u, 1u);

    float lo = 2.0f, hi = -1.0f;
    for (int i = 0; i < 100000; ++i) {
        float v = gv_rng_f01(&r);
        GV_CHECKF(v >= 0.0f && v < 1.0f, "f01 out of [0,1): %g", v);
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    /* The full unit interval should be well covered. */
    GV_CHECK(lo < 0.01f);
    GV_CHECK(hi > 0.99f);

    for (int i = 0; i < 10000; ++i) {
        float v = gv_rng_range_f(&r, -3.0f, 7.0f);
        GV_CHECK(v >= -3.0f && v <= 7.0f);
        float s = gv_rng_signed(&r);
        GV_CHECK(s >= -1.0f && s < 1.0f);
        float a = gv_rng_angle(&r);
        GV_CHECK(a >= 0.0f && a < GV_TAU);
    }

    /* Inverted float range swaps rather than producing an empty interval. */
    for (int i = 0; i < 1000; ++i) {
        float v = gv_rng_range_f(&r, 9.0f, 1.0f);
        GV_CHECK(v >= 1.0f && v <= 9.0f);
    }
    /* Degenerate range is exact. */
    GV_CHECK_NEAR(gv_rng_range_f(&r, 3.0f, 3.0f), 3.0f, 1e-6);
}

GV_TEST(rng_chance_edges)
{
    gv_rng r;
    gv_rng_seed(&r, 606u, 1u);

    for (int i = 0; i < 200; ++i) {
        GV_CHECK(!gv_rng_chance(&r, 0.0f));
        GV_CHECK(!gv_rng_chance(&r, -1.0f));
        GV_CHECK(gv_rng_chance(&r, 1.0f));
        GV_CHECK(gv_rng_chance(&r, 2.0f));
    }

    int hits = 0;
    const int n = 200000;
    for (int i = 0; i < n; ++i) {
        if (gv_rng_chance(&r, 0.25f)) hits++;
    }
    double rate = (double)hits / (double)n;
    GV_CHECKF(rate > 0.24 && rate < 0.26, "chance(0.25) fired at %.4f", rate);
}

GV_TEST(rng_gauss_is_bounded)
{
    gv_rng r;
    gv_rng_seed(&r, 8080u, 1u);

    double sum = 0.0, sum_sq = 0.0;
    const int n = 200000;
    for (int i = 0; i < n; ++i) {
        float v = gv_rng_gauss(&r);
        /* Bounded tails are intentional: particles must stay on screen. */
        GV_CHECKF(v > -3.6f && v < 3.6f, "gauss out of expected bounds: %g", v);
        sum += v;
        sum_sq += (double)v * (double)v;
    }
    double mean = sum / n;
    double var = sum_sq / n - mean * mean;
    GV_CHECKF(fabs(mean) < 0.02, "gauss mean %.4f", mean);
    GV_CHECKF(fabs(var - 1.0) < 0.05, "gauss variance %.4f", var);
}
