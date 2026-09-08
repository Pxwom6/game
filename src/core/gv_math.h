/* gv_math.h — scalar and 2D vector math for GRAVITON.
 *
 * Header-only, freestanding (libm only). Deliberately free of any SDL or
 * platform dependency so the simulation can be unit-tested headlessly.
 */
#ifndef GV_MATH_H
#define GV_MATH_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#define GV_PI 3.14159265358979323846f
#define GV_TAU 6.28318530717958647692f
#define GV_EPS 1e-6f

typedef struct {
    float x, y;
} gv_v2;

static inline gv_v2 gv_v2_make(float x, float y)
{
    gv_v2 v = { x, y };
    return v;
}

static inline gv_v2 gv_v2_zero(void) { return gv_v2_make(0.0f, 0.0f); }

static inline gv_v2 gv_v2_add(gv_v2 a, gv_v2 b) { return gv_v2_make(a.x + b.x, a.y + b.y); }
static inline gv_v2 gv_v2_sub(gv_v2 a, gv_v2 b) { return gv_v2_make(a.x - b.x, a.y - b.y); }
static inline gv_v2 gv_v2_mul(gv_v2 a, float s) { return gv_v2_make(a.x * s, a.y * s); }
static inline gv_v2 gv_v2_neg(gv_v2 a) { return gv_v2_make(-a.x, -a.y); }

/* Component-wise multiply-add: a + b*s. */
static inline gv_v2 gv_v2_madd(gv_v2 a, gv_v2 b, float s)
{
    return gv_v2_make(a.x + b.x * s, a.y + b.y * s);
}

static inline float gv_v2_dot(gv_v2 a, gv_v2 b) { return a.x * b.x + a.y * b.y; }

/* 2D analogue of the cross product: the z component of a x b. */
static inline float gv_v2_cross(gv_v2 a, gv_v2 b) { return a.x * b.y - a.y * b.x; }

static inline float gv_v2_len_sq(gv_v2 a) { return a.x * a.x + a.y * a.y; }
static inline float gv_v2_len(gv_v2 a) { return sqrtf(a.x * a.x + a.y * a.y); }

static inline float gv_v2_dist_sq(gv_v2 a, gv_v2 b)
{
    float dx = a.x - b.x, dy = a.y - b.y;
    return dx * dx + dy * dy;
}

static inline float gv_v2_dist(gv_v2 a, gv_v2 b) { return sqrtf(gv_v2_dist_sq(a, b)); }

/* Normalise, returning the zero vector for degenerate input.
 *
 * The comparison is deliberately written as a negated "is it usable?" test
 * rather than "is it too small?": NaN fails every ordered comparison, so this
 * form routes NaN and infinity to the safe branch instead of propagating them.
 * A single NaN reaching the renderer costs a frozen frame, so normalisation is
 * treated as a hard guarantee that the result is always finite. */
static inline gv_v2 gv_v2_norm(gv_v2 a)
{
    float l2 = gv_v2_len_sq(a);
    if (!(l2 > GV_EPS * GV_EPS) || !isfinite(l2)) return gv_v2_zero();
    return gv_v2_mul(a, 1.0f / sqrtf(l2));
}

/* Normalise, substituting `fallback` when the input is degenerate. */
static inline gv_v2 gv_v2_norm_or(gv_v2 a, gv_v2 fallback)
{
    float l2 = gv_v2_len_sq(a);
    if (!(l2 > GV_EPS * GV_EPS) || !isfinite(l2)) return fallback;
    return gv_v2_mul(a, 1.0f / sqrtf(l2));
}

/* Rotate 90 degrees counter-clockwise. */
static inline gv_v2 gv_v2_perp(gv_v2 a) { return gv_v2_make(-a.y, a.x); }

static inline gv_v2 gv_v2_from_angle(float radians)
{
    return gv_v2_make(cosf(radians), sinf(radians));
}

static inline gv_v2 gv_v2_polar(float radians, float radius)
{
    return gv_v2_make(cosf(radians) * radius, sinf(radians) * radius);
}

static inline float gv_v2_angle(gv_v2 a) { return atan2f(a.y, a.x); }

