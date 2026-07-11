# GoldenEye native Metal project status

Last updated: 2026-07-11

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
now holds the title wait until the CP swap counter advances or the ring drains, backs off briefly
while waiting, never changes the title's skip bits or presented counter, and leaves RPTR writeback
solely to the CP. Ring completion is evaluated from one atomic snapshot: the ring must be configured, its
write pointer must be valid, and no commands may be pending. This also treats a wrapped write
pointer of zero as a valid drained state instead of confusing it with an uninitialized ring.

In bounded runs with scavenging, replay, forced presentation, and replacement rendering disabled,
WPTR progressed beyond 1088, the Metal primary-ring worker passed batch 192, and normal
`IssueSwap` reached at least 1408. The same path now produces the classification screen, complete
gun-barrel sequence, animated RARE splash, and dossier-style main menu from real title shaders,
vertex and index data, textures, constants, resolves, and swaps. The classification screen is
readable across the full 1280x720 output, the gold RARE logo is shaded and animated, and the menu
retains its portrait, crest, numbered options, selected row, help text, and page treatment. These
frames come from the current live render target rather than a retained stale readback.

A corrected-clock 1280x720 proving run on an Apple M3 Ultra used
`GOLDENEYE_AUTO_START=menu` and reached the dossier menu. Short captured intervals reported 30.0
and 59.9 guest-delivered frames per second; this proves a large pacing improvement but not
sustained 60 FPS. The diagnostic uses monotonic host time: Start is held from 200 ms until 1200 ms,
released until 2000 ms, and then pulsed for 250 ms every 6000 ms. Menu mode stops all injection at
19 seconds, before the fourth retry would begin at 20 seconds; one observation saw the menu by
18.14 seconds and provided an unforced measurement window. This changes input only and does not
inject another Start edge into the reached menu; it does not replace graphics, alter PM4, force
presentation, or bypass resolves. The title's own idle behavior remains active. The captured menu
therefore satisfies the strict rendering-provenance requirements for the first target milestone.
It does not establish playable output: culling, depth/stencil, and guest MSAA fidelity remain
incomplete, normal macOS keyboard delivery is absent, and navigation into gameplay has not been
verified.

The dominant host-side pacing fault after reaching the menu was a synchronous Metal wait after
every persistent producer draw. Those draws now use bounded asynchronous command-buffer queues:
each Metal command buffer carries up to 64 draws, with at most four retained batches and a
256-draw safety drain. Queue ordering preserves dependent draws and queued clears to the same
target, while explicit fences drain work before readback, target replacement or release,
resolve/swap, cache teardown, shared-memory or cached-texture mutation, and shaders with
memory-export side effects. The standalone pipeline-probe test covers ordered queued blending,
the batch and draw limits, explicit waits, resize, queued clear, shared/private regional readback,
and release behavior. Metal API validation remains clean for that isolated path.

Resolve and swap work no longer force a full private-texture readback for every title band. The
first three observed copies read only their 256, 256, and 208 source rows through one reusable
staging buffer; the remaining full-frame copy stays 1280x720, and clears remain ordered on the same
queue. A complete top-origin, full-width resolve also records an exact host BGRA result and its
guest tiled-byte mirror. Swap may reuse that result only when all fetch metadata and the live guest
bytes still match; tracked guest writes invalidate it, and a final comparison protects against
untracked CPU writes. Tiled surface ranges now use the real tiled-address extent rather than a
linear byte-size estimate, fixing the previously omitted tail of a 1280x720 destination.

The POSIX tick counter returns nanoseconds, but the former macOS frequency calculation divided one
billion by the value from `clock_getres`. That value is the clock's precision, not the unit of the
counter. On a timer with a multi-nanosecond quantum this accelerated guest time and scaled the FPS
display by the same factor. The frequency now correctly reports one billion ticks per second, and
a regression test protects that unit contract.

