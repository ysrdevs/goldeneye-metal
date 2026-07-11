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

Sustained title execution is now working. The old two-swap ceiling was caused by the host's 80 ms
GPU-wait recovery: while Metal still owned a primary batch, it set a title-owned skip bit. The
title's submission routine saw that bit and returned before writing WPTR for kickoffs 31 and 32.
The recovery also published a sampled RPTR into command-processor-owned writeback memory. The hook
now holds the title wait until the CP swap counter advances or the ring drains, yields while
waiting, never changes the title's skip bits or presented counter, and leaves RPTR writeback solely
to the CP.

In a bounded run with scavenging, replay, forced presentation, and replacement rendering disabled,
WPTR progressed beyond 1088, the Metal primary-ring worker passed batch 192, and normal
`IssueSwap` reached 64. The same run produced nonzero pixels from real title shaders and resources,
carried them through the normal guest resolve, and presented them. Captured results contain
malformed geometry and flat colors rather than a recognizable menu, so fixed-function and
fragment-output fidelity remain incomplete.

### Working foundations

- Native macOS window, Metal layer, device, presenter, and swap integration
- Shared guest-memory access and range tracking
- Authoritative shared Metal-buffer residency for producer vertex fetches
- CPU-backed resolve commits kept coherent with the Metal shared-memory copy
- Texture layout, decode, upload, and Metal texture creation
- Metal 2D/stacked textures matching translated `texture2d_array` bindings
- Xenos microcode analysis and SPIR-V translation
- SPIR-V-to-MSL translation and runtime Metal library creation
- Private Metal render-target allocation and readback
- EDRAM/resolve-copy compute path
- Guest output capture and swap presentation plumbing
- Standalone `metal_resolve_test` with byte-for-byte CPU/GPU comparison
- Standalone `metal_pipeline_probe_test` for external-buffer and array-texture delivery

### Milestones

| Milestone | State | Evidence |
| --- | --- | --- |
| A. Controlled resolve diagnostic | Passed | Magenta resolve reaches the destination and presentation path |
| B. Controlled render-target diagnostic | Passed | A solid Metal render target survives readback and resolve |
| C. Real producer content | In progress | Real title shaders now produce nonzero pixels, but the image is malformed |
| D. Guest-visible resolve | Passed for current producer | Nonzero producer output reaches the guest buffer and normal presenter |
| E. Recognizable menu | Not reached | No strict-path menu frame has been produced |
| F. Sustained title execution | Passed | WPTR >1088 and normal `IssueSwap` 64 without scavenging or replay |

## Primary blocker

Fetch constant 95 is no longer an unknown. Its live value addresses `0x1FABD330`, contains 30
dwords, uses endian mode 2, and supplies six five-dword vertices through `fetch_constants[47].zw`.
The referenced range is made resident in the authoritative Metal shared-memory buffer before the
draw. The exact content vertex shader produces coverage with both a controlled solid fragment and
real title pixel shaders.

The remaining failure is output fidelity. The first sustained frames contain title-driven pixels,
but geometry and color are visibly corrupt. The next investigation must compare the earliest
malformed draw's interpolators, packed constants, texture contents and sampler state, then apply
the guest viewport, scissor, culling, depth, blend, and color-write state faithfully. Missing game
data remains a separate launch problem and must not be diagnosed as a renderer failure.

## Next development priority

Work in this order:

1. Capture the earliest malformed nonzero draw and its complete live register snapshot.
2. Validate its vertex-to-fragment interpolators, packed constants, texture contents, texture type,
   and sampler state against the translated MSL.
3. Apply and verify the guest viewport, scissor, culling, depth, blend, and color-write state.
4. Replace remaining probe-specific system-constant overrides with faithful guest state.
5. Produce a recognizable title frame through the normal resolve and `IssueSwap` path.
6. Audit pacing, input, and long-run stability once the frame is visually correct.

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
