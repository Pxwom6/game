#include "gv_rng.h"

#include "gv_math.h"

#define GV_PCG_MULT 6364136223846793005ULL

void gv_rng_seed(gv_rng *r, uint64_t seed, uint64_t stream)
{
    /* The low bit of `inc` must be set for the LCG to reach full period. */
    r->state = 0u;
    r->inc = (stream << 1u) | 1u;
    (void)gv_rng_u32(r);
    r->state += seed;
    (void)gv_rng_u32(r);
}

uint32_t gv_rng_u32(gv_rng *r)
{
    uint64_t old = r->state;
    r->state = old * GV_PCG_MULT + r->inc;
    uint32_t xorshifted = (uint32_t)(((old >> 18u) ^ old) >> 27u);
    uint32_t rot = (uint32_t)(old >> 59u);
    /* Rotate right by `rot`; the mask keeps the shift in range when rot == 0. */
    return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
}

uint32_t gv_rng_below(gv_rng *r, uint32_t bound)
{
    if (bound == 0u) return 0u;
    /* Rejection sampling removes the modulo bias that a bare `% bound` has. */
    uint32_t threshold = (uint32_t)(-(int32_t)bound) % bound;
    for (;;) {
        uint32_t v = gv_rng_u32(r);
        if (v >= threshold) return v % bound;
    }
}

int gv_rng_range_i(gv_rng *r, int lo, int hi)
{
    if (hi < lo) {
        int t = lo;
        lo = hi;
        hi = t;
    }
    /* Compute the span in 64-bit so that INT_MIN..INT_MAX cannot overflow. */
    uint64_t span = (uint64_t)((int64_t)hi - (int64_t)lo) + 1ull;
    if (span >= 0x100000000ull) {
        /* Full 32-bit range: no rejection needed. */
        return (int)((int64_t)lo + (int64_t)gv_rng_u32(r));
    }
    return (int)((int64_t)lo + (int64_t)gv_rng_below(r, (uint32_t)span));
}

float gv_rng_f01(gv_rng *r)
{
    /* 24 bits of mantissa: the largest representable result is 1 - 2^-24. */
    return (float)(gv_rng_u32(r) >> 8u) * (1.0f / 16777216.0f);
}

float gv_rng_range_f(gv_rng *r, float lo, float hi)
{
    if (hi < lo) {
        float t = lo;
        lo = hi;
        hi = t;
    }
    return lo + (hi - lo) * gv_rng_f01(r);
}

float gv_rng_signed(gv_rng *r)
{
    return gv_rng_f01(r) * 2.0f - 1.0f;
}

bool gv_rng_chance(gv_rng *r, float p)
{
    if (p <= 0.0f) return false;
    if (p >= 1.0f) return true;
    return gv_rng_f01(r) < p;
}

float gv_rng_angle(gv_rng *r)
{
    return gv_rng_f01(r) * GV_TAU;
}

float gv_rng_gauss(gv_rng *r)
{
    /* Irwin-Hall with n = 4, rescaled to unit variance. Bounded output (no
     * infinite tails) is a feature here: it keeps particle bursts on screen. */
    float s = gv_rng_f01(r) + gv_rng_f01(r) + gv_rng_f01(r) + gv_rng_f01(r);
    return (s - 2.0f) * 1.7320508f;
}
