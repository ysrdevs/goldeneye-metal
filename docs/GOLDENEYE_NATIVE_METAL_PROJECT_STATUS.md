# GoldenEye native Metal project status

Last updated: 2026-07-15

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

The first target, a recognizable title-driven menu frame, is passed. The current verified boundary
is fully rendered first-Dam gameplay through the strict path. The long-term target remains stable,
faithful, physically playable output.

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
now preserves guest watchdog time while the CP actively drains the ring, backs off briefly while
waiting, never treats a swap-counter advance as completion, never changes the title's skip bits or
presented counter, and leaves RPTR writeback solely to the CP. Ring completion is evaluated from
one atomic snapshot: the ring must be configured, its write pointer must be valid, and no commands
may be pending. A wrapped write pointer of zero is also treated as a valid drained state rather
than an uninitialized ring.

In bounded runs with scavenging, replay, forced presentation, and replacement rendering disabled,
WPTR progressed beyond 1088, the Metal primary-ring worker passed batch 192, and normal
`IssueSwap` reached at least 4416. The same path now produces the classification screen, complete
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

The deterministic mission route pairs `GOLDENEYE_AUTO_START=menu` with
`GOLDENEYE_AUTO_MISSION=dam`. After a 22-second dossier delay, it contributes five ordinary A
press/release pulses for Select Mission, Dam, Agent, open briefing, and Start mission. Between the
fourth and fifth A pulses it contributes 450 ms of full positive left-stick Y, releases it, and
allows 750 ms for focus to settle. The injector merges these contributions with normal controller
state, permanently idles after the final release, and never inspects or changes title menu state,
PM4, resolves, or presentation.

That route now reaches a clean real Dam briefing at roughly 60 FPS and fully rendered first-Dam
gameplay. An earlier dynamic 1280x720 proving window measured 29.68 FPS. Oldest-first upload
command-buffer retirement and a 2048 global retained-draw ceiling then produced a correct 46.5 FPS
screenshot while reducing draw time from roughly 10 ms to 7.2–7.5 ms per swap. A later correct Dam
screenshot displayed 60.0 FPS; stable complete windows reported roughly 6.4–6.7 ms draw,
4.2–4.35 ms copy, 1.74 ms swap, and 2.9 ms `WAIT_REG_MEM` time per swap. Color signatures continue
changing and sampled register-color draws remain routed. These are strict rendering and
deterministic-input proofs, not a claim that every Dam view sustains 60 FPS or that physical input
and full playability are complete.

The first mission transition initially exposed a separate direct-decoder safety bug. A malformed
8192x8191 tiled descriptor requested a 256 MiB source span from near the end of the 512 MiB guest
physical range. The texture cache rejected the descriptor, but the diagnostic/probe decoder
bypassed that gate and entered `Untile`, reading exactly beyond the guest allocation. The direct
path now validates dimensions, pitch, checked source/output arithmetic, exact linear or tiled
extent, physical range, host-size conversion, and allocation size before translating or reading a
guest pointer. Invalid bindings use the dummy texture. A standalone test preserves the observed
descriptor alongside valid exact-boundary and arithmetic-overflow cases.

Native macOS input now uses a first-responder Cocoa content view and forwards physical key down,
key up, repeat, character, modifier, mouse button, wheel, and relative-motion events through the
common `Window` input path. Hidden mouse capture is unbounded and restored on focus loss or
teardown. The existing keyboard/mouse controller driver consumes those events when
`REX_MNK_MODE=true`; focused tests now feed native-style events through that real driver and verify
WASD, Shift/Return, mouse axes/buttons, keyboard right-stick binds, modal suppression, and focus
loss. A root Finder launcher discovers game data, checks the minimum required layout, selects
Metal, enables MnK, and removes inherited unattended input diagnostics. The GoldenEye pause menu
releases native capture, and its macOS Controls page now edits the common-driver cvars it actually
consumes.
Modifier `FlagsChanged` events avoid AppKit's key-down-only repeat selector, with a focused
regression for their previous-state delivery.
Start defaults to Return on macOS because Escape also opens the host pause overlay. This is a real
input path, not a guest-state or menu-selection diagnostic. Physical navigation and gameplay
control remain to be validated end to end.

