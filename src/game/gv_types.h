/* gv_types.h — entity definitions, tuning constants and the world state for
 * GRAVITON's simulation.
 *
 * Everything reachable from `gv_world` is plain data with no pointers into the
 * heap and no platform dependency. The whole simulation is therefore
 * memcpy-able, trivially snapshot-able for determinism tests, and free of any
 * allocation in the frame loop.
 */
#ifndef GV_TYPES_H
#define GV_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../core/gv_math.h"
#include "../core/gv_rng.h"

/* ---------------------------------------------------------------- capacities
 * Fixed pools. Spawners fail gracefully (dropping the request) when full, so
 * exceeding a pool degrades visuals or spawn rate but never corrupts state. */
#define GV_MAX_ENEMIES 192
#define GV_MAX_ROCKS 48
#define GV_MAX_PYLONS 40
#define GV_MAX_BULLETS 256
#define GV_MAX_PICKUPS 32
#define GV_MAX_PARTICLES 3072
#define GV_MAX_SHOCKWAVES 24
#define GV_MAX_EVENTS 256
#define GV_MAX_FLOATERS 32
#define GV_ROCK_VERTS 11

/* ------------------------------------------------------------------- world */
/* Arena size is a readability decision, not a scenery one. At the default
 * camera zoom the view covers roughly three quarters of the arena in each
 * axis: wide enough that the tether has room and the edges can still hold a
 * surprise, tight enough that the player can nearly always see what is about
 * to reach them. An earlier, much larger arena left most enemies off-screen
 * and the fight was carried by edge markers instead of the actual game. */
#define GV_ARENA_W 1920.0f
#define GV_ARENA_H 1080.0f
#define GV_VIEW_W 1280
#define GV_VIEW_H 720

/* Fixed simulation timestep. 120 Hz keeps the rope constraint stable at the
 * speeds the player can reach and decouples physics from display refresh. */
#define GV_FIXED_DT (1.0f / 120.0f)
/* Upper bound on catch-up steps per frame, so a stalled process (debugger
 * breakpoint, window drag) cannot spiral into an unbounded simulation burst. */
#define GV_MAX_STEPS_PER_FRAME 8

/* ------------------------------------------------------------------ player */
#define GV_PLAYER_RADIUS 13.0f
#define GV_PLAYER_THRUST 2450.0f
#define GV_PLAYER_DRAG 0.62f       /* exponential damping rate, 1/s */
#define GV_PLAYER_MAX_SPEED 1900.0f
#define GV_PLAYER_LIVES 3
#define GV_PLAYER_MAX_LIVES 6
#define GV_PLAYER_INVULN 1.6f
#define GV_PLAYER_SPAWN_GRACE 2.0f

/* Speed at which contact stops hurting the player and starts hurting enemies. */
#define GV_OVERDRIVE_SPEED 640.0f
#define GV_OVERDRIVE_FULL 1150.0f  /* fully saturated visuals + max damage */
/* Fraction of speed retained after a slam kill. Ploughing through fragile
 * enemies barely slows you down, which is what makes chains feel like flight;
 * armoured targets take a real bite out of your momentum. */
#define GV_SLAM_BLEED_MAX 0.94f
#define GV_SLAM_BLEED_MIN 0.62f

#define GV_PULSE_MAX 3.0f
#define GV_PULSE_RECHARGE 0.19f    /* charges per second */
#define GV_PULSE_RADIUS 235.0f
#define GV_PULSE_IMPULSE 1150.0f
#define GV_PULSE_SELF_KICK 210.0f
#define GV_PULSE_COOLDOWN 0.35f

#define GV_FOCUS_MAX 1.0f
#define GV_FOCUS_DRAIN 0.42f       /* per second while held */
#define GV_FOCUS_REGEN 0.22f       /* per second while released */
#define GV_FOCUS_MIN_TO_START 0.12f
#define GV_FOCUS_TIMESCALE 0.42f

/* ------------------------------------------------------------------ tether */
#define GV_TETHER_MAX_LEN 560.0f
/* Kept above the largest rock radius plus the ship radius so that reeling all
 * the way in cannot bury the ship inside its own anchor. */
