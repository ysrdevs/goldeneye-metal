# GoldenEye Metal project status

Last updated: July 22, 2026

GoldenEye Metal runs the recompiled game code on Apple Silicon and translates the original Xenos
GPU work directly to Metal. It does not use Vulkan or MoltenVK, and it is not a traditional
full-system emulator. The compatibility runtime supplies the Xbox 360 services the game expects.

No game files are included. Players must provide a compatible backup they are legally allowed to
use.

## Where the project is now

The game boots through the classification screen, gun-barrel sequence, RARE logo, main menu, Dam
briefing, and playable Dam mission. Those scenes are produced from the game's real command stream,
shaders, textures, geometry, render targets, resolves, and swaps.

Working today:

- Native Apple Silicon and Metal rendering at the game's internal 1280×720 resolution
- Playable keyboard and mouse input
- DualShock 4, DualSense, Xbox One, and Xbox Series controller support
- Original/remastered graphics switching
- A native launcher that imports and verifies a local backup
- Save backup, restore, reset, and crash recovery
- Safe Mode and one-click diagnostic export
- Fullscreen, V-Sync, scaling, filtering, anti-aliasing, colour controls, and performance overlays
- Proper gameplay pause while host settings is open
- Modern, Classic, and Southpaw controller layouts with button remapping
- A guarded Testing page containing all 14 verified runtime cheats
- Signed and notarized release packaging for macOS 14 or newer

This is still an experimental project, not a finished port. Dam is the main tested gameplay area;
later levels and every multiplayer path have not yet received the same validation.

## Native Metal path

The production rendering path is:

```text
game command stream
  → Xenos shader translation
  → Metal draws and render targets
  → guest-visible resolve
  → normal game swap and presentation
```

Early diagnostic rendering and command-recovery shortcuts were useful during bring-up, but they
are disabled for the current results. The visible menu and gameplay do not come from replacement
shaders, synthetic geometry, forced frames, Vulkan translation, or stale captures.

Major problems already solved include incorrect guest-memory aliasing, discarded processed index
buffers, incomplete viewport/blend state, excessive synchronous Metal waits, full-frame CPU
readback during presentation, false GPU-hang detection, unsafe texture descriptors, and two Dam
cleanup crashes reported by early testers.

## Performance

Performance varies by scene and Mac.

A repeatable Dam run on an M3 Ultra discarded eight warm-up windows and measured the next 48. It
averaged **59.909 FPS**, with measured windows ranging from **57.357 to 60.148 FPS**. That run used
the real Metal resolve and swap path with no full-frame CPU presentation uploads or resolve
fallbacks.

This is evidence that the project can approach 60 FPS, not a claim that the whole game is locked
to 60. Less powerful Macs, including the base M5 MacBook Air, currently perform worse in some
views. Reducing synchronization and scene-specific stalls remains a priority.

## Main limitations

- Frame pacing and performance are not consistent across all views or Macs.
- Depth-only rendering, shared guest depth/stencil behavior, and faithful Xbox 360 MSAA remain
  incomplete.
- Some broader scenes may still show missing, flickering, or incorrect graphics.
- Physical USB and Bluetooth testing is not complete across every supported controller family.
- Stability beyond Dam and across full campaign progression still needs wider testing.
- Local split-screen exists, but reliable 2–4 player validation is still ongoing.

## Next priorities

1. Profile repeatable slow scenes on both high-end and lower-power Macs.
2. Reduce the remaining GPU synchronization cost without changing rendered results.
3. Complete physical keyboard, mouse, Sony, and Xbox controller testing.
4. Finish shared depth/stencil, depth-only draws, and faithful MSAA behavior.
5. Validate later missions and local split-screen from start to finish.

## Milestones

| Milestone | Status |
| --- | --- |
| Native Metal window and presentation | Complete |
| Real game shaders and command stream | Complete for tested scenes |
| Recognizable main menu | Complete |
| Playable Dam mission | Complete, with ongoing fidelity and stability work |
| Native launcher and local game-data import | Complete |
| Keyboard, mouse, and modern controllers | Working; physical test coverage ongoing |
| Full-game fidelity and stable 60 FPS | Not complete |

Build instructions, test coverage, profiling commands, and implementation notes live in
[DEVELOPMENT.md](DEVELOPMENT.md). Player instructions are in [PLAYER_GUIDE.md](PLAYER_GUIDE.md).
