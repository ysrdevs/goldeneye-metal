# GoldenEye Metal

GoldenEye Metal is an experimental, source-only static recompilation project focused on a native
Apple Silicon and Metal rendering path. This repository contains the runtime, toolchain, Metal
backend, and game integration in one tree.

> [!WARNING]
> The macOS build is not playable yet. The native title path now runs continuously and presents
> a recognizable main menu through the real Metal draw, resolve, and swap path, but interactive
> gameplay and complete fixed-function fidelity have not been verified. Startup is measurably
> faster than the original synchronous path, but later title and menu pacing remains far below a
> playable frame rate.

## Project status

The intended graphics path is:

```text
PM4 command stream -> Xenos shader translation -> Metal draws -> Metal RT/EDRAM
                   -> guest resolve -> IssueSwap -> Metal presentation
```

Implemented foundations include the macOS window and presenter, shared guest memory, texture
decode and upload, Xenos-to-SPIR-V-to-MSL translation, private Metal render targets, resolve-copy,
and swap presentation. The title's real primary ring, indirect buffers, shaders, resolves, and
`XE_SWAP` now run continuously through Metal without command scavenging or replay. Authoritative
vertex-buffer delivery, translated array-texture bindings, and CPU/Metal resolve coherence are in
place. Metal now consumes guest DMA, converted, and built-in index buffers through real indexed
draws, preserves the guest alpha test, and applies live viewport, scissor, blend, blend-constant,
and per-channel color-write state. Current single-sample host contexts assemble the observed title
sequence's three resolve bands described by its 4xMSAA layout through the normal `IssueCopy` and
`IssueSwap` route. Ordinary persistent Metal draws are now submitted asynchronously in bounded
queues instead of blocking the CPU after every draw: each command buffer carries up to 64 draws,
with at most four retained batches and a 256-draw safety drain. Regional private-texture readback,
ordered queued clears, and an exact resolved-surface swap cache reduce avoidable synchronization
and transfer work while preserving guest-memory validation and the true tiled surface extent.
Explicit fences still protect readback, resolves, shared-resource mutation, swaps, and teardown.
Those fixes produce a clean classification screen, the complete gun-barrel sequence, a shaded
animated gold RARE logo, and a recognizable dossier-style main menu. A representative fixed early
checkpoint improved from 14.43 seconds to approximately 10.5 seconds, about 27%, although runtime
timings vary and the later menu remains far from playable. The next known gaps are culling,
depth/stencil, true guest MSAA behavior, native macOS input, and sustained pacing on the route to
first playable output.

See [the native Metal status report](docs/GOLDENEYE_NATIVE_METAL_PROJECT_STATUS.md) for the exact
milestones, evidence, and next development priority.

| Platform | Status |
| --- | --- |
| macOS on Apple Silicon | Active development; strict Metal rendering reaches the main menu, but gameplay is not verified |
| Windows | Existing backend code is present; this repository's current changes are not verified there |
| Linux | Existing backend code is present; this repository's current changes are not verified there |

## Repository layout

```text
src/graphics/metal/             Native Metal command processor and rendering support
src/ui/metal/                   Metal presenter and macOS UI integration
vendor/GoldenEye-Recomp/        Game integration source, vendored directly in this repository
docs/                           Current status and development notes
thirdparty/                     Pinned source dependencies managed as Git submodules
```

`vendor/GoldenEye-Recomp` is an ordinary source directory, not another repository or submodule.
Generated recompilation output and game data remain local and are ignored by Git.

## Build on Apple Silicon

### Requirements

- Apple Silicon Mac and a current macOS SDK
- Xcode Command Line Tools with AppleClang 18 or newer
- CMake 3.25 or newer
- SPIRV-Cross with MSL support
- Git
- A compatible Xbox 360 executable and game files that you are authorized to use

Install the host build tools with Homebrew:

```sh
brew install cmake spirv-cross
```

Clone and initialize the source dependencies:

```sh
git clone --recurse-submodules https://github.com/ysrdevs/goldeneye-metal.git
cd goldeneye-metal
git submodule update --init --recursive
```

Configure and build the toolchain and Metal isolation test:

```sh
cmake --preset macos-arm64-release
cmake --build --preset macos-arm64-release \
  --target rexglue metal_resolve_test metal_pipeline_probe_test --parallel
ctest --preset macos-arm64-release --output-on-failure
```

### Generate and build the game target

Create `vendor/GoldenEye-Recomp/assets/`, place an authorized compatible `default.xex` there for
code generation, then run:

```sh
./out/macos-arm64/rexglue codegen \
  vendor/GoldenEye-Recomp/ge_manifest.toml

cmake -S vendor/GoldenEye-Recomp --preset macos-arm64-release
cmake --build vendor/GoldenEye-Recomp/out/build/macos-arm64-release \
  --target ge --parallel
```

The game executable is written to its vendor build directory. Run it from the repository root
against your complete authorized game-data directory, which must include `default.xex`, `files/`,
and the companion title data:

```sh
./vendor/GoldenEye-Recomp/out/build/macos-arm64-release/GoldenEye \
  --game_data_root /absolute/path/to/complete/game-data \
  --gpu metal
```

The macOS window backend does not yet forward keyboard events to the guest. Add
`--input_backend sdl` when testing with a compatible controller; use the opt-in unattended input
diagnostic documented in [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) for repeatable menu captures.

These commands build the current menu-reaching prototype; they do not imply playable output. More
diagnostics, test targets, and troubleshooting notes are in
[docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).

## Game-data policy

This repository does not include the original XEX, generated recompiled C++, audio banks,
captures, or extracted game assets. Do not commit or request downloads for those files. Each user
is responsible for supplying compatible files they are legally authorized to use.

## Contributing

Read [CONTRIBUTING.md](CONTRIBUTING.md) before opening a change. The immediate priority is stable
interactive menu navigation and first gameplay with faithful culling, depth/stencil, and MSAA
state: no synthetic geometry, replacement shaders, guessed command buffers, or heuristic
presentation may count as completion evidence.

## Licensing and trademarks

This is a multi-license source tree. The runtime, vendored game integration, derived portions, and
third-party dependencies have separate terms. See [LICENSE](LICENSE),
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), and the license files inside vendored and
submodule directories.

This project is unofficial and is not affiliated with or endorsed by any game publisher, console
manufacturer, or trademark owner. Product names are used only to identify compatibility targets.
No trademark or game-content rights are granted by this repository.
