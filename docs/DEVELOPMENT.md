# Development guide

## Source model

This is one repository. The runtime/toolchain lives at the root and the game integration is
vendored as ordinary source under `vendor/GoldenEye-Recomp/`. Only source dependencies under
`thirdparty/` are Git submodules.

Generated C++ and game data are intentionally excluded. A clean checkout can build the toolchain
and Metal isolation tests without game data; building the game target requires locally generated
sources from an authorized compatible XEX.

## Apple Silicon build

```sh
brew install cmake spirv-cross
git submodule update --init --recursive

cmake --preset macos-arm64-release
cmake --build --preset macos-arm64-release --parallel
ctest --preset macos-arm64-release --output-on-failure
```

Build only the main developer targets:

```sh
cmake --build --preset macos-arm64-release \
  --target rexglue metal_resolve_test metal_pipeline_probe_test \
  metal_texture_decode_validation_test unit_tests --parallel
```

The Metal backend deliberately fails during configuration if SPIRV-Cross MSL support is missing.
This prevents a nominally successful build with a nonfunctional shader path.

## Game code generation and build

From the repository root:

```sh
mkdir -p vendor/GoldenEye-Recomp/assets
# Supply vendor/GoldenEye-Recomp/assets/default.xex locally for code generation.

./out/macos-arm64/rexglue codegen \
  vendor/GoldenEye-Recomp/ge_manifest.toml

cmake -S vendor/GoldenEye-Recomp --preset macos-arm64-release
cmake --build vendor/GoldenEye-Recomp/out/build/macos-arm64-release \
  --target ge --parallel
```

The vendor preset points `REXSDK_DIR` back to the repository root, enables Metal, disables Vulkan,
and leaves runtime profiling off. Generated files stay under `vendor/GoldenEye-Recomp/generated/`
and must not be committed.

Run the game against the complete authorized game-data directory, not an XEX-only staging
directory. The runtime root must contain `default.xex`, `files/`, and the title's companion data:

```sh
./vendor/GoldenEye-Recomp/out/build/macos-arm64-release/GoldenEye \
  --game_data_root /absolute/path/to/complete/game-data \
  --gpu metal
```

Missing extracted data can look like a runtime or renderer fault because optional resource loads
resolve to null. Game data stays local and must never be committed.

### Native macOS input

The Cocoa window delivers physical key down/up events, buttons, wheel input, and Retina-aware
relative mouse motion to the common keyboard/mouse input driver. Hidden capture disassociates the
pointer so mouse-look is not clamped by a screen edge, and focus loss or teardown restores the
system cursor. Modifier `FlagsChanged` events never query AppKit's key-down-only repeat state; a
focused regression covers their previous-state delivery.

For an ordinary interactive run, double-click `Launch GoldenEye.command` at the repository root.
It finds a game-data folder, checks the minimum `default.xex` plus `files/` layout, supplies the
local runtime-library path, selects Metal, enables SDL gamepads and MnK together, and clears
inherited auto-input diagnostics. Manual launches can enable the same path explicitly:

```sh
REX_INPUT_BACKEND=sdl REX_MNK_MODE=true \
./vendor/GoldenEye-Recomp/out/build/macos-arm64-release/GoldenEye \
  --game_data_root /absolute/path/to/complete/game-data \
  --gpu metal
```

The default gameplay bindings include Space for A, Shift for B, WASD for the left stick, the arrow
keys for the D-pad, mouse motion for the right stick, and left/right mouse buttons for the
right/left triggers. Start defaults to Return on macOS because Escape opens the host pause menu.

The SDL gamepad backend is the runtime default. SDL's bundled mappings normalize DualShock 4,
DualSense, Xbox One, and Xbox Series X|S pads to the guest's XInput-style state over the USB and
Bluetooth transports exposed by macOS. Attach and removal events are handled at runtime, slots
stay compact from player 1 through player 4, and an ignored fifth controller is promoted when a
slot opens. Focus loss or a host overlay stops active rumble. The SDL and MnK drivers merge at the
input-system boundary, including keystrokes, so enabling controllers does not disable native
keyboard/mouse control. `REX_HID_MAPPINGS_FILE=/absolute/path/to/mappings.txt` may supply
additional SDL mappings for an unusual third-party pad; the built-in mappings need no external
database file.
The host pause menu suppresses guest input and releases native mouse capture immediately, even
between guest polls. On macOS its Controls page edits the common MnK bindings and sensitivity that
are actually consumed at runtime, including optional keyboard right-stick directions. Per-launch
environment cvars are reapplied after saved configuration, so the launcher's Metal, MnK, and
canonical game-data selections remain authoritative for the session and for an in-game restart.
Saving settings may also preserve non-default launcher choices in the local ignored `ge.toml`.
The native input route is normal runtime input; it does not alter guest state or the graphics path.