The presenter now draws a default-on FPS overlay into each completed guest front buffer. It counts
successful guest frame deliveries over a monotonic interval, not CAMetalLayer paint events, so
window repaints cannot inflate the displayed rate. `REX_METAL_SHOW_FPS=false` hides the overlay
without changing rendering.

Menu profiling exposed three avoidable hot paths. First, 256 KiB guest-memory invalidation blocks
could repeatedly invalidate a texture even when its own bytes were unchanged. The texture cache
now fingerprints the synchronized resident source span and skips CPU untile and Metal upload when
the bytes still match. Second, finalized MSL translations retain immutable reflection for buffer
indices, texture-fetch mappings, interpolators, memory-export state, and void fragments, removing
per-draw source scans and temporary reflection allocations. Third, per-draw constants, CPU vertex
data, and nonresident indices are suballocated from reusable upload arenas whose lifetime follows
their Metal command buffers instead of allocating a new buffer for each binding.

An exact GPU tiled-write resolve exists behind
`GOLDENEYE_METAL_GPU_TILED_RESOLVE=1`, with byte-for-byte coverage for full and partial rectangles
and all four 128-bit endian modes. It remains experimental and disabled by default because it did
not outperform the optimized CPU path on the proving system. It was not required for the observed
30.0-to-59.9 FPS menu samples. Menu visual parity was confirmed after the active performance
changes, and no diagnostic rendering shortcut is part of that result. Gameplay correctness and
performance remain unmeasured.

The decisive geometry fault was discarded index delivery. A four-vertex title triangle fan was
converted correctly by `PrimitiveProcessor` to the six indices `(1,2,0,2,3,0)`, but Metal issued
`drawPrimitives(6)`, causing vertex fetches 4 and 5 to read beyond a four-vertex buffer. The same
omission affected normal DMA-indexed draws and point/rectangle adapter indices. Metal now carries
every processed index-buffer type into `drawIndexedPrimitives`, preserves the processed endian and
line-loop constants, restores the guest alpha test, and flips Y for Metal's viewport convention.

The fixed-function and composition blocker exposed by live viewport/scissor state is resolved for
the observed title sequence. GoldenEye describes its 4xMSAA 720p frame through three resolve bands
(256, 256, and 208 lines). Metal now reads the current matching single-sample host context at each
copy, uses the authoritative local source rectangle, and assembles those bands at guest destination
Y 0, 256, and 512. This avoids both stale cross-target CPU caches and the wrap caused by dumping a
720-line logical target into the 512-line physical capacity of the 4xMSAA EDRAM layout. In the
proving frame, the three copies assembled to 73,900 visible pixels before the final normal
`IssueCopy`/`IssueSwap` route. Faithful guest multisampling is still future work.

Live guest viewport and scissor state is now applied to normal Metal draws. Metal pipelines are
also keyed by the active render-target index, normalized per-channel write mask, and
`RB_BLENDCONTROL`; blend-factor/operation mappings, dynamic blend constants, and the reversed Metal
channel-mask bit order are implemented. The two observed title modes (`0x00010001` replacement and
`0x07060706` source-alpha blending) now render correctly, and the isolated test covers a partial
R/B write mask. With those states enabled, swap 5 is a clean readable classification screen, while
swaps 64, 128, and 192 show successive views of the gold rotating RARE logo through current-context
copies.

The former post-RARE visual blocker was not a corrupt frame: the paired white discs are authored
geometry that opens the gun-barrel sequence. Bounded captures follow the discs across the screen,
show the barrel artwork and walking character, complete the red fade, rotate through the RARE
splash, and reach the main menu after a later Start edge. The first menu milestone is therefore no
longer blocked on that sequence.

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
- Indexed fan-remap, scissor, source-alpha blend, and partial-write-mask regression coverage in
  `metal_pipeline_probe_test`
- Bounded asynchronous producer submission with resource-mutation, readback, swap, and lifecycle
  fences
- Ordered queued-draw, 64-draw command buffers, four retained batches, 256-draw drain, explicit
  wait, resize, queued-clear, regional-readback, and release regression coverage in
  `metal_pipeline_probe_test`