#define GV_TETHER_MIN_LEN 78.0f
#define GV_TETHER_SPEED 3100.0f
#define GV_TETHER_RETRACT_SPEED 4200.0f
#define GV_TETHER_REEL_SPEED 340.0f
#define GV_TETHER_STIFFNESS 0.92f  /* positional correction applied per step */
#define GV_TETHER_DAMP 0.14f       /* radial velocity damping at the constraint */
#define GV_TETHER_WHIP_BONUS 1.35f /* velocity multiplier on release, capped */
#define GV_TETHER_RELEASE_MAX_BOOST 260.0f

/* ----------------------------------------------------------------- enemies */
typedef enum {
    GV_ENEMY_DRONE = 0,   /* homing chaser, fragile */
    GV_ENEMY_LANCER,      /* telegraphs, then dashes in a straight line */
    GV_ENEMY_SENTINEL,    /* drifts slowly, fires aimed plasma */
    GV_ENEMY_SPLITTER,    /* bursts into drones when destroyed */
    GV_ENEMY_WARDEN,      /* armoured miniboss; needs a hard slam or a rock */
    GV_ENEMY_KIND_COUNT
} gv_enemy_kind;

typedef enum {
    GV_EAI_IDLE = 0,
    GV_EAI_SEEK,
    GV_EAI_WINDUP,
    GV_EAI_DASH,
    GV_EAI_RECOVER,
    GV_EAI_ORBIT
} gv_enemy_ai;

typedef struct {
    gv_v2 pos, vel;
    float angle;         /* facing, radians */
    float spin;          /* visual roll rate */
    float radius;
    float hp;
    float max_hp;
    float accel;
    float max_speed;
    float score;
    float timer;         /* AI phase timer */
    float fire_timer;
    float hit_flash;
    float spawn_anim;    /* 0 -> 1 materialisation; intangible until 1 */
    float stagger;       /* seconds of AI suppression after a shove */
    float armour;        /* extra speed required to slam-kill */
    gv_v2 dash_dir;
    uint16_t gen;
    uint8_t kind;
    uint8_t ai;
    uint8_t split_depth; /* limits recursive splitting */
    bool alive;
    bool tethered;       /* the player's tether is anchored to this enemy */
} gv_enemy;

/* -------------------------------------------------------------------- rocks
 * Drifting debris. Doubles as the tether's main anchor and, once whipped up to
 * speed, as the player's heaviest weapon. */
typedef struct {
    gv_v2 pos, vel;
    gv_v2 prev_pos; /* start of the current step, for swept collision */
    float angle, spin;
    float radius;
    float inv_mass;
    float hit_flash;
    float verts[GV_ROCK_VERTS]; /* per-vertex radius scale, procedural shape */
    uint16_t gen;
    bool alive;
    bool tethered;
} gv_rock;

/* ------------------------------------------------------------------- pylons
 * Static arena anchors. They guarantee the tether always has something to grab,
 * which keeps the movement mechanic available even in a cleared arena. */
typedef struct {
    gv_v2 pos;
    float radius;
    float pulse; /* visual only */
    bool alive;
    bool tethered;
} gv_pylon;

/* ------------------------------------------------------------------ bullets */
typedef struct {
    gv_v2 pos, vel;
    float radius;
    float life;
    bool alive;
} gv_bullet;

/* ------------------------------------------------------------------ pickups */
typedef enum {
    GV_PICKUP_CORE = 0, /* score + multiplier sustain */
    GV_PICKUP_LIFE,
    GV_PICKUP_PULSE,
    GV_PICKUP_KIND_COUNT
} gv_pickup_kind;

typedef struct {
    gv_v2 pos, vel;
    float life;
    float bob;
    uint8_t kind;
    bool alive;
} gv_pickup;

/* --------------------------------------------------------------- particles */
typedef enum {
    GV_PART_SPARK = 0,
    GV_PART_SMOKE,
    GV_PART_SHARD,
    GV_PART_RING,
    GV_PART_TRAIL
} gv_particle_kind;