#### Physical controller acceptance

For each available controller family, test USB and Bluetooth where the hardware supports them:

1. Connect the pad before launch and confirm player 1 works through the title menu and Dam.
2. Verify every face button, Start/Back, D-pad direction, stick and stick click, bumper, and trigger.
3. Disconnect and reconnect during play, then repeat by connecting only after the game has started.
4. Trigger rumble, open the host pause menu, and confirm vibration stops while input is suppressed.
5. Switch focus away and back, then confirm neutral state, resumed control, and no stuck buttons.

Record the controller model, transport, macOS version, and any missing input or rumble behavior.
Do not turn a virtual-device pass into a physical-hardware claim.

## Tests

`metal_resolve_test` allocates Metal EDRAM and destination buffers, runs the native resolve-copy
kernel, and compares GPU output against a CPU reference byte for byte. Its eight tiled-write cases
cover full and partial rectangles across all four 128-bit endian modes.

`metal_texture_decode_validation_test` exercises the checked byte-layout contract used by the
direct Metal texture decoder. It covers valid linear and tiled spans, an exact physical-memory
boundary, arithmetic overflow, and the 8192x8191 out-of-range descriptor observed during the first
Dam transition. It requires no game data or Metal device.

The MnK unit regressions feed native-style key, modifier, relative-motion, and button events into
the real controller driver. They verify WASD, Shift/Return, mouse axes and triggers, optional
keyboard right-stick binds, comma-separated alternative binds, modal suppression, and focus-loss
clearing. SDL virtual-gamepad regressions verify representative face-button and D-pad input, both
sticks and triggers, inactive-state suppression, attach/remove handling, four-slot assignment,
fifth-pad promotion, reconnect, rumble values, rumble failure propagation, and lifecycle teardown. A
separate input-system regression ensures an idle SDL pad cannot starve a simultaneous MnK
keystroke. Separate CTest entries check the Finder launcher's shell syntax and exercise canonical
game-data discovery, environment precedence, paths with spaces, controller/MnK selection, and
diagnostic cleanup.

`metal_pipeline_probe_test` renders through an externally owned Metal vertex buffer, samples a
single-layer `texture2d_array`, expands a four-vertex fan through a six-entry index buffer, checks
scissor delivery, verifies the title's source-alpha blend mode, and validates Xenos-to-Metal
per-channel write-mask mapping. It also covers front/back culling with clockwise and
counter-clockwise guest front faces, persistent near/far depth rejection, disabled depth writes,
stencil replace/equal/reject, ordered draws across queued command buffers, the 64-draw
command-buffer boundary, 256-draw oldest-buffer retirement, four upload-arena lifetimes, explicit
waits, reusable upload-arena lifetime across command-buffer completion, and synchronization during
shared/private regional readback, queued clear, resize, and context release. It protects the
resource, fixed-function, and asynchronous-lifetime contracts used by translated producer draws.
It does not replace title-level validation of texture-cache uploads, resolve coherence, or
sustained execution.

Run the Metal test with API validation enabled after changing submission or resource ownership:

```sh
MTL_DEBUG_LAYER=1 ./out/macos-arm64/metal_pipeline_probe_test
```

Unit tests are enabled by the Apple Silicon preset. PPC assembly tests are disabled by default and
require an external `powerpc-none-elf` binutils toolchain. Enable them explicitly with:

```sh
cmake --preset macos-arm64-release -DREXGLUE_BUILD_PPC_TESTS=ON
```

## Runtime and Metal diagnostics

File-producing and execution-changing diagnostics are opt-in so normal runs do not write files or
substitute success criteria. The presenter FPS overlay is the default-on exception and may be
hidden without changing execution.

