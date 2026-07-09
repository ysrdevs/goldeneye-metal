# GoldenEye native Metal project status

Last updated: 2026-07-09

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

The native Metal infrastructure is substantial and operational in isolation. The strict producer
path still presents black. A representative strict 30-second run produced four swaps with zero
visible pixels; the resolve/copy path matched its black source correctly. This localizes the main
failure before or within the title-driven producer draw, not in the final byte copy alone.

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
| C. Real producer content | Blocked | Title-driven render target remains black or receives incomplete draws |
| D. Watched 32-bpp structure | Partial | Buffer layout and copy behavior are understood, content provenance is not yet faithful |
| E. Recognizable menu | Not reached | No strict-path menu frame has been produced |

## Primary blocker

The current command provenance is not yet trustworthy enough. A heuristic VdSwap scavenger is
enabled by default and can recover plausible packets, but it can mis-bound command ranges and load
the wrong shader or fetch state. Exact kickoff and flush replay hooks exist behind diagnostic
environment flags, but they have not yet replaced the scavenger as the proven default path.

The strongest current shader-input symptom is a vertex-fetch mismatch: a shader expects one fetch
constant while the live register state looks texture-typed or otherwise inconsistent. That can
produce a valid Metal pipeline which draws nothing useful.

A separate SIGBUS/watchdog failure exists during some longer or heavily instrumented runs. It must
be isolated, but it is not evidence that the black producer frame is solved.

## Next development priority

Work in this order:

1. Prove exact kickoff and indirect-buffer boundaries from the title hook through the runtime.
2. Disable the VdSwap scavenger and reproduce the same packet sequence through exact replay.
3. Audit each failing draw's vertex fetch constants, endian handling, address, stride, and range.
4. Match translated MSL resource bindings to the exact microcode and live register snapshot.
5. Demonstrate nonzero title pixels in the real producer render target.
6. Follow those pixels through the normal resolve and IssueSwap path to the menu.
7. Minimize and fix the independent SIGBUS/watchdog failure.

Do not spend the next cycle polishing fallback presentation. It has already established that the
host can display pixels; the remaining priority is faithful production of those pixels.

## Useful source areas

- `src/graphics/metal/command_processor.cpp` — command replay, translation, draw, resolve, probes
- `src/graphics/metal/shader.cpp` — SPIR-V-to-MSL translation and sanitization
- `src/graphics/metal/texture_cache.mm` — texture decode and Metal upload
- `src/kernel/xboxkrnl/xboxkrnl_video.cpp` — swap bridge and guest-buffer handoff
- `vendor/GoldenEye-Recomp/src/ge_hooks.cpp` — title kickoff, flush, input, and diagnostic hooks
- `src/graphics/vulkan/command_processor.cpp` — useful behavioral reference, not the target backend

Build, test, and opt-in diagnostic commands are documented in [DEVELOPMENT.md](DEVELOPMENT.md).
