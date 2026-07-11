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
per-channel write-mask mapping. It protects the resource and fixed-function contract used by
translated producer draws. It does not replace title-level validation of texture-cache uploads or
resolve coherence.

Unit tests are enabled by the Apple Silicon preset. PPC assembly tests are disabled by default and
require an external `powerpc-none-elf` binutils toolchain. Enable them explicitly with:

```sh
cmake --preset macos-arm64-release -DREXGLUE_BUILD_PPC_TESTS=ON
```

## Metal diagnostics

Diagnostics are opt-in so normal runs do not write files or substitute success criteria.

| Environment variable | Effect |
| --- | --- |
| `GOLDENEYE_METAL_DUMP_SHADERS=1` | Write translated/failed MSL and selected microcode dumps under `/tmp` |
| `GOLDENEYE_METAL_DUMP_FRAMES=1` | Write selected BGRA frame stages as PPM files under `/tmp` |
| `GOLDENEYE_METAL_SUBMISSION_DIAGNOSTICS=1` | Log rate-limited kickoff, flush, and VdSwap metadata without replaying it |
| `GOLDENEYE_METAL_VDSWAP_SCAVENGE=1` | Enable the legacy heuristic VdSwap packet scavenger; this changes execution and is not a strict-path result |
| `GOLDENEYE_METAL_IM_LOAD_DIAGNOSTICS=1` | Inspect words beyond pointer-based shader extents for boundary diagnosis |
| `GOLDENEYE_METAL_PIPELINE_PROBE=1` | Exercise pipeline-probe diagnostics |
| `GOLDENEYE_METAL_HOST_RT_SOLID_TEST=1` | Run the controlled solid render-target test |
| `GOLDENEYE_METAL_MAGENTA_RESOLVE=1` | Run the controlled resolve visibility test |

Other host-pixel and fallback flags in the code are experiments. Results produced with them must be
labelled as diagnostics and must not be reported as strict-path rendering success.

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
