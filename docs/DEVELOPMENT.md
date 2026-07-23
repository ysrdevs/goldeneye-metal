# Development guide

This is the short version. Current progress is tracked in
[GOLDENEYE_NATIVE_METAL_PROJECT_STATUS.md](GOLDENEYE_NATIVE_METAL_PROJECT_STATUS.md), and release
signing/notarization is covered by [MACOS_DISTRIBUTION.md](MACOS_DISTRIBUTION.md).

## Repository layout

This is one repository:

- `src/` and `include/` contain the ReXGlue runtime and native Metal backend.
- `vendor/GoldenEye-Recomp/` contains the GoldenEye integration.
- `thirdparty/` contains source dependencies managed as Git submodules.
- `tools/` contains benchmark, packaging, and diagnostic utilities.

Generated game code and game data are not committed. Building the GoldenEye target requires a
compatible local `default.xex` that you are authorized to use.

## Build the runtime on Apple Silicon

```sh
brew install cmake spirv-cross
git submodule update --init --recursive

cmake --preset macos-arm64-release
cmake --build --preset macos-arm64-release --parallel
ctest --preset macos-arm64-release --output-on-failure
```

The Metal build requires SPIRV-Cross with MSL support. Configuration fails if that support is
missing.

## Generate and build GoldenEye

Place your compatible XEX at:

```text
vendor/GoldenEye-Recomp/assets/default.xex
```

Then run:

```sh
./out/macos-arm64/rexglue codegen \
  vendor/GoldenEye-Recomp/ge_manifest.toml

cmake -S vendor/GoldenEye-Recomp --preset macos-arm64-release
cmake --build vendor/GoldenEye-Recomp/out/build/macos-arm64-release \
  --target ge --parallel
```

Generated files are written under `vendor/GoldenEye-Recomp/generated/` and must not be committed.

## Run a development build

Use a complete extracted game-data folder containing `default.xex`, `files/`, and the companion
audio data:

```sh
DYLD_LIBRARY_PATH="$PWD/out/macos-arm64" \
REX_INPUT_BACKEND=sdl REX_MNK_MODE=true \
./vendor/GoldenEye-Recomp/out/build/macos-arm64-release/GoldenEye \
  --game_data_root /absolute/path/to/game-data \
  --gpu metal
```

You can also double-click `Launch GoldenEye.command` from the repository root. Game data must stay
local and must never be committed.

## Build the macOS app

```sh
./launcher/build-app.sh
```

The unsigned app is created at:

```text
vendor/GoldenEye-Recomp/out/build/macos-arm64-release/dist/GoldenEye Metal.app
```

The build script runs the release verification gate, checks portable arm64 linkage and metadata,
and confirms that no recognizable game data entered the bundle.

Signing, notarization, ZIP, and DMG creation are separate release-owner steps documented in
[MACOS_DISTRIBUTION.md](MACOS_DISTRIBUTION.md).

## Tests

Run the complete runtime suite:

```sh
ctest --preset macos-arm64-release --output-on-failure
```

Run the complete GoldenEye app gate:

```sh
cmake --build vendor/GoldenEye-Recomp/out/build/macos-arm64-release \
  --target goldeneye_macos_app_verify --parallel
```

That gate covers the launcher, save manager, crash recovery, host pause, testing controls, shader
presentation, MetalFX scaling, crash guards, bundle layout, and linkage.

After changing Metal submission, resource ownership, depth/stencil, or presentation, also run:

```sh
MTL_DEBUG_LAYER=1 ./out/macos-arm64/metal_pipeline_probe_test
```

Useful focused targets include `metal_resolve_test`, `metal_pipeline_probe_test`,
`metal_presenter_shader_test`, `metal_metalfx_scaler_test`, and
`metal_texture_decode_validation_test`.

## Dam benchmark

The deterministic benchmark uses normal game input to reach Dam, discards warm-up windows, and
measures complete 64-frame windows:

```sh
./tools/benchmark-dam.sh
```

Results are written to a unique directory under `out/benchmarks/`. They include the raw log, CSV
windows, frame-pacing/wait summaries, build identity, hardware, macOS version, and effective test
environment. Runs use private logs, config, and saves. A separate `out/benchmarks/cache` is reused
so the result never touches the player's cache; set `GOLDENEYE_BENCH_RESET_CACHE=1` for an
explicitly cold run.

Override the data path or sample size only when needed:

