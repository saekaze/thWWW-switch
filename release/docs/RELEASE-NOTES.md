# thWWW-switch 1.0.1 — release notes

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
