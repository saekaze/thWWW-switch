# thWWW-switch validation report

Validation date: 9 September 2026.

## Scope

The first physical-Switch build launched and played SFX but exposed silent BGM, ordinary gameplay sprites hidden behind stage backgrounds, incorrect post-spell background restoration, and missing player damage. Those defects were corrected, and the resulting runtime NRO (`a60e41bf1472320b47a22f9441c9d214b8632353c875bcbe21c36bdf9b7dfa0e`) was retested on physical Switch hardware and confirmed working. The public thWWW-switch source retains those corrections and is validated by:

1. a real warnings-as-errors devkitA64/libnx cross-build to ELF/NACP/NRO;
2. static artifact, NACP, and embedded-icon inspection;
3. deterministic execution of the same runner/game code in Linux host builds with SDL2, OpenGL, and OpenAL;
4. title-level pooled-danmaku/precise-mask collision and dense-graze regressions;
5. native sampling plus alternating baseline/optimized Release benchmarks of the dense stage-5 workload;
6. focused overlay-filesystem checks.

The hardware tester's verdict on the corrected runtime was “works perfect.” The public build changes only the audited controller/score compatibility paths, icon/metadata, documentation, and reconstructed source for the same event/audio fixes; host regressions and the Switch cross-build are rerun before release.

## Compatibility coverage

- Official `data.win`: WAD 17 / GameMaker 2022.5.0.0.
- 471 functions are referenced by the game's bytecode.
- Runner compatibility scan after the port: 471 implemented, 0 unknown.
- Runtime TTF atlases generated successfully:
  - MochiyPopOne: 1,310 glyphs, 1024×512;
  - Unifont: 1,321 glyphs, 1024×256 (two fonts);
  - umeboshi: 1,320 glyphs, 1024×512;
  - MadouFutoMaruGothic: 1,322 glyphs, 1024×256.

## Deterministic graphical runs

Input was injected from frame-indexed JSON to avoid timing/focus ambiguity. The title intro holds menu input locked for approximately 330 frames, so accepted presses were deliberately scheduled after that lockout.

| Route | Result |
|---|---|
| Startup/language | Successful |
| Title/menu | Successful |
| Normal stage 1 | Corrected draw band exposes player, boss, bullets, and items over the active stage background |
| Practice stage 5 | Corrected draw band plus custom-vertex texture handling exposes gameplay over the 3D background |
| Extra stage 7 | Successful |
| Japanese stage 1 | Successful |

The fixed stage 5 and stage 7 logs contain no OpenGL API errors. Their custom 3D backgrounds exercise position/color/UV vertex buffers, triangle lists, depth state, and surface texture handles.

## Audio lifecycle

A graphical OpenAL run exercised thWWW's actual runtime-buffer music path:

```text
Audio: OpenAL engine initialized
Audio: Created runtime PCM sound 200000 (17640 bytes, 16-bit, 44100 Hz, 2 channel(s))
Audio: Playing runtime PCM sound 200000 as instance 100000 (loop=1)
Audio: Destroyed runtime PCM sound 200000
Audio: Created runtime PCM sound 200000 (16844904 bytes, 16-bit, 44100 Hz, 2 channel(s))
Audio: Playing runtime PCM sound 200000 as instance 100000 (loop=1)
```

The corrected backend queues four small PCM buffers and replenishes them during playback rather than allocating one 16–42 MiB OpenAL buffer per track. Track-position changes rebuild the queue at the requested sample, preserving thWWW's authored intro/loop points. The title and stage 1/5 transitions completed with no OpenAL API failures.

## Pooled-danmaku collision and graze regressions

`scripts/test-thwww-regressions.sh` first launches stage 1 with fixed input and holds Reimu at the top of the playfield. This exercises the real `obj_danmaku_spawn` deactivate/reactivate pool, unchanged-coordinate restoration, the spatial grid, `instance_place(parent_hitbox)`, and the authored precise masks. The corrected run reports:

