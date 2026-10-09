# thWWW-switch — update 3

- **Safer saves:** `Data.ini` holds all progress, unlocks, spell history and scores and is rewritten whenever a spell card starts. It was overwritten in place, so a crash or power loss at that moment could leave it empty. Saves are now written to a `.tmp` file and swapped in; a complete `.tmp` left by an interrupted swap is picked up on the next launch.

---

# thWWW-switch — October 2026 update

## What's new

- **Stage 5 background fixed.** The boat interior is the tunnel from the PC game again (walls, ceiling, wooden floor, the dark opening ahead) instead of a flat, almost top-down view with black triangles at the bottom.
- **Faster dense patterns.** The port's own CPU work on dense Lunatic patterns is about 24% lower. It runs at stock clocks; **an overclock is recommended for Lunatic playthroughs**: at the stock 1020 MHz the hardest spell cards run at 49–57 FPS (mostly around 55) (every bullet runs the game's GameMaker scripts through an interpreter), and with the CPU overclocked (tested docked with a ~1.7 GHz overclock) they hold 60 FPS. A smaller overclock will probably also work.
- **Remappable controls.** Option → Key Config can rebind shot, focus, bomb and pause. Defaults are unchanged (B shoot, A bomb, L/ZL focus, + pause, R/ZR hold to skip dialogue); X and Y are free to bind. ZL and ZR now work like L and R.
- **Your own name.** High scores and replays use the game's name entry again instead of the fixed name `SWITCH`.

## Fixes under the hood

- Sprites are drawn at their layer depth (GameMaker's rule for 3D cameras) and clipped by GameMaker's Direct3D-style near plane, so each stage's 3D background stays in the background view. This replaces the old "draw bands" workaround.
- Instances on hidden room layers are no longer drawn (the playfield walls on thWWW's hidden `Wall` layer showed up as green lines on stage 2 once depth was fixed).
- Faster collisions for player shots, an O(1) object-ancestry table, a cheaper dead-reference sweep, grouped draw-list sorting, less per-instruction interpreter overhead, and `-O3`. Game state is identical to the previous build frame by frame across Lunatic stages 1–6.

Install as before: replace `thwww.nro` in `/switch/thwww/`. Your `save/` folder is kept.

---

# thWWW-switch 1.0.1 — release notes (September 2026)

Release date: 9 September 2026

## Highlights

- Native Nintendo Switch `.nro` for Wonderful Waking World 1.0.1.
- Hardware-confirmed runtime fixes for BGM, post-spell background restoration, gameplay draw order, player damage, and dense danmaku.
- Fixed Saekaze-style controls:
  - Left stick / D-Pad: movement
  - B: shoot / confirm
  - A: bomb / cancel
  - L: focus
  - R held: dialogue skip
  - Plus: pause
  - all remaining buttons and the right stick: inert
- R dialogue skip is context-specific and does not become another gameplay shoot button.
- New score entries use the fixed name `SWITCH`; existing leaderboard names are preserved.
- New 256×256 homebrew icon supplied for this release.
- Save and replay writes remain isolated under `/switch/thwww/save/`.

## Runtime corrections retained from the tested build

- Instance activation/deactivation is committed at the end of the outermost GameMaker event, including activation requested by nested Destroy events.
- Pooled instances synchronize correctly with the collision spatial grid when deactivated and reactivated at unchanged coordinates.
- Precise player/danmaku collision masks remain enabled.
- OpenAL resumes only paused sources, avoiding the repeated `alSourcePlay` calls that restarted stage BGM.
- Large runtime-created PCM music is played through a bounded streaming queue rather than one very large OpenAL buffer per track.
- thWWW-specific draw bands keep negative-depth stage backgrounds behind ordinary gameplay while preserving normal depth order inside each band.
- Stage 5 and stage 7 custom vertex/depth routes are supported by the modern GLES renderer.
- Recorded-cell spatial-grid removal reduces dense stage-5 host CPU time by approximately 20–24%, with byte-identical game-state checkpoints.

## Validation summary

Deterministic regressions pass with the supported official `data.win`:

```text
PASS: pooled precise danmaku collision registered (miss=2, life=-1)
PASS: dense precise-graze workload preserved (graze=41, danmaku=330, graze_boxes=298, miss=0)
```

The Release host build, OpenAL/modern-GL route, warnings-as-errors devkitA64 cross-build, NRO metadata, embedded icon, and legal-data exclusion are checked before packaging. See [`VALIDATION.md`](VALIDATION.md) for details.

## Required game data

The NRO does not include the game. Obtain Wonderful Waking World 1.0.1 from:

<https://oligarchomp.itch.io/wonderful-waking-world>

Install the official files and `thwww.nro` under `/switch/thwww/`. Launch using hbmenu title takeover/full-memory mode.