- Regional private render-target readback with reusable staging and ordered queued clears
- Exact resolved-surface swap reuse guarded by fetch metadata, guest-byte comparison, tracked
  invalidation, and the true tiled-memory extent
- Texture-source fingerprinting that suppresses redundant untile and upload work after coarse
  guest-memory invalidation
- Immutable per-translation MSL reflection used directly by draw delivery
- Reusable command-buffer-owned upload arenas for constants, CPU vertex data, and indices
- Correct nanosecond POSIX clock frequency with unit regression coverage
- Default-on guest-delivery FPS overlay, disableable with `REX_METAL_SHOW_FPS=false`
- Byte-exact experimental GPU tiled resolve behind a default-off diagnostic flag
- Recognizable main-menu presentation through real title draws, guest-visible resolves, and normal
  `IssueSwap`

### Milestones

| Milestone | State | Evidence |
| --- | --- | --- |
| A. Controlled resolve diagnostic | Passed | Magenta resolve reaches the destination and presentation path |
| B. Controlled render-target diagnostic | Passed | A solid Metal render target survives readback and resolve |
| C. Real producer content | Passed for the first menu target | Real title shaders and resources produce the classification, gun-barrel and RARE sequences, then a recognizable main menu; depth/stencil and culling remain incomplete |
| D. Guest-visible resolve | Passed for current producer | Nonzero producer output reaches the guest buffer and normal presenter |
| E. Recognizable menu | Passed | Corrected-clock 1280x720 captures on Apple M3 Ultra present the real dossier-style menu through the strict draw/resolve/swap path; short samples reported 30.0 and 59.9 guest-delivered FPS |
| F. Sustained title execution | Passed | WPTR >1088 and normal `IssueSwap` at least 1408 without scavenging, replay, or forced presentation |

## Primary blocker

Shader interface and basic draw delivery are no longer the primary failure. The translated vertex
and pixel stages agree on interpolator locations, packed float/bool/fetch constants are delivered,
array textures and samplers are bound, processed indices are consumed, and the title splash is
recognizable.

The first-menu rendering blocker is resolved. The next boundary is stable, interactive navigation
from that menu into the first gameplay scene. The current macOS window backend does not forward
keyboard key-down/key-up events to the guest, so unattended verification uses the opt-in periodic
Start diagnostic and normal users need a supported controller backend. That is sufficient for a
rendering proof, but not for a usable application.

The remaining graphics risk is the next layer of fixed-function fidelity. Live viewport/scissor,
current host-context ownership, the observed banded copy sequence, the title's two blend modes,
blend constants, and per-channel write masks are active and verified. Culling/front-face selection,
shared depth/stencil ownership, and true guest MSAA behavior are still absent and are likely to
matter more in gameplay than in the mostly layered 2D menu. The former per-draw wait has been
replaced by bounded asynchronous submission with explicit safety fences, and visual parity has
been rechecked at the menu. Corrected-clock menu samples now range from 30.0 to 59.9 FPS on the
proving system, so the catastrophic pacing fault is gone, but sustained menu pacing still needs
characterization. Performance after entering a mission is unknown and must be measured
independently. Missing game data remains a separate launch problem and must not be diagnosed as a
renderer failure.

## Next development priority

Work in this order:

1. Wire normal macOS keyboard events into the input path or validate the SDL controller route, then
   navigate the real menu into the first mission without automated selection.
2. Capture and profile the first gameplay scene with dumps and verbose logging disabled,
   separating guest pacing, command processing, texture work, resolve/readback, and presentation.
3. Apply guest cull/front-face state and add focused regression coverage before judging 3D scenes.
4. Build shared depth/stencil ownership and faithful guest MSAA resolve behavior for persistent
   Metal render targets.
5. Re-evaluate the experimental GPU tiled resolve only against representative gameplay workloads;
   keep the proven CPU path as the default until measurements justify a change.

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