```text
PASS: pooled precise danmaku collision registered (miss=2, life=-1)
```

The same route before active-state/grid synchronization ended with `miss=0` and `life=2` despite 489 grazes.

The second scenario focus-moves through stage 5's first dense wave. It validates successful grazing without a miss while hundreds of bullet and graze-box instances are active:

```text
PASS: dense precise-graze workload preserved (graze=41, danmaku=330, graze_boxes=298, miss=0)
```

Both scenarios pass in warnings-as-errors Release and AddressSanitizer host builds.

## Dense danmaku/graze performance

Native sampling identified `SpatialGrid_removeInstance` as the demonstrated hotspot. It scanned every room-grid cell whenever any moving instance was re-indexed even though the instance records its exact occupied cells. thWWW moves both each bullet and its associated graze box, multiplying that scan during dense patterns.

The corrected path removes instances only from recorded cells and retains a full-grid recovery only when non-empty tracking data is inconsistent. It also clears pending dirty state when a pooled instance is deactivated before its first insertion, ensuring later reactivation is re-queued rather than omitted.

Ten merged profiling runs reduced removal self-time from 3.39 s to 0.40 s (88.2%). Alternating Release benchmarks, ten runs per build, measured 19.63% less wall/CPU time through dense frame 850 and 23.99% less through frame 1600. These are host CPU results, not Tegra X1 measurements.

Most importantly, baseline and optimized full game-state dumps are byte-identical at both checkpoints. Frame 850 contains 700 active instances (330 danmaku and 333 graze boxes). No collision broad-phase, precise mask, or gameplay rule was disabled. The summarized timings and state hashes are recorded in `PERFORMANCE.md`.

## Fixed Switch controls and score name

Saekaze's Touhou 7 controller source was inspected rather than inferring its layout from a README. Its Switch branch uses fixed physical actions and gives R a dedicated held skip bit. thWWW's `obj_dialogue` bytecode was then inspected: its repeat-skip timer consumes `global.shot_down`, while normal progression consumes `global.shot_pressed`.

The Switch backend presents only B=FACE1 (shoot), A=FACE2 (bomb), L=FACE3 (focus), Plus=SHOULDERL (pause), the left stick/D-Pad, and a separate R slot. A Switch-only thWWW compatibility read feeds that R slot into `obj_dialogue`'s held `shot_down` read. It does not affect gameplay reads, so R is not another fire button. Writes to thWWW's four configurable gamepad globals are fixed on Switch, preventing an old `Data.ini` mapping from changing the physical layout. Other buttons and the right stick are not exposed.

The official score/name bytecode was also inspected. `obj_score_entry` clears `global.name_entry`, `obj_name_entry` normally supplies `NO_NAME` for an empty value, and the score insertion copies only the current global name into the new leaderboard position. On Switch, writes to that current global are fixed to `SWITCH`. Existing names loaded into the leaderboard arrays are not rewritten.

The supplied 447×447 JPEG was converted directly to RGB 256×256 NRO/public assets with Lanczos resampling. The source and generated hashes are recorded in the release package.

## Save overlay

Focused host checks used an otherwise read-only bundle directory and separate save directory:

- First launch created `save/Data.ini` and did not create/modify `bundle/Data.ini`.
- A subsequent launch read the save copy through a path based on `working_directory` and entered `room_main` directly.
- Absolute/working-directory paths are redirected only when rooted in the configured bundle; unrelated absolute paths retain their normal meaning.
- Nested write parents are created recursively.

The same resolver handles working-directory-prefixed replay files under `/switch/thwww/save/`.

## Remaining platform test coverage

- The corrected runtime was physically confirmed; the exact public artifact additionally contains the audited fixed-control, R-skip, score-name, icon, and metadata changes described above.
- Performance measurements are host CPU comparisons, not Tegra X1 profiling.
- Docking/undocking, repeated Home Menu interruptions, and very long sessions have not been exhaustively characterized.
- Applet-mode memory may be insufficient. Title takeover/full-memory mode remains the supported launch method.
