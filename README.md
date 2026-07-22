# GoldenEye Metal

GoldenEye 007 gameplay on macOS, recompiled for Apple Silicon and rendered directly with Apple
Metal.

**[Download v0.2.0 for macOS (.dmg)](https://github.com/ysrdevs/goldeneye-metal/releases/download/v0.2.0/GoldenEye-Metal-0.2.0-macos-arm64.dmg)** ·
[Release notes](https://github.com/ysrdevs/goldeneye-metal/releases/tag/v0.2.0) ·
[Watch gameplay](https://youtu.be/VkbwbXw2tPw) ·
[Discord](https://discord.gg/2AKEFgR7)

[![GoldenEye Metal gameplay on macOS](https://img.youtube.com/vi/VkbwbXw2tPw/maxresdefault.jpg)](https://youtu.be/VkbwbXw2tPw)

> [!WARNING]
> This is an experimental release, not a finished port. The first Dam mission runs and is
> playable, but frame rate and graphics are not yet consistent in every scene.

## Play on macOS

You need:

- an Apple Silicon Mac;
- macOS 14 or newer; and
- a compatible game backup that you are legally authorized to use.

To start playing:

1. [Download the DMG](https://github.com/ysrdevs/goldeneye-metal/releases/download/v0.2.0/GoldenEye-Metal-0.2.0-macos-arm64.dmg).
2. Open it and drag **GoldenEye Metal.app** into Applications.
3. Launch the app. On first use, select your compatible local game backup and wait for the
   one-time import.
4. Choose **Play GoldenEye**. Future launches open the same launcher with your private local copy
   ready.

The first-run launcher accepts:

- the compatible original game-backup ZIP;
- the Xbox LIVE/STFS package stored inside that backup; or
- an extracted folder containing `default.xex`, `files/`, `music.xwb`, and `sfx.xwb`.

The launcher verifies the supported game revision before importing it. Your files stay on your
Mac: the app does not include, download, or upload game data. You can choose a different source
from the launcher later.

If a game session does not close cleanly, the launcher offers **Start in Safe Mode** for one run or
**Play Normally**. You can also choose **Export Diagnostic Bundle…** and send the resulting ZIP
with a report; it excludes game data, saves, cache, and settings.

Choose **Manage Saves…** to create a portable `.gesave` backup, restore a backup, or reset local
progress. Restore and reset preserve the previous data so the action can be undone immediately.

The release is Developer ID signed, Apple-notarized, and stapled. See the
[player guide](docs/PLAYER_GUIDE.md) for detailed controls, controller setup, import behavior, and
troubleshooting.

## Controls

| Action | Keyboard and mouse |
| --- | --- |
| Move | WASD |
| Look | Mouse |
| Fire | Left click |
| Aim | Right click |
| A / confirm | Space |
| B / back | Shift |
| Start | Return |
| D-pad | Arrow keys |
| Original / remastered graphics | F |
| Host settings / release cursor | Escape |
| Quit | Command-Q |

Modern controllers work over USB or Bluetooth, including DualShock 4, DualSense, Xbox One, and
Xbox Series X|S controllers. Controllers can be connected or removed while the game is running,
and keyboard/mouse can remain active at the same time. The Controls page includes Modern,
Classic, and Southpaw layouts plus per-button remapping.

## What this project is

GoldenEye Metal is not a traditional full-system emulator. The game code is recompiled ahead of
time for ARM64, while a compatibility runtime provides the Xbox 360 APIs and GPU behavior the game
expects.

The graphics path consumes the game's real command stream and shaders, translates the shaders,
and renders the result directly through Metal:

```text
game commands -> Xenos shader translation -> Metal rendering -> game resolve -> presentation
```

There is no Vulkan or MoltenVK graphics path in the macOS release. The repository contains the
runtime, recompilation toolchain, Metal backend, native launcher, and game integration source.

## Current state

Working today:

- the classification, gun-barrel, RARE, menu, briefing, and first Dam gameplay sequences;
- native Metal presentation on Apple Silicon;
- native keyboard/mouse input, modern gamepads, layout presets, and button remapping;
- a local game-data importer, crash-aware Safe Mode, save management, true local-mission pause,
  and diagnostic export;
- live performance presets, MetalFX/Sharp output scaling, filtering, FXAA and colour controls;
- optional FPS or detailed performance overlays and a diagnostics-ready 60-second report;
- a guarded Testing page with all 14 verified retail runtime cheats and graphics-mode switching;
- clean macOS window/menu quitting.

Metal renders the game internally at its native **1280x720**. Bilinear, Sharp, and MetalFX Spatial
can scale that image to the window or Retina display, but they do not increase the game's internal
rendering resolution.

Correct Dam captures have shown **46.5–60 FPS**, but this is not a claim of locked 60 FPS. The main
work now is consistent performance across broader scenes, lower resolve/fence cost, physical
controller acceptance testing, and improved depth/stencil and MSAA fidelity.

For implementation details, evidence, milestones, known gaps, and the exact next priorities, read
the [native Metal technical status](docs/GOLDENEYE_NATIVE_METAL_PROJECT_STATUS.md).

| Platform | Status |
| --- | --- |
| Apple Silicon macOS | Active development; v0.2.0 reaches first-mission gameplay |
| Windows and Linux | Backend code exists, but this project's current changes are not verified there |

## Build and contribute

Source builds require an Apple Silicon Mac, current Xcode Command Line Tools, CMake, Git, and a
compatible local `default.xex` that you are authorized to use. Game data and generated game code
are intentionally excluded from Git.

- [Development guide](docs/DEVELOPMENT.md) — configure, build, code generation, tests, manual runs,
  input diagnostics, and Metal profiling
- [macOS distribution](docs/MACOS_DISTRIBUTION.md) — app packaging, Developer ID signing,
  notarization, ZIP, and DMG creation
- [Contributing guide](CONTRIBUTING.md) — development expectations and pull requests
- [Technical status](docs/GOLDENEYE_NATIVE_METAL_PROJECT_STATUS.md) — architecture, proof,
  milestones, blockers, and roadmap

Developers with a completed local source build can also double-click
[Launch GoldenEye.command](<Launch GoldenEye.command>) to run the build-tree version with Metal and
native input selected automatically.

## Game data, licensing, and trademarks

This repository does not include the original XEX, generated recompiled C++, audio banks,
captures, or extracted game assets. Do not commit or request downloads for those files. Each user
is responsible for supplying compatible files they are legally authorized to use.

This is a multi-license source tree. See [LICENSE](LICENSE),
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), and the license files inside vendored and
submodule directories.

This project is unofficial and is not affiliated with or endorsed by any game publisher, console
manufacturer, or trademark owner. Product names are used only to identify compatibility targets.
No trademark or game-content rights are granted by this repository.