The release path now also produces a normal unsigned `GoldenEye Metal.app` with a native first-run
setup window and original icon. A player can select a compatible local backup ZIP, its Xbox
LIVE/STFS package, or an extracted folder. The importer hashes the complete package before the
general container parser sees it, accepts only the supported title revision, rejects unsafe or
case-colliding paths and unexpected size/count/depth, streams into a private staging directory,
validates the result, and publishes the cache atomically under Application Support. ZIP handling
invokes the system reader without a shell and streams only the single package member; it never
extracts arbitrary archive paths, downloads content, or includes game data in the app. Direct STFS
mounting remains available to the runtime, while the one-time flat import is the default player
route for better read performance. The older `.command` launcher remains a build-tree developer
convenience. Signing, DMG creation, and notarization are deliberately release-owner steps and have
not been executed as part of this milestone.

Modern controller input now follows the native SDL gamepad path by default on macOS and is selected
explicitly by the Finder launcher. SDL's bundled mappings cover DualShock 4, DualSense, Xbox One,
and Xbox Series X|S controllers; no separately downloaded mapping database is required for those
families. The backend discovers already-connected pads, accepts hot-plug and removal events,
assigns player 1 first, compacts four guest slots, and promotes a waiting fifth pad. It refreshes
remapped state, safely ignores stale events, stops rumble on focus/modal loss and teardown, and
uses SDL's current success semantics for vibration. Controller and MnK drivers remain active
together, including keystroke delivery. SDL virtual-device tests exercise representative
normalized input, inactivity/resume, repeated driver teardown, five-pad promotion, reconnect,
rumble values, and rumble failure propagation. Physical USB and Bluetooth acceptance across at
least one Sony and one Xbox controller remains a manual validation boundary rather than an
automated-test claim.

Metal now maps the guest's polygon cull-front, cull-back, and front-face winding state to the
render encoder. Point, line, and rectangle expansion routes remain uncullable, and fully culled
polygons still execute vertex memory export without claiming a host color target. The isolated
pipeline test covers front and back culling against both clockwise and counter-clockwise guest
front faces. Depth-only draw routing, polygon fill mode, shared guest depth/stencil, and faithful
MSAA remain open.

Persistent host render contexts now own private `Depth32Float_Stencil8` attachments with ordered
load/store lifetime. Metal depth/stencil state maps normalized guest Z enable, writes, all compare
functions, independent front/back stencil compares and operations, read/write masks, and
references. The isolated probe verifies that a near depth write rejects a later far draw, disabling
depth writes allows that far draw, and stencil replace/equal/reject persists across draws. This is
not shared guest EDRAM ownership yet: each host color context has an independent attachment,
`D24S8` and `D24FS8` lack 24-bit precision/alias fidelity, and guest depth clears, copies, resolves,
readback, texture aliases, and multisampling are still absent.

The title's GPU-wait hook no longer treats a swap-counter advance or one transient drained-ring
snapshot as completion of trailing primary-ring work. The guest D3D watchdog has a hardware-scale
timeout, so exposing the accumulated clock during that race produced expensive false "GPU is
hung" dumps and debug traps even while normal swaps kept advancing. The hook now requires the same
wait context and heartbeat to remain continuously drained for 60 ms before restoring watchdog
time; any pending sample or heartbeat change restarts the grace. A 74-second proving run reached
normal `IssueSwap` 4416, repeatedly presented the dossier, and recorded zero hang dumps, traps,
`WAIT_REG_MEM` stalls, or title-watchdog stall markers. Its pre-fix control recorded 14 hang dumps
and 14 traps. While a ring remains pending, the hold continues only while RPTR makes progress;
30 seconds without RPTR movement restores the title's genuine hang detection. No fence, RPTR,
presented counter, skip bit, or render command is modified.

