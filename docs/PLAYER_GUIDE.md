# GoldenEye Metal player guide

## What you need

- An Apple Silicon Mac running macOS 14 or newer
- A compatible game backup that you are legally allowed to use

GoldenEye Metal does not include or download game data.

## Install and play

1. Download the DMG from [GitHub Releases](https://github.com/ysrdevs/goldeneye-metal/releases).
2. Open it and drag **GoldenEye Metal.app** into Applications.
3. Open the app and choose your backup ZIP, Xbox LIVE package, or extracted game folder.
4. After the launcher verifies and imports the files, choose **Play GoldenEye**.

Imported files stay on your Mac. The app does not upload your backup, and you only need to import
it once. To replace it later, use **Choose Backup ZIP or Package…** or **Use Extracted Game
Folder…** in the launcher.

## Keyboard and mouse

| Action | Default |
| --- | --- |
| Move | WASD |
| Look | Mouse |
| Fire / aim | Left click / right click |
| Confirm / back | Space / Shift |
| Start | Return |
| D-pad | Arrow keys |
| Switch original/remastered graphics | F |
| Open host settings | Escape |
| Quit | Command-Q |

Host settings lets you change bindings, mouse sensitivity, menu cursor speed, and vertical-look
inversion. Opening it during an offline mission pauses the game and releases the mouse cursor.

## Controllers

Supported macOS controllers include DualShock 4, DualSense, Xbox One, and Xbox Series X|S pads.
Connect over USB or Bluetooth before or during play. Up to four controller slots are available,
and keyboard/mouse can remain active.

In **Host Settings → Controls**, choose Modern, Classic, or Southpaw, remap buttons, adjust
sensitivity and deadzones, configure rumble, or test the connected controller. A selects, B goes
back, the D-pad or left stick navigates, and LB/RB changes settings tabs. Right Bumper switches the
game between original and remastered graphics.

## Graphics and performance

The Video page provides Performance, Balanced, and Quality presets plus fullscreen, V-Sync,
filtering, anti-aliasing, colour, and output-scaling options. GoldenEye still renders internally at
1280×720; Sharp and MetalFX improve how that image is presented on your display.

The performance overlay can be Off, FPS-only, or Detailed. A 60-second performance report can be
recorded and automatically included with your next diagnostic export.

## Cheats

Open **Host Settings → Testing** and unlock the page to use the 14 supported runtime cheats,
including God Mode, All Weapons, Infinite Ammo, Invisible, Big Heads, Tiny Bond, Paintball, No
Radar, Turbo, Slow Motion, Invulnerable Characters, Stick Insects, Fresco, and Vaseline-o-vision.

Cheats are available only in an active offline one-player mission while settings has paused the
game. Use a separate save if you want clean progression. The game's original Debug Menu remains
available separately.

## Saves and crash recovery

Choose **Manage Saves…** in the launcher to back up, restore, or reset saves. Keep an extra backup
somewhere safe.

After a crash or force-quit, reopen the launcher and choose **Start in Safe Mode** or **Play
Normally**. To report a problem, choose **Export Diagnostic Bundle…** and send the resulting ZIP.
It contains useful logs and crash information, not game data or saves.

## Current limitations

This is an experimental release. The menu, briefing, and Dam gameplay work, but performance is not
locked to 60 FPS everywhere and some graphics issues remain.

For development details, see [DEVELOPMENT.md](DEVELOPMENT.md) and the
[native Metal project status](GOLDENEYE_NATIVE_METAL_PROJECT_STATUS.md).

## Game-data policy

The app contains no original executable, audio, or extracted game assets. Players must provide a
compatible backup they are legally authorized to use.