| Environment variable | Effect |
| --- | --- |
| `GOLDENEYE_AUTO_START=1` | Hold Start during the initial input window to skip startup screens |
| `GOLDENEYE_AUTO_START=periodic` | Use monotonic-time startup input followed by recurring Start pulses for unattended title/menu traversal |
| `GOLDENEYE_AUTO_START=menu` | Use the periodic schedule until 19 seconds, then stop before the 20-second retry so it does not immediately leave a newly reached dossier menu |
| `GOLDENEYE_AUTO_MISSION=dam` | After a 22-second dossier delay, traverse the default route into Dam using ordinary controller input contributions |
| `REX_INPUT_BACKEND=sdl` | Select SDL gamepad input; this is the runtime and Finder-launcher default |
| `REX_MNK_MODE=true` | Enable keyboard/mouse controller emulation; required for native keyboard gameplay input |
| `REX_HID_MAPPINGS_FILE=/path/file.txt` | Load additional SDL controller mappings; built-in mappings are used when unset |
| `REX_KEYBIND_START=Return` | Explicitly override Start to Return; this is already the macOS default |
| `REX_METAL_SHOW_FPS=false` | Hide the default-on presenter FPS overlay; its value counts guest front-buffer deliveries, not host repaints |
| `GOLDENEYE_METAL_PROFILE=1` | Emit low-overhead command, `WAIT_REG_MEM`, wait-reason, texture-fallback, and presenter ledgers in complete 64-swap/attempt windows |
| `GOLDENEYE_METAL_GPU_TILED_RESOLVE=0` | Disable the default GPU tiled-write resolve path for CPU-fallback comparison |
| `GOLDENEYE_METAL_DUMP_SHADERS=1` | Write translated/failed MSL and selected microcode dumps under `/tmp` |
| `GOLDENEYE_METAL_DUMP_FRAMES=1` or `all` | Write all selected BGRA frame stages as PPM files under `/tmp` |
| `GOLDENEYE_METAL_DUMP_FRAMES=N` | Write only presenter frame number `N`; this avoids the decoded-texture inspection used by an all-stage dump |
| `GOLDENEYE_METAL_SUBMISSION_DIAGNOSTICS=1` | Log rate-limited kickoff, flush, and VdSwap metadata without replaying it |
| `GOLDENEYE_METAL_VERBOSE_DIAGNOSTICS=1` | Restore high-volume renderer diagnostics that are suppressed during normal performance runs |
| `GOLDENEYE_METAL_VDSWAP_SCAVENGE=1` | Enable the legacy heuristic VdSwap packet scavenger; this changes execution and is not a strict-path result |
| `GOLDENEYE_METAL_IM_LOAD_DIAGNOSTICS=1` | Inspect words beyond pointer-based shader extents for boundary diagnosis |
| `GOLDENEYE_METAL_PIPELINE_PROBE=1` | Exercise pipeline-probe diagnostics |
| `GOLDENEYE_METAL_HOST_RT_SOLID_TEST=1` | Run the controlled solid render-target test |
| `GOLDENEYE_METAL_MAGENTA_RESOLVE=1` | Run the controlled resolve visibility test |

The `periodic` and `menu` auto-start modes use `steady_clock`, so renderer speed and input polling
frequency do not move the injected Start edges. They hold Start from 200 ms until 1200 ms, release
until 2000 ms, then emit a 250 ms pulse every 6000 ms. `periodic` continues that schedule;
`menu` stops all injection at 19 seconds, before the fourth pulse would begin at 20 seconds, so a
reached dossier menu gets an unforced capture and profiling window. The title may still leave an
idle menu on its own; this diagnostic only controls the injected input.
Both modes change input only: they do not replace rendering, alter PM4, force presentation, or
bypass resolves. Pair either mode with a numeric `GOLDENEYE_METAL_DUMP_FRAMES` value when
collecting one known presenter checkpoint, or use `1`/`all` only when every diagnostic stage is
needed. Keep all resulting game-content captures outside the repository.

Pair `GOLDENEYE_AUTO_START=menu` with `GOLDENEYE_AUTO_MISSION=dam` for the deterministic first-Dam
route. After waiting 22 seconds for the dossier, the mission injector contributes five ordinary A
presses, each followed by a release edge: Select Mission, Dam, Agent, open briefing, and Start
mission. Between the fourth and fifth A presses it contributes full positive left-stick Y for
450 ms, observes a neutral poll, and allows 750 ms for cursor focus to settle. Contributions merge
with the current controller state, never clear native input, and become permanently idle after the
final A release. This is an input-only rendering/performance diagnostic, not physical-input proof.