The dominant host-side pacing fault after reaching the menu was a synchronous Metal wait after
every persistent producer draw. Those draws now use bounded asynchronous command-buffer queues:
each Metal command buffer carries up to 64 draws and each context has four upload arenas. When all
four are occupied, it retires the oldest command buffer rather than waiting for the newest; the
independent global retained-draw ceiling is 2048. Shared-queue ordering preserves dependent draws,
uploads, and queued clears, while explicit fences drain work before readback, target replacement
or release, resolve/swap, cache teardown, shared-memory or cached-texture mutation, and shaders
with memory-export side effects. The standalone pipeline-probe test covers 256-draw oldest-buffer
retirement, four upload-arena lifetimes, ordered queued blending, explicit waits, resize, queued
clear, shared/private regional readback, and release behavior. Metal API validation remains clean
for that isolated path.

The CPU resolve fallback no longer forces a full private-texture readback for every title band.
Its first three observed copies read only their 256, 256, and 208 source rows through one reusable
staging buffer; the remaining full-frame copy stays 1280x720, and clears remain ordered on the same
queue. The default GPU compute route instead writes the tiled guest destination directly. A
complete top-origin, full-width CPU resolve can also record an exact host BGRA result and its guest
tiled-byte mirror. Swap may reuse that result only when all fetch metadata and the live guest bytes
still match; tracked guest writes invalidate it, and a final comparison protects against untracked
CPU writes. Tiled surface ranges use the real tiled-address extent rather than a linear byte-size
estimate, fixing the previously omitted tail of a 1280x720 destination.

The POSIX tick counter returns nanoseconds, but the former macOS frequency calculation divided one
billion by the value from `clock_getres`. That value is the clock's precision, not the unit of the
counter. On a timer with a multi-nanosecond quantum this accelerated guest time and scaled the FPS
display by the same factor. The frequency now correctly reports one billion ticks per second, and
a regression test protects that unit contract.

The presenter now draws a default-on FPS overlay into each completed guest front buffer. It counts
successful guest frame deliveries over a monotonic interval, not CAMetalLayer paint events, so
window repaints cannot inflate the displayed rate. `REX_METAL_SHOW_FPS=false` hides the overlay
without changing rendering.

The opt-in profiler now reports `WAIT_REG_MEM` alongside draw, copy, swap, resource-wait, texture,
and presenter ledgers in complete 64-swap windows. Its wait report includes the hottest sampled
address, reference, mask, and poll count. That makes the remaining roughly 2.9 ms-per-swap fence
cost visible without enabling high-volume renderer logs.

Menu profiling exposed three avoidable hot paths. First, 256 KiB guest-memory invalidation blocks
could repeatedly invalidate a texture even when its own bytes were unchanged. The texture cache
now fingerprints the synchronized resident source span and skips CPU untile and Metal upload when
the bytes still match. Second, finalized MSL translations retain immutable reflection for buffer
indices, texture-fetch mappings, interpolators, memory-export state, and void fragments, removing
per-draw source scans and temporary reflection allocations. Third, per-draw constants, CPU vertex
data, and nonresident indices are suballocated from reusable upload arenas whose lifetime follows
their Metal command buffers instead of allocating a new buffer for each binding.

The exact GPU tiled-write resolve is now enabled by default. Eight byte-for-byte cases cover full
and partial rectangles across all four 128-bit endian modes, and a live Dam proving run completed
thousands of resolves with zero fallbacks. `GOLDENEYE_METAL_GPU_TILED_RESOLVE=0` is the explicit
CPU-path opt-out. Menu and gameplay visual parity were confirmed after enabling the default path,
and no diagnostic rendering shortcut is part of those results. Remaining work is to make resolve
submission more asynchronous and reduce the fence wait around its consumers, not to re-establish
basic tiled-write correctness.

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
- Ordered queued-draw, 64-draw command buffers, 256-draw oldest-buffer retirement, four upload
  arenas, explicit wait, resize, queued-clear, regional-readback, and release regression coverage
  in
  `metal_pipeline_probe_test`
