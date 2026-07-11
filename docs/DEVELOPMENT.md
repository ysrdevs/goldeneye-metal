# Development guide

## Source model

This is one repository. The runtime/toolchain lives at the root and the game integration is
vendored as ordinary source under `vendor/GoldenEye-Recomp/`. Only source dependencies under
`thirdparty/` are Git submodules.

Generated C++ and game data are intentionally excluded. A clean checkout can build the toolchain
and Metal isolation tests without game data; building the game target requires locally generated
sources from an authorized compatible XEX.

## Apple Silicon build

```sh
brew install cmake spirv-cross
git submodule update --init --recursive

cmake --preset macos-arm64-release
cmake --build --preset macos-arm64-release --parallel
ctest --preset macos-arm64-release --output-on-failure
```

Build only the main developer targets:

```sh
cmake --build --preset macos-arm64-release \
  --target rexglue metal_resolve_test metal_pipeline_probe_test unit_tests --parallel
```

The Metal backend deliberately fails during configuration if SPIRV-Cross MSL support is missing.
This prevents a nominally successful build with a nonfunctional shader path.

## Game code generation and build

From the repository root:

```sh
mkdir -p vendor/GoldenEye-Recomp/assets
# Supply vendor/GoldenEye-Recomp/assets/default.xex locally for code generation.

./out/macos-arm64/rexglue codegen \
  vendor/GoldenEye-Recomp/ge_manifest.toml

cmake -S vendor/GoldenEye-Recomp --preset macos-arm64-release
cmake --build vendor/GoldenEye-Recomp/out/build/macos-arm64-release \
  --target ge --parallel
```

The vendor preset points `REXSDK_DIR` back to the repository root, enables Metal, disables Vulkan,
and leaves runtime profiling off. Generated files stay under `vendor/GoldenEye-Recomp/generated/`
and must not be committed.

Run the game against the complete authorized game-data directory, not an XEX-only staging
directory. The runtime root must contain `default.xex`, `files/`, and the title's companion data:

```sh
./vendor/GoldenEye-Recomp/out/build/macos-arm64-release/GoldenEye \
  --game_data_root /absolute/path/to/complete/game-data \
  --gpu metal
```

Missing extracted data can look like a runtime or renderer fault because optional resource loads
resolve to null. Game data stays local and must never be committed.

## Tests

`metal_resolve_test` allocates Metal EDRAM and destination buffers, runs the native resolve-copy
kernel, and compares GPU output against a CPU reference byte for byte.

`metal_pipeline_probe_test` renders through an externally owned Metal vertex buffer, samples a
single-layer `texture2d_array`, expands a four-vertex fan through a six-entry index buffer, checks
scissor delivery, verifies the title's source-alpha blend mode, and validates Xenos-to-Metal
per-channel write-mask mapping. It also verifies ordered draws across queued command buffers, the
64-draw command-buffer boundary, four retained batches, the 256-draw safety drain, explicit waits,
and synchronization during shared/private regional readback, queued clear, resize, and context
release. It protects the resource, fixed-function, and asynchronous-lifetime contracts used by
translated producer draws. It does not replace title-level validation of texture-cache uploads,
resolve coherence, or sustained execution.

Run the Metal test with API validation enabled after changing submission or resource ownership:

```sh
MTL_DEBUG_LAYER=1 ./out/macos-arm64/metal_pipeline_probe_test
```

Unit tests are enabled by the Apple Silicon preset. PPC assembly tests are disabled by default and
require an external `powerpc-none-elf` binutils toolchain. Enable them explicitly with:

```sh
cmake --preset macos-arm64-release -DREXGLUE_BUILD_PPC_TESTS=ON
```

## Runtime and Metal diagnostics

Diagnostics are opt-in so normal runs do not write files or substitute success criteria.