static inline gv_v2 gv_v2_rotate(gv_v2 a, float radians)
{
    float c = cosf(radians), s = sinf(radians);
    return gv_v2_make(a.x * c - a.y * s, a.x * s + a.y * c);
}

static inline gv_v2 gv_v2_lerp(gv_v2 a, gv_v2 b, float t)
{
    return gv_v2_make(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t);
}

/* Clamp a vector's magnitude to `max`. Zero-length input passes through, and
 * the result is always finite: NaN collapses to zero, and a vector large
 * enough to overflow the squared length is rescaled before clamping rather
 * than being lost. */
static inline gv_v2 gv_v2_clamp_len(gv_v2 a, float max)
{
    if (!(max > 0.0f)) return gv_v2_zero();

    float l2 = gv_v2_len_sq(a);
    if (isfinite(l2)) {
        if (l2 <= max * max) return a;
        return gv_v2_mul(a, max / sqrtf(l2));
    }

    /* len_sq overflowed to infinity, or a component was NaN. Divide through by
     * the largest component so a merely enormous vector still yields a usable
     * direction; genuinely broken input falls through to zero. */
    float ax = fabsf(a.x), ay = fabsf(a.y);
    float m = (ax > ay) ? ax : ay; /* NaN-tolerant: NaN loses the comparison */
    if (!(m > 0.0f) || !isfinite(m)) return gv_v2_zero();
    gv_v2 s = gv_v2_make(a.x / m, a.y / m);
    float sl = sqrtf(gv_v2_len_sq(s));
    if (!(sl > 0.0f) || !isfinite(sl)) return gv_v2_zero();
    return gv_v2_mul(s, max / sl);
}

static inline bool gv_v2_finite(gv_v2 a) { return isfinite(a.x) && isfinite(a.y); }

/* ---- scalar helpers ---- */

static inline float gv_minf(float a, float b) { return a < b ? a : b; }
static inline float gv_maxf(float a, float b) { return a > b ? a : b; }
static inline int gv_mini(int a, int b) { return a < b ? a : b; }
static inline int gv_maxi(int a, int b) { return a > b ? a : b; }

static inline float gv_clampf(float v, float lo, float hi)
{
    /* NaN-safe: a NaN input falls through both comparisons and yields `lo`. */
    if (v > hi) return hi;
    if (v > lo) return v;
    return lo;
}

static inline int gv_clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline float gv_lerpf(float a, float b, float t) { return a + (b - a) * t; }

static inline float gv_absf(float v) { return v < 0.0f ? -v : v; }

static inline float gv_signf(float v) { return v < 0.0f ? -1.0f : (v > 0.0f ? 1.0f : 0.0f); }

/* Map v from [a0,a1] into [b0,b1], clamped. Degenerate input ranges yield b0. */
static inline float gv_remap(float v, float a0, float a1, float b0, float b1)
{
    float d = a1 - a0;
    if (gv_absf(d) < GV_EPS) return b0;
    return b0 + (gv_clampf(v, gv_minf(a0, a1), gv_maxf(a0, a1)) - a0) * (b1 - b0) / d;
}