```sh
GOLDENEYE_BENCH_GAME_DATA=/absolute/path/to/game-data \
GOLDENEYE_BENCH_WARMUP_WINDOWS=8 \
GOLDENEYE_BENCH_MEASURE_WINDOWS=48 \
./tools/benchmark-dam.sh
```

The benchmark refuses stale runtime or game builds, accepts only complete 64-frame windows, and
distinguishes its own deliberate shutdown from an early exit or crash. Do not report
transition-heavy samples as steady-state performance. Always include the Mac model, warm-up count,
measured-window count, and whether any fallback or Metal error occurred.

## Automated stability and rendering checks

Run repeatable boot-to-Dam and native-quit cycles without using the player's saves or settings:

```sh
./tools/stability-cycle.py --cycles 3 --mode dam
```

Dam captures are useful as raw evidence, but gameplay is not synchronized closely enough for a
pixel reference. Use the deterministic menu route for image regression:

```sh
./tools/stability-cycle.py --cycles 1 --mode menu --capture
./tools/render_regression.py accept \
  out/stability/.../cycle-001/menu.png out/render-references/menu.png
./tools/stability-cycle.py --cycles 3 --mode menu --capture \
  --reference out/render-references/menu.png
```

For Dam screenshots, run `./tools/stability-cycle.py --cycles 3 --mode dam --capture` without a
reference. Reports, logs, captures, and diff images remain under `out/`. Capturing requires macOS
Screen Recording permission. A cycle fails on crashes, renderer failures, incomplete final profile
data, or orphaned child processes. References may contain game assets and must not be committed or
shared.

## Common diagnostics

Diagnostics are opt-in so normal runs remain representative.

| Variable | Purpose |
| --- | --- |
| `GOLDENEYE_AUTO_START=menu` | Automatically reach the dossier menu |
| `GOLDENEYE_AUTO_MISSION=dam` | Continue through the normal menu route into Dam |
| `GOLDENEYE_METAL_PROFILE=1` | Record draw, copy, swap, wait, and presenter timing |
| `REX_METAL_SHOW_FPS=false` | Hide the FPS overlay |
| `GOLDENEYE_METAL_DUMP_SHADERS=1` | Dump translated or failed Metal shaders under `/tmp` |
| `GOLDENEYE_METAL_DUMP_FRAMES=N` | Dump one selected presenter frame under `/tmp` |
| `GOLDENEYE_METAL_VERBOSE_DIAGNOSTICS=1` | Enable high-volume renderer logging |
| `GOLDENEYE_METAL_GPU_TILED_RESOLVE=0` | Compare against the slower CPU resolve fallback |

Auto-start and auto-mission change input only. They do not replace shaders, geometry, command
buffers, resolves, or presentation. Frame and shader dumps may contain copyrighted game content;
keep them out of the repository and do not redistribute them.

## Architecture notes

The accepted rendering path is the game's real PM4 command stream and Xenos microcode translated
into Metal draws, followed by the normal guest-visible resolve and game-requested swap. Diagnostic
replacement rendering is not evidence of production success.

Metal work is submitted asynchronously. Do not remove synchronization before readback, resolve,
swap, resource replacement, cache teardown, or memory-exporting shaders unless ownership and
ordering are proven by a focused regression.

The macOS app uses native Cocoa keyboard/mouse events and SDL gamepads. Host settings pauses active
offline missions through the game's pause state while Metal presentation remains alive. Cheat
requests are queued from the UI and applied only on the guest game thread during a safely paused
offline one-player mission.

The launcher validates and imports only local user-supplied data. Diagnostic exports must continue
to exclude game data, saves, cache, configuration, and remembered source paths.

## Current priorities

1. Improve frame pacing on lower-power Macs and repeatable slow scenes.
2. Reduce remaining GPU wait cost without weakening guest-visible ordering.
3. Finish physical keyboard, mouse, Sony, and Xbox controller validation.
4. Complete shared depth/stencil, depth-only draws, and faithful MSAA behavior.
5. Validate later missions and local split-screen.

## Debugging rules

- Record the exact build, hardware, environment variables, and reproduction path.
- Change one rendering or synchronization assumption at a time.
- Prefer bounded counters and logs over unconditional per-draw output.
- Never advance title-owned fences, ring pointers, or presented counters from a timeout.
- Keep game data, generated code, captures, logs, and benchmark output out of commits.

## Formatting

The repository includes `.clang-format`:

```sh
clang-format -i path/to/touched_file.cpp path/to/touched_file.h
```
