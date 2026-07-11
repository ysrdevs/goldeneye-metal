# GoldenEye Metal

GoldenEye Metal is an experimental, source-only static recompilation project focused on a native
Apple Silicon and Metal rendering path. This repository contains the runtime, toolchain, Metal
backend, and game integration in one tree.

> [!WARNING]
> The macOS build is not playable yet. The native title path now runs continuously and presents
> a recognizable animated RARE splash, but it has not reached a correct menu or playable game.

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
draws, preserves the guest alpha test, and uses the correct Metal Y orientation. Those fixes turn
the previous malformed full-screen polygon into a readable title-driven RARE logo through the
normal resolve and presentation path. The current blocker is faithful render-target composition
and fixed-function state; this is the next demonstrated blocker after command and shader delivery.

See [the native Metal status report](docs/GOLDENEYE_NATIVE_METAL_PROJECT_STATUS.md) for the exact
milestones, evidence, and next development priority.

| Platform | Status |
| --- | --- |
| macOS on Apple Silicon | Active development; builds and diagnostics work, game rendering is not playable |
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

These commands build the current prototype; they do not imply playable output. More diagnostics,
test targets, and troubleshooting notes are in [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).

## Game-data policy

This repository does not include the original XEX, generated recompiled C++, audio banks,
captures, or extracted game assets. Do not commit or request downloads for those files. Each user
is responsible for supplying compatible files they are legally authorized to use.

## Contributing

Read [CONTRIBUTING.md](CONTRIBUTING.md) before opening a change. The immediate priority is a
correct title/menu frame with faithful composition and fixed-function state: no synthetic geometry,
replacement shaders, guessed command buffers, or heuristic presentation may count as completion
evidence.

## Licensing and trademarks

This is a multi-license source tree. The runtime, vendored game integration, derived portions, and
third-party dependencies have separate terms. See [LICENSE](LICENSE),
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), and the license files inside vendored and
submodule directories.

This project is unofficial and is not affiliated with or endorsed by any game publisher, console
manufacturer, or trademark owner. Product names are used only to identify compatibility targets.
No trademark or game-content rights are granted by this repository.