/* Smoothstep, clamped to [0,1]. */
static inline float gv_smoothstep(float edge0, float edge1, float v)
{
    float d = edge1 - edge0;
    if (gv_absf(d) < GV_EPS) return v < edge0 ? 0.0f : 1.0f;
    float t = gv_clampf((v - edge0) / d, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

/* Frame-rate independent exponential approach. `rate` is the reciprocal of the
 * time constant: larger converges faster. */
static inline float gv_approach_exp(float current, float target, float rate, float dt)
{
    if (rate <= 0.0f) return current;
    float t = 1.0f - expf(-rate * dt);
    return current + (target - current) * t;
}

static inline gv_v2 gv_v2_approach_exp(gv_v2 current, gv_v2 target, float rate, float dt)
{
    if (rate <= 0.0f) return current;
    float t = 1.0f - expf(-rate * dt);
    return gv_v2_lerp(current, target, t);
}

/* Move `current` toward `target` by at most `max_delta`. */
static inline float gv_approach_linear(float current, float target, float max_delta)
{
    float d = target - current;
    if (gv_absf(d) <= max_delta) return target;
    return current + gv_signf(d) * max_delta;
}

/* Wrap an angle into (-pi, pi]. */
static inline float gv_wrap_angle(float a)
{
    /* remainderf gives the IEEE remainder, which already lands in [-pi, pi]. */
    if (!isfinite(a)) return 0.0f;
    return remainderf(a, GV_TAU);
}

/* Shortest signed angular difference from `from` to `to`. */
static inline float gv_angle_delta(float from, float to)
{
    return gv_wrap_angle(to - from);
}

/* Rotate `from` toward `to` by at most `max_delta` radians, taking the short way. */
static inline float gv_angle_approach(float from, float to, float max_delta)
{
    float d = gv_angle_delta(from, to);
    if (gv_absf(d) <= max_delta) return gv_wrap_angle(to);
    return gv_wrap_angle(from + gv_signf(d) * max_delta);
}

/* ---- geometry queries ---- */

static inline bool gv_circles_overlap(gv_v2 a, float ra, gv_v2 b, float rb)
{
    float r = ra + rb;
    return gv_v2_dist_sq(a, b) <= r * r;
}

/* Closest point to `p` on the segment ab. */
static inline gv_v2 gv_closest_point_on_segment(gv_v2 a, gv_v2 b, gv_v2 p)
{
    gv_v2 ab = gv_v2_sub(b, a);
    float len_sq = gv_v2_len_sq(ab);
    if (len_sq < GV_EPS) return a;
    float t = gv_clampf(gv_v2_dot(gv_v2_sub(p, a), ab) / len_sq, 0.0f, 1.0f);
    return gv_v2_madd(a, ab, t);
}

static inline float gv_point_segment_dist_sq(gv_v2 a, gv_v2 b, gv_v2 p)
{
    return gv_v2_dist_sq(p, gv_closest_point_on_segment(a, b, p));
}

/* Ray/circle intersection. Returns true and writes the nearest non-negative hit
 * distance along `dir` (which must be unit length) to `out_t`. */
static inline bool gv_ray_circle(gv_v2 origin, gv_v2 dir, gv_v2 center, float radius,
                                 float max_dist, float *out_t)
{
    gv_v2 m = gv_v2_sub(origin, center);
    float b = gv_v2_dot(m, dir);
    float c = gv_v2_len_sq(m) - radius * radius;
    /* Origin outside the circle and pointing away: no hit. */
    if (c > 0.0f && b > 0.0f) return false;
    float disc = b * b - c;
    if (disc < 0.0f) return false;
    float t = -b - sqrtf(disc);
    if (t < 0.0f) t = 0.0f; /* origin already inside */
    if (t > max_dist) return false;
    if (out_t) *out_t = t;
    return true;
}

/* Swept circle-vs-circle: does a circle of radius `ra` moving from `a` along
 * `delta` touch a static circle (`b`, `rb`)? Writes the time of impact in
 * [0,1] to `out_t`. Handles the already-overlapping case as t = 0. */
static inline bool gv_sweep_circle_circle(gv_v2 a, float ra, gv_v2 delta, gv_v2 b, float rb,
                                          float *out_t)
{
    float r = ra + rb;
    gv_v2 m = gv_v2_sub(a, b);
    float c = gv_v2_len_sq(m) - r * r;
    if (c <= 0.0f) { /* already overlapping */
        if (out_t) *out_t = 0.0f;
        return true;
    }
    float aa = gv_v2_len_sq(delta);
    if (aa < GV_EPS) return false; /* not moving and not overlapping */
    float bb = gv_v2_dot(m, delta);
    if (bb >= 0.0f) return false; /* moving away */
    float disc = bb * bb - aa * c;
    if (disc < 0.0f) return false;
    float t = (-bb - sqrtf(disc)) / aa;
    if (t < 0.0f || t > 1.0f) return false;
    if (out_t) *out_t = t;
    return true;
}

#endif /* GV_MATH_H */