typedef struct {
    gv_v2 pos, vel;
    float life, max_life;
    float size;
    float angle, spin;
    float drag;
    uint8_t r, g, b;
    uint8_t kind;
    bool alive;
} gv_particle;

typedef struct {
    gv_v2 pos;
    float radius, max_radius;
    float life, max_life;
    float thickness;
    uint8_t r, g, b;
    bool alive;
} gv_shockwave;

/* Rising score text. Cosmetic, but lives in the world so a replay renders
 * identically. */
typedef struct {
    gv_v2 pos, vel;
    float life, max_life;
    int value;
    float scale;
    uint8_t r, g, b;
    bool alive;
} gv_floater;

/* ------------------------------------------------------------------- tether */
typedef enum {
    GV_ANCHOR_NONE = 0,
    GV_ANCHOR_PYLON,
    GV_ANCHOR_ROCK,
    GV_ANCHOR_ENEMY
} gv_anchor_kind;

typedef enum {
    GV_TETHER_IDLE = 0,
    GV_TETHER_FLYING,
    GV_TETHER_ATTACHED,
    GV_TETHER_RETRACTING
} gv_tether_state;

typedef struct {
    gv_v2 tip;      /* world position of the hook */
    gv_v2 tip_vel;
    gv_v2 origin;   /* where it was fired from, for the flying-length check */
    float length;   /* rope rest length while attached */
    float taut;     /* 0..1, smoothed tension for rendering and audio */
    float attach_time;
    uint16_t anchor_gen;
    int16_t anchor_index;
    uint8_t anchor_kind;
    uint8_t state;
} gv_tether;

/* ------------------------------------------------------------------- player */
typedef struct {
    gv_v2 pos, vel;
    gv_v2 prev_pos;     /* start of the current step, for swept collision */
    gv_v2 aim;          /* unit vector toward the aim point */
    float angle;
    float radius;
    /* Cached |vel|. Refreshed at every point in the step that changes velocity
     * (thrust, walls, rope, impacts) so that anything reading it mid-step sees
     * a current value. Code that writes `vel` directly must recompute it or
     * read gv_v2_len(vel) instead. */
    float speed;
    float overdrive;    /* 0..1 ramp derived from speed */
    float invuln;
    float hit_flash;
    float pulse_charge; /* continuous, 0..GV_PULSE_MAX */
    float pulse_cooldown;
    float focus;        /* 0..1 meter */
    float thrust_amount;/* 0..1, for exhaust rendering */
    float trail_timer;
    int lives;
    bool focus_active;
    bool alive;
    gv_tether tether;
} gv_player;

/* -------------------------------------------------------------------- waves */
typedef enum {
    GV_WAVE_INTRO = 0,   /* short breather + banner before spawning */
    GV_WAVE_SPAWNING,
    GV_WAVE_FIGHTING,
    GV_WAVE_CLEARED
} gv_wave_state;

typedef struct {
    int index;           /* 1-based wave number */
    int state;
    float timer;
    float spawn_timer;
    float budget;        /* remaining spawn budget for this wave */
    float fight_time;    /* seconds spent clearing the current wave */
    /* Escalation applied to stragglers, 0..1. A wave-shooter's worst failure
     * mode is the last evasive enemy turning a fight into a chore, so surviving
     * enemies get progressively faster and more committed until they come to
     * the player. */
    float hunt;
    bool warden_wave;
} gv_wave;

/* -------------------------------------------------------------------- score */
#define GV_MULT_MAX 32
#define GV_MULT_WINDOW 3.2f

typedef struct {
    int64_t score;
    int multiplier;
    float mult_timer;    /* seconds left before the multiplier decays */
    int combo;           /* kills in the current chain */
    int best_combo;
    int kills;
    int slam_kills;
    int whip_kills;
    int pulse_saves;
    float time_alive;
    int64_t style_points;
} gv_score;

/* ------------------------------------------------------------------- events
 * One-frame signals drained by the presentation layer (audio, camera shake,
 * haptics). Keeping them here means the simulation stays pure. */