- A 2048-draw global asynchronous ceiling that avoids normal-frame full-queue drains while keeping
  pathological streams bounded
- Regional private render-target readback with reusable staging and ordered queued clears
- Exact resolved-surface swap reuse guarded by fetch metadata, guest-byte comparison, tracked
  invalidation, and the true tiled-memory extent
- Texture-source fingerprinting that suppresses redundant untile and upload work after coarse
  guest-memory invalidation
- Immutable per-translation MSL reflection used directly by draw delivery
- Reusable command-buffer-owned upload arenas for constants, CPU vertex data, and indices
- Correct nanosecond POSIX clock frequency with unit regression coverage
- Default-on guest-delivery FPS overlay, disableable with `REX_METAL_SHOW_FPS=false`
- Native Cocoa keyboard and relative-mouse delivery through the common controller driver
- Default-on SDL gamepad input with built-in PS4, PS5, Xbox One, and Xbox Series mappings
- Hot-plug, four-slot compaction, fifth-pad promotion, focus-safe rumble, and virtual-device tests
- Native macOS application launcher with exact local ZIP/STFS validation, safe atomic import,
  remembered game data, and automatic Metal/controller/MnK selection
- Build-tree `.command` launcher for local developer runs
- Controller-state input regressions plus pause-menu capture suppression and runtime macOS rebinding
- Guest polygon cull mode and front-face winding with focused Metal regression coverage
- Persistent per-context Metal depth/stencil state with ordered depth and stencil probe coverage
- Stable-drain guest-watchdog timing with a zero-hang 74-second swap-4416 live proof
- Checked direct texture-decode bounds with the first-Dam invalid descriptor as a regression
- Deterministic input-only dossier-to-Dam route with balanced A and left-stick contributions
- Safe macOS modifier transitions with focused previous-state regression coverage
- Opt-in 64-swap command, `WAIT_REG_MEM`, wait-reason, and 64-attempt presenter profiling ledgers
- Default GPU tiled resolve with eight byte-exact cases and thousands of live resolves without a
  fallback; `GOLDENEYE_METAL_GPU_TILED_RESOLVE=0` opts out
- Recognizable main-menu presentation through real title draws, guest-visible resolves, and normal
  `IssueSwap`
- Fully rendered, dynamic first-Dam gameplay through the strict path, with correct 46.5 and
  60.0 FPS captures and broader-scene stability still in progress

### Milestones

| Milestone | State | Evidence |
| --- | --- | --- |
| A. Controlled resolve diagnostic | Passed | Magenta resolve reaches the destination and presentation path |
| B. Controlled render-target diagnostic | Passed | A solid Metal render target survives readback and resolve |
| C. Real producer content | Passed for the first menu target | Real title shaders and resources produce the classification, gun-barrel and RARE sequences, then a recognizable main menu; per-context depth/stencil state is present but guest-shared EDRAM fidelity remains incomplete |
| D. Guest-visible resolve | Passed for current producer | Nonzero producer output reaches the guest buffer and normal presenter |
| E. Recognizable menu | Passed | Corrected-clock 1280x720 captures present the real dossier menu through the strict path; short samples reported 30.0 and 59.9 guest-delivered FPS |
| F. Sustained title execution | Passed | WPTR >1088 and normal `IssueSwap` at least 4416 without scavenging, replay, forced presentation, false GPU-hang dumps, or debug traps |
| G. First mission gameplay | Passed for deterministic input | The input-only route reaches a clean Dam briefing and fully rendered dynamic gameplay; correct captures have displayed 46.5 and 60.0 FPS, but sustained 60 across broader views remains in progress |
| H. Native player launcher | Passed unsigned | The arm64 `.app` stages with portable runtime linkage, icon, metadata, and notices but no game content; the exact supported LIVE/STFS package imports and validates all 1,803 title files through the same backend used by the first-run UI |

## Primary blocker

