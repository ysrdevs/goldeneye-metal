# GoldenEye game integration

This directory contains the source-side game integration for GoldenEye Metal. It is vendored
directly into the main repository and is not a separate repository or Git submodule.

> [!WARNING]
> The native Apple Silicon/Metal path is an active research prototype. It builds and passes
> controlled GPU diagnostics. Real title PM4 now reaches Metal draws, resolve, and swap without
> heuristic replay, but the producer render target remains black and the build is not playable.

## What is included

- Build configuration and recompilation manifest
- Runtime hooks and host application glue
- Input, menu, post-processing, and networking integration
- Licensed Community Edition patch data and logic

The original XEX, generated recompiled C++, audio banks, captures, and extracted game assets are
not included. Supply only files that you are authorized to use. Do not request or provide download
links for game data in project spaces.

## Build on Apple Silicon

First build the root toolchain as described in the main [README](../../README.md). From the
repository root, place your compatible executable at `assets/default.xex` within this directory for
code generation, then run:

```sh
./out/macos-arm64/rexglue codegen \
  vendor/GoldenEye-Recomp/ge_manifest.toml

cmake -S vendor/GoldenEye-Recomp --preset macos-arm64-release
cmake --build vendor/GoldenEye-Recomp/out/build/macos-arm64-release \
  --target ge --parallel
```

Run the resulting prototype from the repository root against the complete authorized game-data
directory, including `default.xex`, `files/`, and the companion title data:

```sh
./vendor/GoldenEye-Recomp/out/build/macos-arm64-release/GoldenEye \
  --game_data_root /absolute/path/to/complete/game-data \
  --gpu metal
```

Build products go under `out/`; generated game code stays under `generated/`. Both are ignored.

## Licensing

Original files in this directory are released under [the Unlicense](LICENSE). Derived portions are
covered by the notices in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and the repository-root
[third-party notices](../../THIRD_PARTY_NOTICES.md). No game-content or trademark rights are
granted.