typedef enum {
    GV_EV_TETHER_FIRE = 0,
    GV_EV_TETHER_ATTACH,
    GV_EV_TETHER_MISS,
    GV_EV_TETHER_RELEASE,
    GV_EV_ENEMY_KILLED,
    GV_EV_ENEMY_HIT,
    GV_EV_ENEMY_SPAWN,
    GV_EV_ENEMY_FIRE,
    GV_EV_PLAYER_HIT,
    GV_EV_PLAYER_DIED,
    GV_EV_PULSE,
    GV_EV_PULSE_EMPTY,
    GV_EV_WAVE_START,
    GV_EV_WAVE_CLEAR,
    GV_EV_PICKUP,
    GV_EV_EXTRA_LIFE,
    GV_EV_MULT_UP,
    GV_EV_ROCK_HIT,
    GV_EV_WALL_BOUNCE,
    GV_EV_KIND_COUNT
} gv_event_kind;

typedef struct {
    gv_v2 pos;
    float magnitude; /* 0..1-ish intensity hint for audio and shake */
    int32_t a;       /* kind-specific payload (enemy kind, pickup kind, ...) */
    uint8_t kind;
} gv_event;

/* --------------------------------------------------------------- difficulty */
typedef enum {
    GV_DIFF_CADET = 0,
    GV_DIFF_PILOT,
    GV_DIFF_ACE,
    GV_DIFF_COUNT
} gv_difficulty;

typedef struct {
    float enemy_speed;   /* multiplier */
    float enemy_accel;
    float spawn_budget;  /* multiplier on per-wave budget */
    float spawn_rate;
    float bullet_speed;
    float score_mult;
    int start_lives;
} gv_tuning;

/* -------------------------------------------------------------------- input
 * The simulation's only view of the outside world. Already normalised and
 * deadzoned by the platform layer, so tests can drive it directly. */
typedef struct {
    gv_v2 move;       /* thrust axis, magnitude clamped to 1 */
    gv_v2 aim_point;  /* world-space point the ship aims at */
    float reel;       /* +1 reel in, -1 pay out */
    bool tether_held;
    bool tether_pressed;
    bool pulse_pressed;
    bool focus_held;
} gv_input;

/* --------------------------------------------------------------- the world */
typedef enum {
    GV_RUN_PLAYING = 0,
    GV_RUN_DYING,      /* death animation before the game-over screen */
    GV_RUN_GAME_OVER
} gv_run_state;

typedef struct {
    gv_player player;
    gv_enemy enemies[GV_MAX_ENEMIES];
    gv_rock rocks[GV_MAX_ROCKS];
    gv_pylon pylons[GV_MAX_PYLONS];
    gv_bullet bullets[GV_MAX_BULLETS];
    gv_pickup pickups[GV_MAX_PICKUPS];
    gv_particle particles[GV_MAX_PARTICLES];
    gv_shockwave shockwaves[GV_MAX_SHOCKWAVES];
    gv_floater floaters[GV_MAX_FLOATERS];

    int enemy_count;   /* live enemies, maintained incrementally */
    int rock_count;
    int pylon_count;

    gv_wave wave;
    gv_score score;
    gv_tuning tuning;
    int difficulty;

    gv_rng rng;   /* gameplay stream: affects outcomes */
    gv_rng fxrng; /* cosmetic stream: never affects outcomes */
    uint64_t seed;

    float time;        /* seconds of simulated time */
    float time_scale;  /* focus / death slowdown, applied by the caller */
    /* Cosmetic particle density, 0..1, from the settings menu. Only ever
     * scales counts drawn from the cosmetic RNG stream, so lowering it cannot
     * change the outcome of a run. */
    float fx_quality;
    float death_timer;
    float arena_w, arena_h;
    int run_state;

    /* Presentation feedback produced by the simulation. */
    float shake;       /* 0..1 trauma, decays; renderer squares it */
    float hitstop;     /* seconds of remaining freeze */
    gv_event events[GV_MAX_EVENTS];
    int event_count;
    bool events_dropped;

    /* Rolling ids so that pooled slots can be referenced safely. */
    uint16_t next_gen;
} gv_world;

#endif /* GV_TYPES_H */