Shader interface and basic draw delivery are no longer the primary failure. The translated vertex
and pixel stages agree on interpolator locations, packed float/bool/fetch constants are delivered,
array textures and samplers are bound, processed indices are consumed, and the title splash is
recognizable.

The first-menu and first-gameplay rendering boundaries are resolved. The former reproducible
30 FPS gameplay ceiling has been crossed: correct Dam captures have displayed 46.5 and 60.0 FPS,
and stable complete windows around the latter reduced draw, copy, and swap costs to approximately
6.4–6.7 ms, 4.2–4.35 ms, and 1.74 ms per swap. The current primary blocker is repeatability across
broader Dam views. Resolve submission and the guest-visible fence remain synchronization targets;
the profiler measures roughly 2.9 ms of `WAIT_REG_MEM` per swap and reports the top
address/reference/mask/poll condition. The presenter ledger measures CPU submission calls, not GPU
completion or display latency.

Physical native input is the next validation boundary after that performance pass. The macOS
window forwards keyboard and mouse events into the existing controller driver, while SDL now
normalizes modern Sony and Xbox pads into the same guest state. Focused tests cover host MnK
delivery, controller mapping, hot-plug, slot promotion, rumble, and merged keystrokes. The launcher
and pause-menu integration remove the known configuration/capture gaps. The deterministic mission
injector proves that ordinary controller edges can traverse the complete route, but a person still
needs to confirm navigation, sustained Bond control, pause/resume, focus recovery, and real USB and
Bluetooth behavior through the physical paths.

The remaining graphics risk is the next layer of fixed-function fidelity. Live viewport/scissor,
current host-context ownership, the observed banded copy sequence, the title's two blend modes,
blend constants, per-channel write masks, culling/front-face selection, and per-context
depth/stencil state are active and verified. Depth-only EDRAM draw routing, guest-addressed shared
depth/stencil ownership, depth clear/resolve fidelity, and true guest MSAA behavior remain absent.
The current per-color-target private depth attachment is useful but cannot establish shared-depth
or multisample correctness. The supported local-backup importer now prevents missing game data
from silently looking like a renderer fault; unsupported or incomplete data is rejected before
runtime initialization.

## Next development priority

Work in this order:

1. Repeat complete-window profiling across several deterministic Dam views, leaving frame and
   shader dumps and verbose diagnostics disabled, and identify which views fall below 60 FPS.
2. Make tiled resolve submission more asynchronous and reduce the measured `WAIT_REG_MEM` fence
   cost without changing guest-visible ordering, pixels, command provenance, or safety fences.
3. Navigate into Dam and validate gameplay with physical native keyboard/mouse input plus one Sony
   and one Xbox controller over USB and Bluetooth, including hot-plug, rumble, focus loss, capture
   recovery, pause, and sustained player control.
4. Route depth-only draws and promote private per-context depth/stencil to guest-addressed shared
   ownership with faithful clear, copy, resolve, readback, and texture-alias behavior.
5. Implement faithful guest MSAA and remaining polygon fill/depth-bias state, then re-evaluate
   resolve and fence behavior against representative gameplay workloads.

Do not restore heuristic command scavenging or direct presentation shortcuts. The real ring and
normal `XE_SWAP` path now provide the authoritative evidence needed to debug production.

## Useful source areas

- `src/graphics/metal/command_processor.cpp` — translation, producer draws, resolve, and probes
- `src/graphics/metal/shader.cpp` — SPIR-V-to-MSL translation and sanitization
- `src/graphics/metal/texture_cache.mm` — texture decode and Metal upload
- `src/kernel/xboxkrnl/xboxkrnl_video.cpp` — VdSwap packet construction and guest-buffer handoff
- `vendor/GoldenEye-Recomp/src/ge_hooks.cpp` — title integration and submission diagnostics
- `src/graphics/vulkan/command_processor.cpp` — useful behavioral reference, not the target
  backend

Build, test, and opt-in diagnostic commands are documented in [DEVELOPMENT.md](DEVELOPMENT.md).
