# Contributing to GoldenEye Metal

Thank you for helping with the project. The current engineering priority is the first recognizable
title-driven frame through the native Metal path, followed by a stable menu and playable runtime.

## Before changing code

1. Read [the current Metal status](docs/GOLDENEYE_NATIVE_METAL_PROJECT_STATUS.md).
2. Build the Apple Silicon preset and run the available tests.
3. Keep game data, generated code, captures, and local debug output outside Git.
4. Preserve all license and third-party attribution notices.

## Development expectations

- Keep the default path faithful to the real guest command stream, shaders, resources, and
  resolves.
- Put probes, fallbacks, synthetic draws, and data dumps behind explicit diagnostic flags.
- Do not treat diagnostic color fills, host-generated geometry, or heuristic presentation as game
  rendering success.
- Update the status document when evidence changes a milestone or invalidates a previous theory.
- Avoid unrelated formatting or mechanical rewrites in focused changes.

## Build and test

On Apple Silicon:

```sh
cmake --preset macos-arm64-release
cmake --build --preset macos-arm64-release --parallel
ctest --preset macos-arm64-release --output-on-failure
```

PPC assembly tests are optional because they require an externally installed
`powerpc-none-elf` cross-binutils toolchain:

```sh
cmake --preset macos-arm64-release -DREXGLUE_BUILD_PPC_TESTS=ON
```

Format touched C and C++ sources with the repository's `.clang-format` configuration.

## Pull requests

Describe the problem, the approach, and the evidence. For Metal changes, include the exact command
path exercised, relevant diagnostic flags, and whether the output came from real title data or a
controlled test. Never attach proprietary assets or captures containing game-derived data.

By contributing, you agree that your change may be distributed under the license applicable to
the files you modify.
