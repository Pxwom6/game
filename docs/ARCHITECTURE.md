# GRAVITON — architecture

C11 and SDL2. No other dependencies, no asset files, no code generation, and no
build system beyond a Makefile.

## The layering rule

```
  src/core/    maths, RNG                       no dependencies at all
  src/game/    the simulation                   depends on core only
  src/render/  neon renderer, HUD, menus, font   depends on core + game + SDL
  src/audio/   synthesiser and soundtrack        depends on game + SDL
  src/app/     window, input, screens, save      depends on everything
```

Dependencies only ever point downwards, and the line that matters is the one
under `src/game/`: **the simulation does not know SDL exists.** It takes a
`gv_input` struct and a float, and mutates a `gv_world`. That is the whole
interface.

That single constraint is what makes the test suite possible. `make test` links
`tests/*.c` against `src/core` and `src/game` and nothing else, so the entire
simulation is exercised with no window, no audio device, no filesystem and no
SDL — which is why the suite runs in a couple of seconds and why it is easy to
run half a million assertions against it.

`src/app/gv_save.c` is the one file outside `src/game/` the tests also link: it
holds the save *format* and the score table with no SDL in it, while
`gv_save_path.c` holds the part that has to ask the platform where the
preferences directory is. The split exists so that "never trust the save file"
can actually be tested against deliberately corrupt input.

## Determinism

The simulation is deterministic to the bit, from a seed:

- **Fixed 120 Hz timestep.** The frame loop accumulates real time and steps the
  simulation in whole `1/120 s` increments, up to 8 per frame. Rendering
  interpolates between steps; the simulation never sees a variable `dt`.
- **Two independent RNG streams.** `world.rng` drives gameplay; `world.fxrng`
  drives particles and other cosmetics. They are separate so that turning
  particle density down — a settings option — cannot change the outcome of a
  run.
- **PCG32**, with rejection sampling on bounded draws so there is no modulo
  bias.
- **`-ffp-contract=off`** in the build, so the compiler cannot fuse
  multiply-adds and make an optimised build drift from a debug one.
- **`gv_world` is plain data**: no pointers into itself, no allocation. It can
  be `memcpy`'d, snapshotted and compared byte for byte, which is exactly what
  the replay tests do.

## Memory

Every pool is fixed size and lives inside `gv_world` — 192 enemies, 48 rocks,
3072 particles, and so on. **Nothing is allocated during a frame.** Spawning
past a pool's capacity fails gracefully and is covered by explicit
pool-exhaustion tests; pooled handles carry generation counters so a stale one
cannot resolve to a recycled slot.

`gv_world` is a few hundred KB, which is why it lives behind a single `malloc`
at startup rather than on the stack.

## Rendering

No shaders. The neon look is built the way it was before pixel shaders existed:
every element is drawn two or three times — a wide dim halo, a mid-width body
and a near-white core — additively blended over black.

- **One texture atlas**, 256×128, generated at startup: a radial glow and a
  linear gradient with a 2-pixel gutter between them so bilinear filtering
  cannot bleed one into the other. Every primitive in the game is a transformed
  quad from that atlas, so the whole frame batches into very few draw calls.
- **Bloom without shaders.** `SDL_BLENDMODE_MOD` of the scene against itself
  squares the colour, which is a bright-pass; two downsample levels are then
  composited back additively. Where render targets are unavailable the game
  simply draws without the haze and is otherwise identical, which is what lets
  the tests run against SDL's software renderer.
- **The font is code.** A single-stroke vector typeface on a 6×8 grid, drawn
  through the same glow pipeline as everything else, so text scales and blooms
  like the rest of the game.

## Audio

A 48-voice synthesiser, mixed in the SDL audio callback: sine, saw, square,
triangle and noise oscillators, AD envelopes, a one-pole low-pass, constant-power
panning and a feed-forward limiter on the master bus. Every sound effect is a
handful of voices described in code.

The soundtrack is a four-bar A-minor sequencer whose density follows a running
estimate of how dangerous the arena currently is — it thickens when you are in
trouble. That makes it information as well as atmosphere.

