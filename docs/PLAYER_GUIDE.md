# GoldenEye Metal player guide

This guide covers the public macOS application. For source builds and developer diagnostics, see
[DEVELOPMENT.md](DEVELOPMENT.md).

## Requirements

- Apple Silicon Mac
- macOS 14 or newer
- A compatible game backup that you are legally authorized to use

GoldenEye Metal does not contain or download game data.

## Install

1. Download the current DMG from the
   [GitHub release page](https://github.com/ysrdevs/goldeneye-metal/releases/tag/v0.1.1).
2. Open the DMG.
3. Drag **GoldenEye Metal.app** into Applications.
4. Open the app from Applications.

The release is Developer ID signed, Apple-notarized, and stapled.

## First launch and game-data import

The launcher accepts any one of these local inputs:

- the compatible original game-backup ZIP;
- the Xbox LIVE/STFS package stored inside that backup; or
- an extracted game-data folder containing `default.xex`, `files/`, `music.xwb`, and `sfx.xwb`.

The app verifies the exact supported game revision before parsing or importing it. ZIPs are
handled locally, and only the package inside the selected ZIP is read. Validated game data is
installed under:

```text
~/Library/Application Support/GoldenEye Metal/Game Data
```

The temporary package is deleted after import. Later launches use the private cached copy, so the
original ZIP or package does not need to remain mounted or selected.

The app never uploads the selected backup. It records the expected file count and size, checks
critical resources and executable identity on later launches, and asks to rebuild a damaged or
incomplete cache from a verified source.

Hold **Option** while opening the app to show the source chooser again. Cancelling a new import
does not replace an existing working cache.

## Keyboard and mouse

Native keyboard/mouse input is enabled automatically by the macOS app.

| Action | Default input |
| --- | --- |
| Move | WASD |
| Look | Mouse movement |
| Fire | Left click |
| Aim | Right click |
| A / confirm | Space |
| B / back | Shift |
| Start | Return |
| D-pad | Arrow keys |
| Host settings / release cursor | Escape |

Mouse capture is relative and hidden during play. Opening the host settings menu with Escape
releases the cursor. The Controls page changes keyboard bindings and mouse sensitivity.

Command-Q, Command-W, the application menu's Quit command, the Dock's Quit command, and the window
close button all close the game without requiring Force Quit.

## Controllers

The native SDL gamepad path supports the standard macOS mappings for:

- PlayStation 4 DualShock 4
- PlayStation 5 DualSense
- Xbox One controllers
- Xbox Series X|S controllers

Pair a wireless controller in macOS Bluetooth settings or connect it over USB, then start the
game. Controllers can also be connected or removed while the game is running. The first connected
pad becomes player 1; up to four guest controller slots are available. Keyboard and mouse can
remain active alongside a controller.

Face buttons follow their physical positions: Cross/A is guest A, Circle/B is guest B, Square/X
is guest X, and Triangle/Y is guest Y. Options/Menu is Start. Share, Create, or View is Back. The
D-pad, sticks, stick clicks, bumpers, and triggers use their normal mappings. Rumble is forwarded
when the controller and its macOS connection expose it.

Automated coverage includes normalized input, hot-plugging, slot promotion, pause suppression,
rumble, and simultaneous keyboard/controller input. Physical USB and Bluetooth acceptance testing
across every controller family is still in progress.

## Current limitations

Version 0.1.1 is an experimental development release. It reaches the menu, Dam briefing, and first
mission gameplay. Correct captures have shown 46.5–60 FPS, but performance is not locked to 60 FPS
in every view. Some depth, stencil, multisampling, and broader-scene graphics behavior still needs
work.

See the [native Metal technical status](GOLDENEYE_NATIVE_METAL_PROJECT_STATUS.md) for current
evidence, limitations, and development priorities.

## Game-data policy

The application contains no original executable, audio banks, or extracted game assets. It does
not provide links to game-content downloads. Each user is responsible for supplying a compatible
backup they are legally authorized to use.
