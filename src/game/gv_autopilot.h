/* gv_autopilot.h — a scripted player.
 *
 * It is not meant to be good; it is meant to be *competent and deterministic*.
 * It plays the intended loop — anchor, swing, build speed, release into a
 * target — which makes it useful in three places:
 *
 *   1. the title screen, where it plays a live demo behind the menu;
 *   2. the soak tests, reaching states a random-input fuzzer never will
 *      (long swings, overdrive slams, chained kills, deep waves); and
 *   3. the balance probe, where how far it gets is a stable reference point,
 *      so a tuning regression shows up as a change in its reach.
 */
#ifndef GV_AUTOPILOT_H
#define GV_AUTOPILOT_H

#include "../core/gv_rng.h"
#include "gv_world.h"

/* Produce one frame of input for the current world state. `jitter` seeds the
 * small amount of randomness that stops the bot from getting stuck in a loop;
 * pass the same generator across a run to keep it reproducible. */
gv_input gv_autopilot_think(gv_world *w, gv_rng *jitter);

#endif /* GV_AUTOPILOT_H */
