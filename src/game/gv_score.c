/* gv_score.c — scoring and the chain multiplier.
 *
 * The multiplier is the game's tension dial: it only climbs while you keep
 * killing, and a single hit resets it. That is what pushes a competent player
 * to keep their speed up instead of playing safe.
 */
#include "gv_sim.h"
#include "gv_world.h"

void gv_score_register_kill(gv_world *w, const gv_enemy *e, int cause)
{
    gv_score *s = &w->score;

    s->kills++;
    s->combo++;
    if (s->combo > s->best_combo) s->best_combo = s->combo;

    if (cause == GV_KILL_SLAM) s->slam_kills++;
    if (cause == GV_KILL_WHIP) s->whip_kills++;

    /* Climb the multiplier, but only while the chain window is still open. */
    if (s->multiplier < GV_MULT_MAX) {
        s->multiplier++;
        gv_emit(w, GV_EV_MULT_UP, e->pos, gv_clampf((float)s->multiplier / GV_MULT_MAX, 0.1f, 1.0f),
                s->multiplier);
    }
    s->mult_timer = GV_MULT_WINDOW;

    float base = e->score * w->tuning.score_mult;
    /* Whipping a rock through something is the hardest way to kill, so it pays
     * double. Raw speed pays a smaller premium on top. */
    float cause_mul = (cause == GV_KILL_WHIP) ? 2.0f : 1.0f;
    float speed_mul = 1.0f + 0.6f * w->player.overdrive;

    int64_t value = (int64_t)(base * cause_mul * speed_mul) * (int64_t)s->multiplier;
    if (value < 1) value = 1;
    s->score += value;

    int64_t bonus = value - (int64_t)base;
    if (bonus > 0) s->style_points += bonus;

    uint8_t cr = 255, cg = 255, cb = 255;
    if (cause == GV_KILL_WHIP) {
        cr = 180; cg = 255; cb = 120;
    } else if (w->player.overdrive > 0.65f) {
        cr = 255; cg = 190; cb = 90;
    }
    /* Clamp the displayed value: the floater only ever needs to be readable. */
    gv_fx_floater(w, e->pos, (int)gv_mini((int)value, 999999), cr, cg, cb);
}

void gv_score_update(gv_world *w, float dt)
{
    gv_score *s = &w->score;
    if (s->mult_timer > 0.0f) {
        s->mult_timer -= dt;
        if (s->mult_timer <= 0.0f) {
            s->mult_timer = 0.0f;
            s->multiplier = 1;
            s->combo = 0;
        }
    }
}
