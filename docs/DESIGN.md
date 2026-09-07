# GRAVITON — design notes

## The one rule

Every enemy has a **breaking speed**:

```
required = GV_OVERDRIVE_SPEED (640 px/s) + enemy armour
```

Touch it above that and you destroy it. Touch it below and it takes a life.
There is no third outcome, and there is no weapon that bypasses it.

Everything else in the game exists to make that one comparison interesting.

| Archetype | Armour | Breaking speed | Health |
|---|---|---|---|
| Drone | 0 | 640 | 1 |
| Splitter | 40 | 680 | 1 |
| Lancer | 60 | 700 | 1 |
| Sentinel | 140 | 780 | 2 |
| Warden | 360 | 1000 | 3 |

The player's ceiling is 1900 px/s, so nothing is ever unreachable — but the
Warden demands most of the arena as a run-up, or a rock thrown at it.

### Why it is legible

A rule this sharp only works if the player can see which side of it they are on,
continuously and without doing arithmetic. Two readouts carry that:

- The **overdrive meter** turns gold the moment the ship is above the base
  threshold, and fills as it climbs beyond.
- Every enemy wears an **armour ring**, drawn solid gold when the player's
  *current* speed would break *that specific enemy* and dim when it would not.

So "can I hit this?" is answered by looking at the thing you want to hit. No
numbers, no menus.

## Movement is the weapon

Thrust alone tops out well short of the interesting speeds. The tether is how
you get there.

- **Fire** it at a pylon (fixed) or a rock (movable) within 560 px.
- The rope is **inextensible and one-way**: it pulls, it never pushes. Slack
  rope applies no force at all, so you can fall towards an anchor freely and the
  constraint only engages when the rope goes taut.
- **Reeling in conserves angular momentum.** Halve the radius and the tangential
  speed doubles — the skater-pulling-their-arms-in trick, and the main way to
  build a lethal velocity. A per-step clamp stops a single frame from
  multiplying speed absurdly, but over an arc the physics is genuine.
- **Releasing** adds a modest tangential boost (up to 260 px/s) on top of the
  speed you built, so a well-timed release beats a panicked one.

The consequence is a rhythm the player learns without being told: swing wide to
build, reel to sharpen, release into the thing you want to break, then survive
the moment afterwards where you are slow and exposed.

### Rocks

Rocks are the second weapon. Tether one, swing it, and let it go into a crowd: a
rock above its own lethal speed destroys what it hits, and a **whip kill pays
double**. Rocks never hurt the player — that is deliberate. The fantasy is
throwing furniture around, and a fantasy that punishes you for engaging with it
is not a fantasy.

## The two safety valves

**Pulse** (3 charges, ~5 s each to recharge) shoves everything within 235 px
away, deletes incoming fire, and kicks the ship in the opposite direction. It is
both an escape and a movement tool.

**Focus** slows the world to 42% speed while held. It drains in about 2.4
seconds and refills in about 4.5. It buys a decision — which anchor, which
target — not an escape.

Neither does damage. The player's only damage source is their own momentum.

## Waves

Waves are **budget-driven, not scripted**. Each wave receives a pool of points
and spends them on a randomised mix of whatever archetypes have unlocked:

```
budget = 3.0 + 1.75·(wave−1) + 0.045·(wave−1)²
```

Linear early, gently super-linear later — ahead of a player who is getting
better at the tether, without becoming a wall. Archetypes unlock at waves 3
(Lancer), 5 (Sentinel) and 7 (Splitter), and a Warden arrives every tenth wave.
A hard cap of 58 simultaneous enemies applies whatever the budget says: past
that the screen stops being readable, and unreadable is not the same as
difficult.

### The straggler problem

Budget-driven waves have a characteristic failure: the last one or two enemies
run away and the wave dribbles out over a minute of nothing happening. So after
8 seconds of grace, surviving enemies begin to **escalate** over a 16-second
ramp — up to +85% acceleration, +60% top speed, and Sentinels closing 72% of
their standoff distance. The HUD shows `PURSUIT` while it is active.

The wave ends when the arena is clear. It ends *quickly* because the arena comes
to find you.

## Scoring

The chain **multiplier** climbs by one per kill and resets to ×1 if the chain
window expires or the player is hit. It is the tension dial: it rewards staying
fast and punishes playing safe, which is the behaviour the whole design wants.

```
value = base × cause × speed × multiplier
        cause = 2.0 for a whip kill, 1.0 otherwise
        speed = 1.0 + 0.6 × overdrive
```

Getting hit costs a life *and* the multiplier. Losing the chain usually stings
more.

## Difficulty

| | Enemy speed | Enemy accel | Spawn budget | Spawn rate | Bullets | Lives | Score |
|---|---|---|---|---|---|---|---|
| **CADET** | ×0.84 | ×0.82 | ×0.78 | ×0.80 | ×0.82 | 4 | ×0.85 |
| **PILOT** | ×1.00 | ×1.00 | ×1.00 | ×1.00 | ×1.00 | 3 | ×1.00 |
| **ACE** | ×1.16 | ×1.18 | ×1.30 | ×1.28 | ×1.18 | 2 | ×1.30 |

Measured with the autopilot over six seeds each (`make tools && build/gv_balance`):

| | Mean wave | Mean score | Mean run | Deaths |
|---|---|---|---|---|
| CADET | 10.8 | 62,288 | 360 s | 4 / 6 |
| PILOT | 9.5 | 72,080 | 316 s | 5 / 6 |
| ACE | 7.2 | 85,670 | 223 s | 6 / 6 |

That spread is the intended shape: CADET lets a run breathe, ACE ends it, and
score rises with difficulty fast enough that the harder settings are where the
leaderboard lives. The autopilot is a competent-but-unimaginative player, so
treat these as a floor rather than a target.

## Look and feel

Neon vectors on black, drawn the way it was done before pixel shaders: every
element painted two or three times — a wide dim halo, a mid-width body, a
near-white core — additively blended, with an optional bloom pass on top. No
shaders are used anywhere, which is what lets it run identically on the software
renderer used by the tests.

Shape language carries meaning: **anything that can hurt you has straight
edges**, and **anything you can grab onto is round**. Enemies are polygons;
pylons are ticked rings; rocks are irregular but rounded. That distinction was
added after review shots showed pylons and Sentinels were both hexagons and
genuinely hard to tell apart at speed.

Sound is synthesised in full — a 48-voice engine with a four-bar A-minor
sequencer whose density follows how dangerous the arena currently is. It gets
busier when you are in trouble, which is information as much as atmosphere.
