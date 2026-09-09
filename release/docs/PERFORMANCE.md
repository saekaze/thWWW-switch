# thWWW-switch dense-danmaku performance investigation

Validation date: 9 September 2026.

## Finding

The reported slowdown was reproducible as a native runner CPU hotspot, not as a consequence of weakening or bypassing player collision. In a deterministic stage-5 practice route, `SpatialGrid_removeInstance` was the largest sampled native function: it scanned every spatial-grid cell whenever a moving instance was re-indexed, even though each instance already records the exact cells containing it. thWWW moves both a danmaku instance and its associated graze-box instance, so dense patterns multiplied that unnecessary whole-grid scan hundreds of times per frame.

The renderer was also reviewed. The Switch modern-GL path already batches up to 4,096 compatible quads and flushes on texture/type/state boundaries; no speculative draw-order or collision simplification was made.

## Fix

`src/spatial_grid.c` now:

- removes ordinary moving instances only from their recorded cells;
- retains a defensive whole-grid recovery when non-empty tracking data is demonstrably inconsistent;
- treats an empty tracking list as “not inserted/no valid collision box,” avoiding a whole-grid scan for every newly created bullet;
- clears pending dirty state when an inactive/destroyed instance is removed before its first grid insertion, so later pooled reactivation reliably re-queues it.

Cell-array order is still preserved with stable removal. Collision candidate order, precise masks, query de-duplication, object ancestry filtering, and gameplay scripts are unchanged.

## Profile evidence

Ten identical gprof-instrumented stage-5 runs were merged for each build (fixed seed `12648430`, frames 0–1600, WAD17-only no-op host build):

| Metric | Baseline | Optimized |
|---|---:|---:|
| Total sampled CPU | 11.01 s | 7.23 s |
| `SpatialGrid_removeInstance` self time | 3.39 s (30.79%) | 0.40 s (5.46%) |
| Removal-hotspot reduction | — | 88.2% |

The table above preserves the merged profile results used for the optimization decision.

A separate warnings-as-errors Release benchmark alternated baseline/optimized runs to reduce ordering bias. Values below are medians of 10 runs per build and include process startup/game parsing:

| Deterministic route | Baseline wall | Optimized wall | Wall reduction | CPU reduction |
|---|---:|---:|---:|---:|
| Through frame 850 | 0.5025 s | 0.4039 s | 19.63% | 19.64% |
| Through frame 1600 | 1.2961 s | 0.9852 s | 23.99% | 23.91% |

The benchmark table above preserves the medians from the alternating baseline/optimized runs.

These are host CPU measurements, not Tegra X1 hardware numbers. They demonstrate that the reported workload had a concrete native hotspot and that the fix materially reduces it. The corrected port was subsequently confirmed on physical Switch hardware.

## Collision and state preservation

At frame 850, the deterministic stage-5 scene contains 700 active instances, including 330 danmaku and 333 graze boxes. At frame 1600 it contains 204 danmaku and 204 graze boxes. Complete baseline and optimized runner-state JSON dumps are byte-identical at both checkpoints:

- frame 850: `a56d41d68829563f1fb9ca0c1513354c8e56619b98cbfd47a707fe2d31f079a9`
- frame 1600: `3cca0beaac741135a4faf7caa8993f77b2297309dc5a378563f19e0c09d7c2e1`

The expanded end-to-end regression also focus-moves through the first dense stage-5 wave and reports:

```text
PASS: dense precise-graze workload preserved (graze=41, danmaku=330, graze_boxes=298, miss=0)
```

The original deliberate-collision check still reports:

```text
PASS: pooled precise danmaku collision registered (miss=2, life=-1)
```

Both scenarios pass in the Release WERROR build and under AddressSanitizer (with known unrelated upstream shutdown leak detection disabled).

## Rendering/audio recheck

The final host OpenGL/OpenAL build was run through stage 1 and the dense stage-5 route after the spatial-grid change. The expected bullet field remained visible over the 3D background. Runtime PCM title/stage music was created and played without reported OpenGL or OpenAL API errors.

## Switch release

The public release is cross-built with devkitA64 GCC 15.2.0, WAD17-only modern GLES/OpenAL, and warnings as errors. Release artifact hashes are recorded in `release/SHA256SUMS`; the exact source revision used by the build is recorded in `release/SOURCE-COMMIT.txt`.

Static inspection verifies the supplied 256×256 JPEG icon, NACP metadata (`Wonderful Waking World`, `Oligarchomp / thWWW-switch`, version `1.0.1`), and an empty RomFS. Proprietary game data is not embedded.