| Environment variable | Effect |
| --- | --- |
| `GOLDENEYE_AUTO_START=1` | Hold Start during the initial input window to skip startup screens |
| `GOLDENEYE_AUTO_START=periodic` | Keep the initial hold, then issue short Start retries separated by release gaps for unattended title/menu runs |
| `GOLDENEYE_METAL_DUMP_SHADERS=1` | Write translated/failed MSL and selected microcode dumps under `/tmp` |
| `GOLDENEYE_METAL_DUMP_FRAMES=1` or `all` | Write all selected BGRA frame stages as PPM files under `/tmp` |
| `GOLDENEYE_METAL_DUMP_FRAMES=N` | Write only presenter frame number `N`; this avoids the decoded-texture inspection used by an all-stage dump |
| `GOLDENEYE_METAL_SUBMISSION_DIAGNOSTICS=1` | Log rate-limited kickoff, flush, and VdSwap metadata without replaying it |
| `GOLDENEYE_METAL_VERBOSE_DIAGNOSTICS=1` | Restore high-volume renderer diagnostics that are suppressed during normal performance runs |
| `GOLDENEYE_METAL_VDSWAP_SCAVENGE=1` | Enable the legacy heuristic VdSwap packet scavenger; this changes execution and is not a strict-path result |
| `GOLDENEYE_METAL_IM_LOAD_DIAGNOSTICS=1` | Inspect words beyond pointer-based shader extents for boundary diagnosis |
| `GOLDENEYE_METAL_PIPELINE_PROBE=1` | Exercise pipeline-probe diagnostics |
| `GOLDENEYE_METAL_HOST_RT_SOLID_TEST=1` | Run the controlled solid render-target test |
| `GOLDENEYE_METAL_MAGENTA_RESOLVE=1` | Run the controlled resolve visibility test |

`GOLDENEYE_AUTO_START=periodic` changes input only: it preserves the initial Start hold, then emits
short retries after full release intervals so unattended runs can cross title gates. It does not
replace rendering or presentation. Pair it with a numeric `GOLDENEYE_METAL_DUMP_FRAMES` value when
collecting one known presenter checkpoint, or use `1`/`all` only when every diagnostic stage is
needed. Keep all resulting game-content captures outside the repository.

Other host-pixel and fallback flags in the code are experiments. Results produced with them must be
labelled as diagnostics and must not be reported as strict-path rendering success.

## Metal submission and synchronization

Persistent producer draws use bounded asynchronous command-buffer submission. A probe context
batches up to 64 draws in each Metal command buffer and retains at most four committed batches,
giving each context a 256-draw safety bound. Normal ordering on a context's Metal command queue
preserves successive draws and clears to the same target without a CPU wait between them.

Synchronization remains mandatory before a result is read or a resource may change underneath
queued GPU work. The runtime therefore drains pending contexts before readback, clear, target
resize or release, resolve/swap, cache clear, and shutdown. Shared-memory uploads, guest CPU/GPU
write commits, texture replacement, and shaders with memory-export side effects also fence through
the command processor. Do not remove one of these boundaries to improve a microbenchmark; validate
ownership first.

Private render-target reads use a reusable staging buffer and copy only the requested rectangle.
The observed title frame's three resolve bands therefore transfer 256, 256, and 208 rows rather
than independently reading the full 720-line target; the sequence's remaining full-frame readback
stays 1280x720. Clear operations needed by the same sequence are queued in order on the context
instead of forcing a separate CPU wait.

Complete, top-origin, full-width resolves can populate an exact resolved-surface cache. A swap may
reuse its host BGRA pixels only when the fetch base, pitch, dimensions, endian mode, and live guest
tiled bytes still match. External guest writes and overlapping resolve writes invalidate the
cache, and the final byte comparison catches writes that bypass normal tracking. Both the mirror
and dirty-range commits use the true tiled-address upper bound rather than a linear
`pitch * height * 4` estimate; the latter is too small for the bottom of a 1280x720 tiled surface.

The title-facing GPU-wait hook determines completion from one atomic primary-ring snapshot. A ring
is drained only when it is configured, has a valid write pointer, and has no pending commands;
this also handles a valid wrapped write pointer of zero without mistaking it for an uninitialized
ring.

Rate-limited `IssueSwap` output includes cumulative asynchronous submission, wait, waited-command,
and maximum-pending counters. Use those counters with a fixed swap checkpoint when comparing
pacing, and leave frame/shader dumps and verbose diagnostics disabled during timing runs.

On the current Apple Silicon test system, a representative fixed early checkpoint improved from
14.43 seconds on the original per-draw-wait path to approximately 10.5 seconds after the bounded
submission and resolve/swap work, roughly 27%. Startup and periodic-input timing varies between
runs, so treat this as directional evidence rather than a stable benchmark. Reaching and animating
the later menu is still far from a playable frame rate.

## Debugging guardrails

- Record the exact binary, build type, environment variables, and command stream being tested.
- Change one provenance assumption at a time.
- Prefer counters and bounded logs over unconditional output.
- Keep dumps outside the repository and never share captures containing proprietary game data.
- Confirm whether a pixel came from the real producer target before debugging the final swap copy.
- Track guest VdSwap calls, CP write-pointer progress, and `IssueSwap` separately.
- Do not advance title-owned fences or ring pointers from a wall-clock timeout.

## Formatting

The repository includes `.clang-format`. Format only touched files where practical:

```sh
clang-format -i path/to/touched_file.cpp path/to/touched_file.h
```
