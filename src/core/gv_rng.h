/* gv_rng.h — deterministic pseudo-random number generation.
 *
 * PCG32 (XSH-RR variant). Chosen over rand() because the sequence must be
 * exactly reproducible from a seed across runs and machines: replays, the
 * headless soak tests and the wave director all depend on it.
 *
 * The game keeps two independent streams — one for simulation, one for purely
 * cosmetic effects — so that changing particle density in the settings cannot
 * perturb gameplay.
 */
#ifndef GV_RNG_H
#define GV_RNG_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint64_t state;
    uint64_t inc; /* stream selector; always odd */
} gv_rng;

/* Seed a generator. Any seed/stream pair is valid, including zero. */
void gv_rng_seed(gv_rng *r, uint64_t seed, uint64_t stream);

/* Uniform in [0, 2^32). */
uint32_t gv_rng_u32(gv_rng *r);

/* Uniform in [0, bound). Returns 0 when bound is 0. Debiased by rejection. */
uint32_t gv_rng_below(gv_rng *r, uint32_t bound);

/* Uniform integer in [lo, hi] inclusive. Tolerates hi < lo by swapping. */
int gv_rng_range_i(gv_rng *r, int lo, int hi);

/* Uniform float in [0, 1). Never returns exactly 1.0f. */
float gv_rng_f01(gv_rng *r);

/* Uniform float in [lo, hi). Tolerates hi < lo by swapping. */
float gv_rng_range_f(gv_rng *r, float lo, float hi);

/* Uniform float in [-1, 1). */
float gv_rng_signed(gv_rng *r);

/* True with probability p. p <= 0 is never, p >= 1 is always. */
bool gv_rng_chance(gv_rng *r, float p);

/* Uniform angle in [0, 2*pi). */
float gv_rng_angle(gv_rng *r);

/* Approximately normal, mean 0, standard deviation 1 (sum of uniforms). */
float gv_rng_gauss(gv_rng *r);

#endif /* GV_RNG_H */