`GOLDENEYE_METAL_PROFILE=1` records CPU-side duration and count aggregates without enabling verbose
renderer logging. Every 64 command swaps it reports draw, copy, inclusive swap, fallback texture
decode, categorized synchronization waits, and `WAIT_REG_MEM` cost. The wait ledger also reports
the hottest sampled address, reference, mask, and poll count so a fence wait can be tied back to
the observed guest condition. Every 64 presenter attempts it reports source reuse, drawable
failures, upload bytes, commits, and CPU time in drawable acquisition, upload, and present
submission. Only complete windows are emitted; present-submission time is not GPU completion or
display latency. When the flag is absent, the timers and presenter source hashing are bypassed.

The Metal presenter draws `FPS` by default after each completed guest front-buffer update. It
counts guest-delivered frames over a monotonic interval rather than CAMetalLayer paint events, so
window expose or repaint traffic cannot inflate the value. Set `REX_METAL_SHOW_FPS=false` to
disable the overlay.

Other host-pixel and fallback flags in the code are experiments. Results produced with them must be
labelled as diagnostics and must not be reported as strict-path rendering success.

## Metal submission and synchronization

Persistent producer draws use bounded asynchronous command-buffer submission. A probe context
batches up to 64 draws in each Metal command buffer and has four upload arenas. When all four are
occupied, it retires the oldest command buffer instead of waiting for the newest; the independent
global retained-draw ceiling is 2048. Normal ordering on the shared Metal command queue preserves
successive draws, uploads, and clears without a CPU wait between each operation.

Synchronization remains mandatory before a result is read or a resource may change underneath
queued GPU work. The runtime therefore drains pending contexts before readback, clear, target
resize or release, resolve/swap, cache clear, and shutdown. Shared-memory uploads, guest CPU/GPU
write commits, texture replacement, and shaders with memory-export side effects also fence through
the command processor. Do not remove one of these boundaries to improve a microbenchmark; validate
ownership first.

Private render-target reads in the CPU fallback use a reusable staging buffer and copy only the
requested rectangle. The observed title frame's three resolve bands therefore transfer 256, 256,
and 208 rows rather than independently reading the full 720-line target; the sequence's remaining
full-frame readback stays 1280x720. The default GPU tiled resolve writes the guest destination
directly. Clear operations needed by the same sequence are queued in order on the context instead
of forcing a separate CPU wait.

Complete, top-origin, full-width resolves can populate an exact resolved-surface cache. A swap may
reuse its host BGRA pixels only when the fetch base, pitch, dimensions, endian mode, and live guest
tiled bytes still match. External guest writes and overlapping resolve writes invalidate the
cache, and the final byte comparison catches writes that bypass normal tracking. Both the mirror
and dirty-range commits use the true tiled-address upper bound rather than a linear
`pitch * height * 4` estimate; the latter is too small for the bottom of a 1280x720 tiled surface.

Coarse guest-memory invalidation may cover 256 KiB even when the resident texture bytes did not
change. The texture cache therefore fingerprints the synchronized source span before decoding and
uploading it again. An unchanged fingerprint re-arms tracking without another CPU untile or Metal
`replaceRegion`; a changed source still follows the normal replacement path.

The first Dam transition exposed a tiled 8192x8191 texture descriptor whose 256 MiB source extent
started too near the end of the 512 MiB guest physical range. The texture cache rejected it, but
the direct diagnostic/probe decoder previously translated the pointer and entered `Untile` without
the cache's range gate. That path now checks dimensions, pitch, every multiplication and addition,
the exact tiled or linear source span, host-size conversions, and the output allocation before any
guest-pointer translation, scan, copy, or untile. Invalid bindings fall back to the dummy texture;
the standalone validation test preserves the observed descriptor as a regression target.

MSL resource reflection is computed once after a translation's final source sanitization. Buffer
indices, texture-fetch mappings, interpolator locations, memory-export state, and void-fragment
state are then read from the immutable translation record instead of rescanning MSL for every
draw. Per-draw constants, CPU vertex data, and nonresident index data are suballocated from
reusable upload arenas associated with their command buffer. Arenas are recycled only after GPU
completion, preserving asynchronous resource lifetime without allocating a Metal buffer for every
binding.