## Application layer

`gv_app` owns the screens (title, play, pause, settings, scores, help, game
over) and the transitions between them. The title screen runs a live demo behind
the menu — the same autopilot the balance probe uses, playing the real game.

`gv_input` normalises keyboard, mouse and gamepad into the single `gv_input`
struct the simulation consumes, and maintains a small queue of menu navigation
actions drained once per frame.

Settings and high scores live in a small human-readable key/value file at
`~/Library/Application Support/Graviton/graviton.cfg`. It is written atomically
(temp file plus rename), and **nothing read back from it is trusted**: every
value is range-checked, unparseable values fall back to their defaults, unknown
keys are ignored so a file written by a newer build still loads, and the parser
bounds its own work so a pathological file cannot stall startup.

## Testing

| | |
|---|---|
| `make test` | 100 tests, ~500,000 assertions, simulation only |
| `make sanitize` | the same suite under ASan + UBSan with leak detection |
| `make smoke` | the whole application, headless, through every screen |

The suite is self-registering: `GV_TEST(name) { ... }` uses a constructor
attribute, so adding a file to `tests/` is the only wiring needed.

What it covers, beyond the obvious:

- **Determinism and replay** — the same seed and input produce a byte-identical
  world.
- **Alternate timesteps** — 1/240 through 1/30, to catch anything that has
  quietly become frame-rate dependent.
- **Soak and fuzz** — 12 seeds × 3 difficulties driven by random input, plus
  8 seeds × 3 difficulties driven by the autopilot, asserting invariants
  throughout (nothing NaN, no pool corruption, counts consistent).
- **Hostile inputs** — NaN and infinity fed into aim, movement and reel; the
  maths layer has explicit regression tests for the normalisation paths that
  once leaked a NaN into the ship's heading.
- **Pool exhaustion** — every pool driven past capacity.
- **The save file** — corrupt, truncated, binary, absurdly long, NUL-embedded
  and hand-edited files, plus score-table ranking, ties and capping.

`make smoke` is the other half. It runs the real `gv_app` against SDL's dummy
video and audio drivers, walks every screen by *label* rather than by counting
keypresses, drives every settings control past both ends of its range, plays two
runs to completion, exercises window resizes and degenerate frame deltas
(0, −1, 1000, NaN), and round-trips the save file through the real loader. It
writes a screenshot of each screen to `build/shots`, which is how the interface
is reviewed without a display — that review is what caught the arena being too
large to read, pylons and Sentinels being indistinguishable hexagons, and rocks
being too dark to see.

It sandboxes `HOME` to its output directory, so running it cannot overwrite a
player's real settings.

## Packaging

`tools/build_macos.sh` produces `dist/GRAVITON.app` and, with `--dmg`, a
compressed disk image.

1. Finds SDL2 — the framework from libsdl.org first, then a Homebrew dylib.
2. Reads the architectures that SDL2 actually contains with `lipo` and narrows
   the build to those, because a universal binary cannot link against a
   single-architecture library.
3. Builds and runs the test suite against the binaries being shipped.
4. Assembles the bundle: `Info.plist`, `PkgInfo`, the executable, the icon.
5. Copies SDL2 inside and rewrites the install names to `@rpath`, with
   `-Wl,-rpath,@executable_path/../Frameworks` linked in.
6. **Verifies** with `otool -L` that the executable references nothing outside
   `/usr/lib`, `/System/Library` and the bundle itself, and fails the build if
   it does.
7. Ad-hoc signs the nested library first, then the executable, then the bundle —
   in that order, because a bundle's signature covers what it contains.

The icon is not a file in the repository. `tools/gv_icon.c` draws it with the
game's own renderer, masks it to the rounded-square silhouette macOS expects,
and writes a complete `.iconset` for `iconutil`. Small sizes are cut from their
own heavier-stroked plate, because artwork designed at 1024 px dissolves into a
smudge at 16. It carries a small hand-written PNG encoder — stored (uncompressed)
deflate blocks, so it needs an Adler-32 and a CRC-32 and no zlib — to keep the
tool dependency-free.
