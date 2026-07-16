# GoldenEye Metal

GoldenEye Metal is an experimental static recompilation project focused on a native Apple Silicon
and Metal rendering path. This repository contains the runtime, toolchain, Metal backend, native
macOS launcher, and game integration in one tree.

> [!WARNING]
> The macOS build is not fully playable yet. The strict native path now reaches a clean Dam
> briefing and fully rendered first-mission gameplay through real Metal draws, resolves, and
> swaps. Native keyboard/mouse and SDL gamepad input now have controller-state regression
> coverage and a double-click launcher, but physical Dam gameplay still needs final live
> validation. Correct dynamic Dam captures have now displayed
> 46.5 and 60.0 FPS, but not every view sustains 60 FPS; broader-scene pacing plus important
> depth/MSAA fidelity gaps remain.

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
with at most four retained batches per context. Completed upload command buffers retire from the
oldest forward, and the global retained-draw ceiling is 2048. Regional private-texture readback,
ordered queued clears, and an exact resolved-surface swap cache reduce avoidable synchronization
and transfer work while preserving guest-memory validation and the true tiled surface extent.
Texture-source fingerprints avoid redundant decode and upload work after coarse guest-memory
invalidations, immutable MSL reflection removes repeated shader-source scans from draw delivery,
and reusable Metal upload arenas replace per-draw constant and vertex-buffer allocation. Explicit
fences still protect readback, resolves, shared-resource mutation, swaps, and teardown. The macOS
clock now reports the same nanosecond unit used by its tick counter, so guest time and the native
FPS overlay are no longer scaled by the timer's hardware resolution. Those fixes produce a clean
classification screen, the complete gun-barrel sequence, a shaded animated gold RARE logo, and a
recognizable dossier-style main menu. On an Apple M3 Ultra, corrected-clock 1280x720 menu captures
reported 30.0 and 59.9 guest-delivered frames per second in different short intervals. Native
Cocoa key and mouse events now feed the common keyboard/mouse controller driver, including hidden
relative mouse capture, and Metal applies guest front/back culling and front-face winding for
polygon draws. Persistent host contexts also carry private depth/stencil attachments and map guest
Z compare/write plus front/back stencil state. A deterministic input-only route now combines
`GOLDENEYE_AUTO_START=menu` with `GOLDENEYE_AUTO_MISSION=dam`: five ordinary A press/release
pulses traverse the default dossier choices, with a 450 ms left-stick-up contribution before the
final A. It reaches a clean real Dam briefing near 60 FPS and then the fully rendered first Dam
scene. A historical proving run sustained 29.68 FPS; retiring completed upload command buffers and
raising the global draw ceiling then produced a correct 46.5 FPS capture while reducing draw time
from roughly 10 ms to 7.2–7.5 ms per swap. A later correct Dam capture displayed 60.0 FPS, with
stable complete profiler windows reporting roughly 6.4–6.7 ms draw, 4.2–4.35 ms copy, 1.74 ms
swap, and 2.9 ms `WAIT_REG_MEM` time per swap. These results establish real progress beyond the
former 30 FPS ceiling, not sustained 60 FPS across every Dam view.

The gameplay transition exposed an invalid 8192x8191 tiled texture descriptor whose source span
extended beyond guest physical memory. The direct diagnostic decoder had bypassed the texture
cache's range rejection and attempted to untile it. Texture decode now validates all layout
arithmetic and source/output bounds before translating a guest pointer or allocating output, and a
standalone regression test preserves the failing descriptor. The current priorities are
stabilizing high frame rates across broader Dam scenes, reducing resolve and fence-wait costs,
then validating physical native input in gameplay.
Depth-only draw routing, guest-addressed shared depth/stencil ownership, and faithful guest MSAA
remain open.

The title's GPU-wait hook also rejects transient drained-ring snapshots before exposing accumulated
guest watchdog time. A 74-second proving run reached normal `IssueSwap` 4416 with repeated dossier
frames and no false D3D GPU-hang dumps or debug traps; the prior control emitted 14 of each. This
guard changes no guest fence, ring pointer, presented counter, or render command. A pending ring
must keep advancing RPTR to retain the host-time hold; 30 seconds without RPTR progress restores
the title's genuine hang detection.

The presenter FPS overlay is enabled by default and measures completed guest front-buffer
deliveries, not host window repaints. Set `REX_METAL_SHOW_FPS=false` to hide it. The validated GPU
tiled-resolve path is enabled by default; set `GOLDENEYE_METAL_GPU_TILED_RESOLVE=0` to compare the
CPU fallback. It has eight byte-exact regression cases and has completed thousands of live Dam
resolves with zero fallbacks. Set `GOLDENEYE_METAL_PROFILE=1` for low-overhead command,
`WAIT_REG_MEM`, wait-reason, texture-fallback, and presenter ledgers every 64 swaps or presentation
attempts.

See [the native Metal status report](docs/GOLDENEYE_NATIVE_METAL_PROJECT_STATUS.md) for the exact
milestones, evidence, and next development priority.

| Platform | Status |
| --- | --- |
| macOS on Apple Silicon | Active development; strict Metal rendering reaches correct dynamic Dam gameplay with 46.5 and 60.0 FPS captures, while broader-scene stability and physical-input validation remain |
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

## Play on macOS

A packaged release is a normal **GoldenEye Metal.app** for Apple Silicon and macOS 14 or newer.
Drag it to Applications and open it from Finder. On the first launch, the app asks for one of these
local inputs:

- your compatible original game-backup ZIP;
- the Xbox LIVE/STFS package stored inside that backup; or
- an already extracted game-data folder containing `default.xex`, `files/`, `music.xwb`, and
  `sfx.xwb`.

The launcher verifies the exact supported game revision before parsing or importing it. ZIPs are
handled locally: only the package inside the selected ZIP is read, the validated game data is
installed under `~/Library/Application Support/GoldenEye Metal/Game Data`, and the temporary
package is deleted. Later launches start directly from that private cache. The app does not
contain, download, or link to game content, and it never uploads the selected backup.
Hold Option while opening the app to show the chooser again and select a different local source.
Import can be cancelled without replacing an existing working cache. The app records the expected
file count and byte total, checks critical resources and the executable identity on later launches,
and automatically rebuilds a damaged cache when the verified package is selected again.

Native Metal, SDL gamepads, and keyboard/mouse control are selected automatically. The current
build is still an active-development prototype; see the warning at the top of this README before
treating it as a finished gameplay release.

WASD moves, Space is A, Shift is B, Return is Start, and Escape opens the host settings menu.
Mouse movement looks, left click fires, and right click aims. The settings menu releases the
cursor; its Controls page changes the bindings and mouse sensitivity consumed by the macOS input
driver.

### Controllers

The native SDL gamepad path is enabled by default and supports the standard mappings for:

- PlayStation 4 DualShock 4
- PlayStation 5 DualSense
- Xbox One controllers
- Xbox Series X|S controllers

Pair a wireless controller in macOS Bluetooth settings or connect it over USB, then start the
game. Controllers can be connected or removed while the game is running. The first connected pad
becomes player 1; the runtime maintains up to four controller slots and promotes a waiting pad if
a slot becomes free. Keyboard and mouse remain usable at the same time.

Face buttons follow their physical positions: Cross/A is guest A, Circle/B is guest B, Square/X
is guest X, and Triangle/Y is guest Y. Options/Menu is Start, and the pad's normal select button
(Share, Create, or View) is Back; extra capture and microphone buttons do not affect gameplay. The
D-pad, both sticks, stick clicks, bumpers, and triggers use their standard SDL mappings. Rumble is
forwarded when the controller and its macOS connection expose it. Automated tests cover normalized
button/axis input, hot-plugging, slot promotion, pause suppression, rumble calls, and simultaneous
keyboard/controller keystrokes. The USB and Bluetooth hardware matrix still needs physical
acceptance testing across each controller family.

### Developer launcher

After building the game from source, developers can also double-click
[Launch GoldenEye.command](<Launch GoldenEye.command>) at the repository root. This script finds
an existing extracted data folder, supplies the local build-tree library path, selects Metal and
native input, and clears unattended diagnostics. It remains a development convenience; the
packaged `.app` above is the release experience for nontechnical players.

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
  --target rexglue metal_resolve_test metal_pipeline_probe_test \
  metal_texture_decode_validation_test --parallel
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

Build and verify the unsigned native application bundle with:

```sh
cmake --build vendor/GoldenEye-Recomp/out/build/macos-arm64-release \
  --target goldeneye_macos_app_verify --parallel
```

The result is
`vendor/GoldenEye-Recomp/out/build/macos-arm64-release/dist/GoldenEye Metal.app`. It contains no
game data. Release-owner commands for Developer ID signing, DMG creation, notarization, stapling,
and final Gatekeeper checks are documented in
[macOS application distribution](docs/MACOS_DISTRIBUTION.md).

The game executable is written to its vendor build directory. Run it from the repository root
against your complete authorized game-data directory, which must include `default.xex`, `files/`,
and the companion title data:

```sh
REX_INPUT_BACKEND=sdl REX_MNK_MODE=true \
./vendor/GoldenEye-Recomp/out/build/macos-arm64-release/GoldenEye \
  --game_data_root /absolute/path/to/complete/game-data \
  --gpu metal
```

Keyboard/mouse controller emulation is opt-in for manual launches; both macOS launchers enable it
automatically. Space is A, Shift is B, WASD is the left stick,
the arrow keys are the D-pad, the mouse is the right stick, and its left/right buttons are the
right/left triggers. Start defaults to Return on macOS because Escape opens the host pause overlay.
The SDL controller backend is now the default, and keyboard/mouse input may remain enabled
alongside it. The unattended input diagnostic in
[docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) remains useful for repeatable rendering captures.

These commands build the current first-gameplay-reaching prototype; they do not imply fully
playable output. More diagnostics, test targets, and troubleshooting notes are in
[docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).

## Game-data policy

This repository does not include the original XEX, generated recompiled C++, audio banks,
captures, or extracted game assets. Do not commit or request downloads for those files. Each user
is responsible for supplying compatible files they are legally authorized to use.

## Contributing

Read [CONTRIBUTING.md](CONTRIBUTING.md) before opening a change. The immediate priority is making
high frame rates repeatable across broader Dam views by reducing resolve and fence-wait costs,
followed by physical native-input validation and the remaining depth/stencil and MSAA fidelity
work. Synthetic geometry, replacement
shaders, guessed command buffers, or heuristic presentation may not count as completion evidence.

## Licensing and trademarks

This is a multi-license source tree. The runtime, vendored game integration, derived portions, and
third-party dependencies have separate terms. See [LICENSE](LICENSE),
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), and the license files inside vendored and
submodule directories.

This project is unofficial and is not affiliated with or endorsed by any game publisher, console
manufacturer, or trademark owner. Product names are used only to identify compatibility targets.
No trademark or game-content rights are granted by this repository.
