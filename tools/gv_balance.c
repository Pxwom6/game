/* gv_balance.c — balance probe.
 *
 * Runs the scripted bot across seeds and difficulties and reports how far it
 * gets. The bot is not a good player, so the absolute numbers mean little; the
 * value is in the comparison, before and after a tuning change.
 *
 * Usage: gv_balance [seconds_per_run] [runs_per_difficulty]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/game/gv_sim.h"
#include "../src/game/gv_world.h"
#include "../src/game/gv_autopilot.h"

typedef struct {
    float time;
    int wave;
    int kills, slam, whip;
    int64_t score;
    float peak_speed;
    int best_combo;
    bool died;
} gv_run_result;

static gv_run_result gv_play(uint64_t seed, int difficulty, float seconds, bool verbose)
{
    gv_run_result r;
    memset(&r, 0, sizeof(r));

    gv_world *w = (gv_world *)malloc(sizeof(gv_world));
    if (!w) return r;
    gv_world_init(w, seed, difficulty);

    gv_rng jitter;
    gv_rng_seed(&jitter, seed ^ 0xB07u, 5u);

    int steps = (int)(seconds / GV_FIXED_DT);
    int report = (int)(20.0f / GV_FIXED_DT);

    for (int i = 0; i < steps; ++i) {
        gv_input in = gv_autopilot_think(w, &jitter);
        gv_world_step(w, &in, GV_FIXED_DT);
        gv_world_clear_events(w);
        if (w->player.speed > r.peak_speed) r.peak_speed = w->player.speed;

        if (verbose && (i % report) == 0) {
            printf("  t=%5.0f wave=%2d alive=%2d kills=%4d score=%9lld lives=%d "
                   "spd=%4.0f mult=%2d hunt=%.2f\n",
                   (double)w->time, w->wave.index, w->enemy_count, w->score.kills,
                   (long long)w->score.score, w->player.lives, (double)w->player.speed,
                   w->score.multiplier, (double)w->wave.hunt);
        }
        if (gv_world_is_over(w)) {
            r.died = true;
            break;
        }
    }

    r.time = w->time;
    r.wave = w->wave.index;
    r.kills = w->score.kills;
    r.slam = w->score.slam_kills;
    r.whip = w->score.whip_kills;
    r.score = w->score.score;
    r.best_combo = w->score.best_combo;
    free(w);
    return r;
}

int main(int argc, char **argv)
{
    float seconds = (argc > 1) ? (float)atof(argv[1]) : 420.0f;
    int runs = (argc > 2) ? atoi(argv[2]) : 6;
    if (runs < 1) runs = 1;

    printf("=== annotated run (seed 99, PILOT) ===\n");
    gv_play(99u, GV_DIFF_PILOT, seconds, true);

    printf("\n=== sweep: %d runs x %.0fs per difficulty ===\n", runs, (double)seconds);
    printf("%-7s %-6s %7s %5s %6s %6s %6s %10s %6s %6s\n", "diff", "seed", "time", "wave",
           "kills", "slam", "whip", "score", "peak", "combo");

    for (int d = 0; d < GV_DIFF_COUNT; ++d) {
        double sum_wave = 0, sum_score = 0, sum_time = 0;
        int deaths = 0;

        for (int i = 1; i <= runs; ++i) {
            uint64_t seed = (uint64_t)i * 7919u;
            gv_run_result r = gv_play(seed, d, seconds, false);
            printf("%-7s %-6llu %7.1f %5d %6d %6d %6d %10lld %6.0f %6d %s\n",
                   gv_difficulty_name(d), (unsigned long long)seed, (double)r.time, r.wave,
                   r.kills, r.slam, r.whip, (long long)r.score, (double)r.peak_speed,
                   r.best_combo, r.died ? "" : "(survived)");
            sum_wave += r.wave;
            sum_score += (double)r.score;
            sum_time += r.time;
            if (r.died) deaths++;
        }
        printf("  %-5s mean: wave %.1f  score %.0f  time %.0fs  deaths %d/%d\n\n",
               gv_difficulty_name(d), sum_wave / runs, sum_score / runs, sum_time / runs, deaths,
               runs);
    }
    return 0;
}
