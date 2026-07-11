# GoldenEye native Metal project status

Last updated: 2026-07-10

## Goal

Build a real native Apple Silicon rendering path that reaches a recognizable menu frame and then a
playable game without depending on Vulkan translation. The required path is:

```text
real PM4 packets
  -> real Xenos vertex and pixel microcode
  -> faithful resource and constant bindings
  -> Metal draw into a real render target / EDRAM representation
  -> guest-visible resolve
  -> real IssueSwap presentation
```

The first target is a recognizable title-driven menu frame. The long-term target is stable,
playable output.

## Strict definition of success

A result counts only when all of these are true:

- The command stream comes from the title's real kickoff and indirect buffers.
- The translated shaders come from the title's real microcode.
- Vertex fetches, textures, constants, and render state come from the real draw.
- The producer render target contains nonzero title output.
- The normal resolve writes the expected guest-visible buffer.
- The normal swap path presents that buffer.

Synthetic geometry, replacement shaders, diagnostic fills, guessed command ranges, cached old
textures, and heuristic presentation are useful probes but do not satisfy the milestone.

## Current result

The command-provenance blocker is resolved. Apple Silicon uses 16 KiB mapping granularity, while
the guest `0xE0000000` physical-heap view begins at backing-store offset `0x1000`. Generated title
memory accesses omitted that host adjustment, so writes to the virtual command ring at
`0xFFC9C000` landed one page before the command processor's physical `0x1FC9D000` view.

The centralized physical-host-offset fix makes the physical, A, C, and E aliases agree. In a
strict run with the heuristic scavenger and replay disabled, the command processor drained the
real primary ring, followed the title's real indirect buffers, loaded real microcode, issued Metal
draws and resolves, processed `PM4_XE_SWAP`, and presented through the normal path.

The result is still black. A representative content draw used vertex shader
`bc83ca8d00933874`, pixel shader `e4ed09508b1a5ae8`, and a real 1280x720 texture binding, but the
owned producer render target reported zero visible pixels. The resolve and presenter faithfully
carried that black source. This localizes the active failure to the producer draw rather than
command delivery or final presentation.

### Working foundations

- Native macOS window, Metal layer, device, presenter, and swap integration
- Shared guest-memory access and range tracking
- Texture layout, decode, upload, and Metal texture creation
- Xenos microcode analysis and SPIR-V translation
- SPIR-V-to-MSL translation and runtime Metal library creation
- Private Metal render-target allocation and readback
- EDRAM/resolve-copy compute path
- Guest output capture and swap presentation plumbing
- Standalone `metal_resolve_test` with byte-for-byte CPU/GPU comparison

### Milestones

| Milestone | State | Evidence |
| --- | --- | --- |
| A. Controlled resolve diagnostic | Passed | Magenta resolve reaches the destination and presentation path |
| B. Controlled render-target diagnostic | Passed | A solid Metal render target survives readback and resolve |
| C. Real producer content | In progress | Real title PM4 and microcode reach Metal, but the producer target remains black |
| D. Guest-visible resolve | Partial | Real resolves and `XE_SWAP` execute; they currently carry the black producer source |
| E. Recognizable menu | Not reached | No strict-path menu frame has been produced |

## Primary blocker

The first failing content draw builds a valid Metal pipeline but writes no visible pixels. Its
vertex shader references fetch constant 95 (`1fabd333 1000007a ...` in the captured live state),
while current producer diagnostics only decode the simpler fetch-0 setup draws. The next task is to
trace that exact fetch through address, stride, endian, range, shader binding, transformed position,
and raster state. Texture binding and resolve are downstream checks once vertex/raster coverage is
proven.

The reproducible SIGBUS and invalid-function failures from the initial diagnostic runs were traced
to an XEX-only staging directory. Running against the complete authorized game-data root proceeds
through audio, draws, resolves, and CP-driven swaps. A later interactive black/audio run coincided
with the app and development session exiting, so longer-run stability remains to be audited with
bounded, redirected diagnostics. Missing game data must not be treated as a renderer defect.

## Next development priority

Work in this order:

1. Capture the exact first failing content draw and its complete live register snapshot.
2. Decode vertex fetch 95 and validate its address, stride, endian mode, range, and Metal binding.
3. Verify translated vertex positions, clip-space coverage, viewport/scissor, culling, and color
   write state for that draw.
4. Match the translated MSL resource bindings to the exact microcode and live constants.
5. Demonstrate nonzero title pixels in the real producer render target.
6. Follow those pixels through the already-working normal resolve and `IssueSwap` path to the menu.

Do not restore heuristic command scavenging or direct presentation shortcuts. The real ring and
normal `XE_SWAP` path now provide the authoritative evidence needed to debug production.

## Useful source areas

- `src/graphics/metal/command_processor.cpp` — translation, producer draws, resolve, and probes
- `src/graphics/metal/shader.cpp` — SPIR-V-to-MSL translation and sanitization
- `src/graphics/metal/texture_cache.mm` — texture decode and Metal upload
- `src/kernel/xboxkrnl/xboxkrnl_video.cpp` — VdSwap packet construction and guest-buffer handoff
- `vendor/GoldenEye-Recomp/src/ge_hooks.cpp` — title integration and submission diagnostics
- `src/graphics/vulkan/command_processor.cpp` — useful behavioral reference, not the target backend

Build, test, and opt-in diagnostic commands are documented in [DEVELOPMENT.md](DEVELOPMENT.md).