The GPU compute path writes the Xenos tiled destination directly and is enabled by default.
Byte-exact coverage includes eight full and partial rectangle cases across all four 128-bit endian
modes, and a live Dam proving run completed thousands of resolves with zero fallbacks. Set
`GOLDENEYE_METAL_GPU_TILED_RESOLVE=0` only when comparing the CPU fallback, and record the opt-out
in performance results. Further work should make resolve submission more asynchronous and reduce
the fence wait around its consumers.

Persistent Metal render contexts own private `Depth32Float_Stencil8` attachments. Normalized
`RB_DEPTHCONTROL`, `RB_STENCILREFMASK`, and `RB_STENCILREFMASK_BF` state drives depth compare/write
and independent front/back stencil compare, operations, masks, and references. Attachment
load/store ordering follows the existing queued render context, and resize/reset/release drains
pending work before changing its lifetime.

This is intentionally a bounded fidelity slice. Depth attachments are still per host color-target
context rather than shared by guest `RB_DEPTH_INFO.depth_base` / EDRAM identity. Guest `D24S8` and
`D24FS8` both use Metal 32-bit float depth, and guest depth clear, copy, resolve, readback,
texture-alias, MSAA, sample-mask, depth-bias, and non-solid-fill behavior remain unimplemented.

The title-facing GPU-wait hook determines completion from one atomic primary-ring snapshot. A ring
is drained only when it is configured, has a valid write pointer, and has no pending commands;
this also handles a valid wrapped write pointer of zero without mistaking it for an uninitialized
ring.

The guest D3D watchdog uses a hardware-scale timeout that is shorter than host scheduling and Metal
pipeline compilation. A single drained snapshot is not a safe release point because the producer
may append the next primary-ring tail immediately afterward. The title hook therefore requires the
same wait context and heartbeat to observe a continuously drained ring for 60 ms before restoring
accumulated guest watchdog time. Any pending sample or heartbeat change restarts that grace. This
does not acknowledge work: the title still owns its fence, skip bits, and presented counter, and
the command processor still owns RPTR. While commands remain pending, held watchdog time is allowed
only while RPTR continues to change; 30 seconds without RPTR progress restores the title's genuine
hang detection. A 74-second proving run reached swap 4416 with zero guest GPU-hang dumps or debug
traps, versus 14 of each in the pre-fix control.

Rate-limited `IssueSwap` output includes cumulative asynchronous submission, wait, waited-command,
and maximum-pending counters. Use those counters with a fixed swap checkpoint when comparing
pacing, and leave frame/shader dumps and verbose diagnostics disabled during timing runs.

The POSIX host tick count is represented in nanoseconds. Its frequency is therefore fixed at one
billion ticks per second rather than derived from `clock_getres`, which reports the timer's
precision rather than the unit of the returned count. The former calculation scaled guest time and
the FPS display by the host timer quantum on macOS. With the corrected clock and performance
optimizations above, verified 1280x720 captures on an Apple M3 Ultra reach the dossier and a clean
real Dam briefing at roughly 60 FPS. An earlier dynamic Dam proving window measured 29.68 FPS.
Oldest-first upload command-buffer retirement and a 2048 global retained-draw ceiling then
produced a correct 46.5 FPS screenshot while reducing draw time from roughly 10 ms to 7.2–7.5 ms
per swap. A later correct Dam screenshot displayed 60.0 FPS; stable complete windows reported
roughly 6.4–6.7 ms draw, 4.2–4.35 ms copy, 1.74 ms swap, and 2.9 ms `WAIT_REG_MEM` time per swap.
Not every Dam view sustains 60 FPS, so the primary performance target is broader-scene stability
through asynchronous resolve work and lower fence-wait cost, followed by live physical
keyboard/mouse gameplay validation.

## Debugging guardrails

- Record the exact binary, build type, environment variables, and command stream being tested.
- Change one provenance assumption at a time.
- Prefer counters and bounded logs over unconditional output.
- Keep dumps outside the repository and never share captures containing proprietary game data.
- Confirm whether a pixel came from the real producer target before debugging the final swap copy.
- Track guest VdSwap calls, CP write-pointer progress, and `IssueSwap` separately.
- Do not advance title-owned fences or ring pointers from a wall-clock timeout.

## Formatting

The repository includes `.clang-format`. Format only touched files where practical:

```sh
clang-format -i path/to/touched_file.cpp path/to/touched_file.h
```
