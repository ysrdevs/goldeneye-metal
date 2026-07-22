// ge - project mid-ASM hooks: 0x830E0xxx fragment reconstruction.
#include <thread>
//
// 8 functions branch to 0x830E0xxx, ZERO in the static XEX (rexglue codegen
// stubs it) but at runtime real PPC code (identical to fragments IDA
// mis-coalesced into sub_821A9720). codegen prunes the code after the
// unconditional `b 0x830E0xxx`, so each continuation point is declared as its
// own ge_cont_* function. Each [[midasm_hook]] (return = true) replicates the
// fragment's register/memory effect, tail-invokes the continuation function,
// and the recompiled source function then returns.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <mutex>

#include "ge_init.h"   // PPCRegister/PPCContext + generated function decls
#include <rex/cvar.h>  // REXCVAR_* (mouse-look settings)
#include <rex/input/input_system.h>
#include <rex/ui/keybinds.h>  // ParseVirtualKey (keyboard rebinding)
#include <rex/ui/virtual_key.h>
#include <rex/hook.h>  // ThreadState, kernel_state, memory
#include <rex/runtime.h>
#include <rex/system/xmemory.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/command_processor.h>
#include <rex/system/xthread.h>
#include <rex/system/kernel_state.h>
#include <cstdio>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>  // ShellExecuteW (WIN32_LEAN_AND_MEAN excludes it)
#else
#include <limits.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif
#include <string>
#include <string_view>

#include "ge_crash_guards.h"
#include "ge_host_pause.h"
#include "ge_testing_tools.h"

namespace ge {
// Relaunch this same executable as a fresh, detached process. Used by the ONLINE
// pause-menu tab's "Save & Restart": the new instance reads the just-written
// configuration (new username / server / online-enable) at boot, then the caller tears
// the current process down. Launching a second instance of a running exe is fine
// on Windows -- the image file is opened share-read.
void LaunchSelfDetached() {
#if defined(_WIN32)
  wchar_t exe_path[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) {
    return;  // can't resolve our own path; skip relaunch (caller still quits)
  }
  // Start it from the exe's own directory so a normal boot's relative paths hold.
  std::wstring full(exe_path, exe_path + n);
  size_t slash = full.find_last_of(L"\\/");
  std::wstring workdir = (slash == std::wstring::npos) ? std::wstring() : full.substr(0, slash);
  ShellExecuteW(nullptr, L"open", exe_path, nullptr, workdir.empty() ? nullptr : workdir.c_str(),
                SW_SHOWNORMAL);
#else
  char exe_path[PATH_MAX] = {};
#if defined(__APPLE__)
  uint32_t size = sizeof(exe_path);
  if (_NSGetExecutablePath(exe_path, &size) != 0) {
    return;
  }
#elif defined(__linux__)
  ssize_t n = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
  if (n <= 0) {
    return;
  }
  exe_path[n] = '\0';
#else
  return;
#endif
  char resolved[PATH_MAX] = {};
  const char* launch_path = realpath(exe_path, resolved) ? resolved : exe_path;
  std::string full(launch_path);
  size_t slash = full.find_last_of('/');
  std::string workdir = (slash == std::string::npos) ? std::string() : full.substr(0, slash);

  pid_t pid = fork();
  if (pid == 0) {
    if (!workdir.empty()) {
      chdir(workdir.c_str());
    }
    execl(full.c_str(), full.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
#endif
}
}  // namespace ge

namespace {
bool EnvironmentFlagEnabled(const char* name) {
  const char* value = std::getenv(name);
  return value && value[0] != '\0' && !(value[0] == '0' && value[1] == '\0');
}

// rexglue CP swap counter sampled at the last guest present (sub_821996F8).
// This is diagnostic state only: XE_SWAP may execute before trailing primary-ring
// packets retire, so a counter advance is not a completion fence.
std::atomic<uint32_t> g_present_cpcnt{0};
std::atomic<uint64_t> g_gpu_wait_blocked_polls{0};
std::atomic<uint64_t> g_gpu_wait_completed_polls{0};
std::atomic<uint64_t> g_gpu_wait_drain_grace_polls{0};
std::atomic<uint64_t> g_gpu_wait_drain_grace_starts{0};
std::atomic<uint64_t> g_gpu_wait_stable_drain_releases{0};
std::atomic<uint64_t> g_gpu_wait_pending_progress_resets{0};
std::atomic<uint64_t> g_gpu_wait_pending_stall_releases{0};
inline rex::graphics::CommandProcessor* ge_cp() {
  auto* ks = rex::system::kernel_state();
  if (!ks)
    return nullptr;
  auto* rt = ks->emulator();
  if (!rt)
    return nullptr;
  auto* igs = rt->graphics_system();
  if (!igs)
    return nullptr;
  return static_cast<rex::graphics::GraphicsSystem*>(igs)->command_processor();
}
inline rex::graphics::GraphicsSystem* ge_gs() {
  auto* ks = rex::system::kernel_state();
  if (!ks)
    return nullptr;
  auto* rt = ks->emulator();
  if (!rt)
    return nullptr;
  auto* igs = rt->graphics_system();
  if (!igs)
    return nullptr;
  return static_cast<rex::graphics::GraphicsSystem*>(igs);
}

constexpr auto kGpuWatchdogStableDrainGrace = std::chrono::milliseconds(60);
// Guest watchdog ticks are hardware-scale and can expire during ordinary host
// scheduling or synchronous shader compilation. Keep them frozen while the CP
// read pointer is moving, but never suppress a genuinely stuck pending ring
// forever. Thirty seconds is intentionally conservative for cold Metal work.
constexpr auto kGpuWatchdogPendingStallLimit = std::chrono::seconds(30);

struct GpuWatchdogDrainGraceState {
  uint32_t wait_context = 0;
  uint32_t heartbeat_token = 0;
  uint32_t pending_read_pointer = 0;
  std::chrono::steady_clock::time_point drained_since{};
  std::chrono::steady_clock::time_point pending_progress_at{};
  bool observing_drain = false;
  bool observing_pending = false;
  bool drain_release_recorded = false;
  bool pending_release_recorded = false;
};

constexpr bool ge_hold_gpu_watchdog_time(bool command_processor_available,
                                         bool primary_ring_drained, bool stable_drain_grace_elapsed,
                                         bool pending_stall_limit_elapsed) {
  if (!command_processor_available) {
    return false;
  }
  return primary_ring_drained ? !stable_drain_grace_elapsed : !pending_stall_limit_elapsed;
}
static_assert(ge_hold_gpu_watchdog_time(true, false, false, false));
static_assert(ge_hold_gpu_watchdog_time(true, false, true, false));
static_assert(!ge_hold_gpu_watchdog_time(true, false, false, true));
static_assert(ge_hold_gpu_watchdog_time(true, true, false, false));
static_assert(!ge_hold_gpu_watchdog_time(true, true, true, false));
static_assert(!ge_hold_gpu_watchdog_time(false, false, false, false));
}  // namespace

namespace {
constexpr uint32_t kGuestLowMemoryStart = 0x10000u;
constexpr uint32_t kGuestLowMemoryEnd = 0x70000000u;

inline bool guest_low_range_valid(uint32_t ga, uint32_t length) {
  return ga >= kGuestLowMemoryStart && ga < kGuestLowMemoryEnd &&
         length <= (kGuestLowMemoryEnd - ga);
}

inline void getcb(PPCContext*& ctx, uint8_t*& base) {
  ctx = rex::runtime::ThreadState::Get()->context();
  base = rex::system::kernel_state()->memory()->virtual_membase();
}
inline uint32_t LD32(uint8_t* b, uint32_t ga) {
  uint32_t v;
  std::memcpy(&v, b + ga, 4);
  return __builtin_bswap32(v);
}
inline uint64_t LD64(uint8_t* b, uint32_t ga) {
  uint64_t v;
  std::memcpy(&v, b + ga, 8);
  return __builtin_bswap64(v);
}
inline void ST32(uint8_t* b, uint32_t ga, uint32_t val) {
  uint32_t v = __builtin_bswap32(val);
  std::memcpy(b + ga, &v, 4);
}
inline void STF32(uint8_t* b, uint32_t ga, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  v = __builtin_bswap32(v);
  std::memcpy(b + ga, &v, 4);
}
inline float LDF32(uint8_t* b, uint32_t ga) {
  uint32_t v;
  std::memcpy(&v, b + ga, 4);
  v = __builtin_bswap32(v);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
inline uint16_t LD16(uint8_t* b, uint32_t ga) {
  uint16_t v;
  std::memcpy(&v, b + ga, 2);
  return __builtin_bswap16(v);
}
inline void ST16(uint8_t* b, uint32_t ga, uint16_t val) {
  uint16_t v = __builtin_bswap16(val);
  std::memcpy(b + ga, &v, 2);
}

void ge_sample_present_main_thread_path(uint8_t* base, uint32_t present_index) {
  auto* ks = rex::system::kernel_state();
  if (!ks)
    return;
  auto threads = ks->object_table()->GetObjectsByType<rex::system::XThread>();
  for (auto& th : threads) {
    if (!th || th->creation_params()->start_address != 0x8235E4A8u)
      continue;
    auto* ts = th->thread_state();
    if (!ts)
      continue;
    auto* mc = ts->context();
    if (!mc)
      continue;

    uint32_t seen[64];
    int seen_count = 0;
    for (int it = 0; it < 1600 && seen_count < 62; ++it) {
      uint32_t pc = static_cast<uint32_t>(mc->lr);
      if (pc >= 0x82000000u && pc < 0x84000000u) {
        bool duplicate = false;
        for (int j = 0; j < seen_count; ++j) {
          if (seen[j] == pc) {
            duplicate = true;
            break;
          }
        }
        if (!duplicate)
          seen[seen_count++] = pc;
      }
      std::this_thread::yield();
    }

    char path_buffer[520];
    int path_offset = 0;
    path_buffer[0] = 0;
    for (int j = 0; j < seen_count && path_offset < 480; ++j) {
      int n = std::snprintf(path_buffer + path_offset, sizeof(path_buffer) - path_offset, "%x ",
                            seen[j]);
      if (n > 0)
        path_offset += n;
    }
    std::fprintf(stderr, "[ge] GEPATH present#%u main unique_lr=%d %s\n", present_index, seen_count,
                 path_buffer);

    int logged = 0;
    for (int snap = 0; snap < 40 && logged < 4; ++snap) {
      uint32_t pc = static_cast<uint32_t>(mc->lr);
      bool in_limiter =
          (pc >= 0x823B3040u && pc <= 0x823B3540u) || (pc >= 0x82189DC0u && pc <= 0x82189E14u);
      if (!in_limiter && pc >= 0x82000000u && pc < 0x84000000u) {
        uint32_t sp = mc->r1.u32;
        char stack_buffer[520];
        int stack_offset =
            std::snprintf(stack_buffer, sizeof(stack_buffer), "lr=%x sp=%x | ", pc, sp);
        if (guest_low_range_valid(sp, 4)) {
          uint8_t* stack = base + sp;
          uint32_t scan_length = std::min<uint32_t>(0x1800u, kGuestLowMemoryEnd - sp);
          uint8_t* stack_end = stack + scan_length;
          for (uint8_t* pp = stack; pp + 4 <= stack_end && stack_offset < 480; pp += 4) {
            uint32_t value;
            std::memcpy(&value, pp, 4);
            value = __builtin_bswap32(value);
            if (value >= 0x82000000u && value < 0x84000000u) {
              int n = std::snprintf(stack_buffer + stack_offset,
                                    sizeof(stack_buffer) - stack_offset, "%x ", value);
              if (n > 0)
                stack_offset += n;
            }
          }
        }
        std::fprintf(stderr, "[ge] GEPATH present#%u framepath#%d %s\n", present_index, logged,
                     stack_buffer);
        ++logged;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    std::fflush(stderr);
    break;
  }
}
}  // namespace

// ===========================================================================
// Freeze watchdog. Auto-detects the visual freeze (the guest keeps presenting
// -- present# advancing -- but the GPU command ring stops advancing) and logs
// the exact pipeline state once per stall episode. This is observation-only;
// synchronization state remains owned by the title and command processor.
// ===========================================================================
namespace {
std::atomic<uint32_t> g_ge_device{0};     // device struct (dev) seen by ge_dbg_now
std::atomic<uint32_t> g_ge_idblk{0};      // id-block (idblk) seen by ge_dbg_now
std::atomic<uint32_t> g_dbgnow_calls{0};  // increments each ge_dbg_now (guest polling sub_82198C28)

void ge_watchdog_thread() {
  uint8_t* base = rex::system::kernel_state()->memory()->virtual_membase();
  uint32_t last_wpi = 0xFFFFFFFFu, last_rpi = 0, last_present = 0, last_submit = 0;
  uint32_t present_at_stall_start = 0, dbg_at_stall_start = 0, submit_at_stall_start = 0;
  uint32_t no_present_at_stall_start = 0, no_present_dbg_at_stall_start = 0,
           no_present_submit_at_stall_start = 0;
  uint32_t stall = 0;
  uint32_t no_present_stall = 0;
  bool logged = false;
  bool no_present_logged = false;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    auto* cp = ge_cp();
    if (!cp)
      continue;
    uint32_t wpi = cp->write_ptr_index();
    uint32_t rpi = cp->read_ptr_index();
    uint32_t present = g_present_cpcnt.load(std::memory_order_relaxed);
    uint32_t dbg = g_dbgnow_calls.load(std::memory_order_relaxed);
    uint32_t dev = g_ge_device.load(std::memory_order_relaxed);
    uint32_t idblk = g_ge_idblk.load(std::memory_order_relaxed);
    uint32_t submit = dev ? LD32(base, dev + 16544) : 0;

    bool present_alive = (present != last_present);
    bool ring_moved = (wpi != last_wpi) || (rpi != last_rpi);
    if (!present_alive && present >= 8) {
      if (no_present_stall == 0) {
        no_present_at_stall_start = present;
        no_present_dbg_at_stall_start = dbg;
        no_present_submit_at_stall_start = submit;
      }
      ++no_present_stall;
      if (no_present_stall >= 24 && !no_present_logged) {  // ~6s of no presents
        no_present_logged = true;
        uint32_t rg = LD32(base, 0x8242043Cu);
        uint32_t presented = dev ? LD32(base, dev + 16552) : 0;
        uint32_t target = dev ? LD32(base, dev + 10908) : 0;
        uint32_t completed = idblk ? LD32(base, idblk + 0) : 0;
        REXKRNL_INFO(
            "GENOPRESENT STALL: ring rpi={:#x} wpi={:#x} [{}] | present#={} (+{}/stall) | "
            "dbgnow_polls={} (+{}/stall) | submit={} (+{}/stall) completed={} target={} "
            "presented={} render_gate={:#x} | dev={:#x} idblk={:#x}",
            rpi, wpi, (rpi == wpi ? "DRAINED" : "PENDING"), present,
            present - no_present_at_stall_start, dbg, dbg - no_present_dbg_at_stall_start, submit,
            submit - no_present_submit_at_stall_start, completed, target, presented, rg, dev,
            idblk);
        std::fprintf(stderr,
                     "[ge] GENOPRESENT STALL rpi=0x%08x wpi=0x%08x %s present=%u(+%u) "
                     "dbg=%u(+%u) submit=%u(+%u) completed=%u target=%u presented=%u "
                     "render_gate=0x%08x dev=0x%08x idblk=0x%08x\n",
                     rpi, wpi, (rpi == wpi ? "DRAINED" : "PENDING"), present,
                     present - no_present_at_stall_start, dbg, dbg - no_present_dbg_at_stall_start,
                     submit, submit - no_present_submit_at_stall_start, completed, target,
                     presented, rg, dev, idblk);
        if (dev) {
          uint32_t f21516 = LD32(base, dev + 21516u);
          uint32_t f22280 = LD32(base, dev + 22280u);
          uint32_t f22276 = LD32(base, dev + 22276u);
          uint32_t f21604 = LD32(base, dev + 21604u);
          uint32_t f21600 = LD32(base, dev + 21600u);
          uint32_t b10941 = base[dev + 10941u];
          uint32_t b10943 = base[dev + 10943u];
          uint32_t vbl = LD32(base, dev + 16532u);
          uint32_t fr = LD32(base, dev + 16684u);
          uint32_t fw = LD32(base, dev + 16688u);
          REXKRNL_INFO(
              "GENOPRESENT -> devflags +21516(VdSwap-skip if !=0)={:#x} | +22280&4(gpu-wait)={} | "
              "+22276={:#x} | +21604={} +21600={} (ring) | +10941={:#x} +10943={:#x}",
              f21516, (f22280 & 4u), f22276, f21604, f21600, b10941, b10943);
          REXKRNL_INFO("GENOPRESENT -> vblank ctx[4133]={} | GPU fences read={} write={} [{}]", vbl,
                       fr, fw, (fr != fw ? "PENDING -- fences NOT retiring" : "drained"));
          std::fprintf(stderr,
                       "[ge] GENOPRESENT devflags +21516=0x%08x +22280&4=%u +22276=0x%08x "
                       "+21604=%u +21600=%u +10941=0x%02x +10943=0x%02x vblank=%u "
                       "fence_read=%u fence_write=%u %s\n",
                       f21516, f22280 & 4u, f22276, f21604, f21600, b10941, b10943, vbl, fr, fw,
                       (fr != fw ? "PENDING" : "drained"));
        }
        auto* ks2 = rex::system::kernel_state();
        if (ks2) {
          auto threads = ks2->object_table()->GetObjectsByType<rex::system::XThread>();
          for (auto& th : threads) {
            if (!th)
              continue;
            auto* ts = th->thread_state();
            if (!ts)
              continue;
            auto* c = ts->context();
            if (!c)
              continue;
            uint32_t sa = th->creation_params()->start_address;
            bool rw = (sa == 0x821A4A68u);
            REXKRNL_INFO(
                "GENOPRESENT THREAD start={:#x}{} lr={:#x} ctr={:#x} lastIndTgt={:#x} msr={:#x} | "
                "r1={:#x} r3={:#x} r11={:#x} r28={:#x} r29={:#x} r30={:#x} r31={:#x}",
                sa, rw ? " [RENDER-WORKER]" : "", (uint32_t)c->lr, c->ctr.u32,
                c->last_indirect_target, c->msr, c->r1.u32, c->r3.u32, c->r11.u32, c->r28.u32,
                c->r29.u32, c->r30.u32, c->r31.u32);
            std::fprintf(stderr,
                         "[ge] GENOPRESENT THREAD start=0x%08x%s lr=0x%08x ctr=0x%08x "
                         "last=0x%08x msr=0x%08x r1=0x%08x r3=0x%08x r11=0x%08x "
                         "r28=0x%08x r29=0x%08x r30=0x%08x r31=0x%08x\n",
                         sa, rw ? " [RENDER-WORKER]" : "", (uint32_t)c->lr, c->ctr.u32,
                         c->last_indirect_target, c->msr, c->r1.u32, c->r3.u32, c->r11.u32,
                         c->r28.u32, c->r29.u32, c->r30.u32, c->r31.u32);
            uint32_t sp = c->r1.u32;
            if (guest_low_range_valid(sp, 4)) {
              uint8_t* hsp = base + sp;
              uint32_t scan_length = std::min<uint32_t>(0x2400u, kGuestLowMemoryEnd - sp);
              char sbuf[500];
              int soff = 0;
              sbuf[0] = 0;
              for (uint8_t* pp = hsp; pp + 4 <= hsp + scan_length && soff < 460; pp += 4) {
                uint32_t val;
                std::memcpy(&val, pp, 4);
                val = __builtin_bswap32(val);
                if (val >= 0x82000000u && val < 0x84000000u) {
                  int n = std::snprintf(sbuf + soff, sizeof(sbuf) - soff, "%x ", val);
                  if (n > 0)
                    soff += n;
                }
              }
              REXKRNL_INFO("GENOPRESENT   STACK start={:#x} sp={:#x}: {}", sa, sp, sbuf);
            }
            if (rw) {
              uint32_t bw = c->r28.u32;
              auto sLD = [&](uint32_t ga) -> uint32_t {
                return (ga >= 0x1000u && ga < 0x50000000u) ? LD32(base, ga) : 0xDEADBEEFu;
              };
              uint32_t v3 = sLD(bw);
              uint32_t sig = sLD(bw + 4);
              uint32_t v3f = sLD(v3 + 368);
              uint32_t subq = sLD(bw + 0x38);
              uint32_t procq = sLD(bw + 0x3C);
              REXKRNL_INFO(
                  "GENOPRESENT   WORKER a1={:#x} queue Flink/submit={} Blink/proc={} [{}] | "
                  "SignalState={} v3={:#x} *(v3+368)={} -> wait={}",
                  bw, subq, procq,
                  (subq == procq ? "EMPTY (producer stopped feeding)" : "PENDING (LOST WAKEUP!)"),
                  sig, v3, v3f, (sig != v3f ? "INFINITE" : "30ms-timeout"));
              std::fprintf(stderr,
                           "[ge] GENOPRESENT WORKER a1=0x%08x queue_submit=%u queue_proc=%u %s "
                           "SignalState=%u v3=0x%08x v3_368=%u wait=%s\n",
                           bw, subq, procq, (subq == procq ? "EMPTY" : "PENDING"), sig, v3, v3f,
                           (sig != v3f ? "INFINITE" : "30ms-timeout"));
            }
          }
        }
        std::fflush(stderr);
      }
    } else {
      no_present_stall = 0;
      no_present_logged = false;
    }
    if (present_alive && !ring_moved) {
      if (stall == 0) {
        present_at_stall_start = present;
        dbg_at_stall_start = dbg;
        submit_at_stall_start = submit;
      }
      ++stall;
      if (stall >= 6 && !logged) {  // ~1.5s of present-but-no-ring
        logged = true;
        uint32_t presented = dev ? LD32(base, dev + 16552) : 0;
        uint32_t target = dev ? LD32(base, dev + 10908) : 0;
        uint32_t completed = idblk ? LD32(base, idblk + 0) : 0;
        uint32_t skip = dev ? (base[dev + 10941] & 2) : 0;
        REXKRNL_INFO(
            "GEWATCHDOG STALL: ring rpi={:#x} wpi={:#x} [{}] | present#={} (+{}/stall) | "
            "dbgnow_polls={} (+{}/stall) | submit={} completed={} target={} presented={} "
            "skipbit={} "
            "| dev={:#x} idblk={:#x}",
            rpi, wpi, (rpi == wpi ? "DRAINED" : "PENDING"), present,
            present - present_at_stall_start, dbg, dbg - dbg_at_stall_start, submit, completed,
            target, presented, skip, dev, idblk);
        REXKRNL_INFO(
            "GEWATCHDOG -> completion={} | presenting={} | producer={} | polling={}",
            (submit > completed ? "GPU BEHIND (completion not delivered)" : "caught up"),
            (submit > presented ? "frames NOT presenting" : "caught up"),
            (submit != submit_at_stall_start ? "ALIVE (submitting)" : "STALLED (not submitting)"),
            (dbg != dbg_at_stall_start ? "guest spinning in sub_82198C28" : "guest NOT polling"));
        // Render gate: frame loop runs render+present only when dword_8242043C&2
        // (sub_8209E1C0). Set by sub_8209E1D0(mode): mode 3 at init (enabled),
        // mode 1 = bit clear = render skipped every frame = freeze.
        uint32_t rg = LD32(base, 0x8242043Cu);
        REXKRNL_INFO("GEWATCHDOG -> render-gate dword_8242043C={} -> render+present {}", rg,
                     (rg & 2u) ? "ENABLED" : "DISABLED (frame loop skips render = FREEZE)");
        // Device flags gating the present/submit (a1 = dev). +21516 != 0 => the
        // present SKIPS VdSwap (no screen update) and sub_821A4D50 takes its alt
        // path; +22280&4 gates the GPU-completion wait; +10941/+10943 = skip bits.
        if (dev) {
          uint32_t f21516 = LD32(base, dev + 21516u);
          uint32_t f22280 = LD32(base, dev + 22280u);
          uint32_t f22276 = LD32(base, dev + 22276u);
          uint32_t f21604 = LD32(base, dev + 21604u);
          uint32_t f21600 = LD32(base, dev + 21600u);
          uint32_t b10941 = base[dev + 10941u];
          uint32_t b10943 = base[dev + 10943u];
          REXKRNL_INFO(
              "GEWATCHDOG -> devflags +21516(VdSwap-skip if !=0)={:#x} | +22280&4(gpu-wait)={} | "
              "+22276={:#x} | +21604={} +21600={} (ring) | +10941={:#x} +10943={:#x}",
              f21516, (f22280 & 4u), f22276, f21604, f21600, b10941, b10943);
          uint32_t vbl = LD32(base, dev + 16532u);  // ctx[4133] vblank count
          uint32_t fr = LD32(base, dev + 16684u);   // ctx[4171] fence read idx
          uint32_t fw = LD32(base, dev + 16688u);   // ctx[4172] fence write idx
          REXKRNL_INFO("GEWATCHDOG -> vblank ctx[4133]={} | GPU fences read={} write={} [{}]", vbl,
                       fr, fw, (fr != fw ? "PENDING -- fences NOT retiring" : "drained"));
        }
        // Frame counter dword_8308851C is updated each frame AFTER the frame-
        // limiter (0x82189e64). Sample it twice: if FROZEN, the main thread never
        // exits the frame-limiter (clock/timebase not advancing for it); if it
        // ADVANCES, the main thread cycles and the render is skipped after.
        uint32_t fc1 = LD32(base, 0x8308851Cu);
        uint32_t tb1 = (uint32_t)REX_QUERY_TIMEBASE();
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        uint32_t fc2 = LD32(base, 0x8308851Cu);
        uint32_t tb2 = (uint32_t)REX_QUERY_TIMEBASE();
        REXKRNL_INFO(
            "GEWATCHDOG -> frameCounter 0x8308851C {}->{} [{}] | guestTimebase {}->{} [{}]", fc1,
            fc2,
            (fc1 != fc2 ? "ADVANCING (main thread cycles; render skipped after limiter)"
                        : "FROZEN (main thread STUCK in frame-limiter)"),
            tb1, tb2, (tb1 != tb2 ? "advancing" : "FROZEN"));
        // Dump every guest thread's jump state -- find WHERE the render workers
        // (guest entry 0x821A4A68) are wedged inside sub_821A4750. lr = return
        // addr, ctr = next indirect target, lastIndTgt = last REX_CALL_INDIRECT
        // target, msr bit 0x8000 = interrupts enabled.
        auto* ks2 = rex::system::kernel_state();
        if (ks2) {
          auto threads = ks2->object_table()->GetObjectsByType<rex::system::XThread>();
          for (auto& th : threads) {
            if (!th)
              continue;
            auto* ts = th->thread_state();
            if (!ts)
              continue;
            auto* c = ts->context();
            if (!c)
              continue;
            uint32_t sa = th->creation_params()->start_address;
            bool rw = (sa == 0x821A4A68u);
            REXKRNL_INFO(
                "GEWATCHDOG THREAD start={:#x}{} lr={:#x} ctr={:#x} lastIndTgt={:#x} msr={:#x} | "
                "r3={:#x} r11={:#x} r28={:#x} r29={:#x} r30={:#x} r31={:#x}",
                sa, rw ? " [RENDER-WORKER]" : "", (uint32_t)c->lr, c->ctr.u32,
                c->last_indirect_target, c->msr, c->r3.u32, c->r11.u32, c->r28.u32, c->r29.u32,
                c->r30.u32, c->r31.u32);
            // Guest stack walk: scan [r1, r1+0x2400) for guest code addresses
            // (0x82xxxxxx return addresses) -> the call chain, directly readable.
            {
              uint32_t sp = c->r1.u32;
              if (guest_low_range_valid(sp, 4)) {
                uint8_t* hsp = base + sp;
                uint32_t scan_length = std::min<uint32_t>(0x2400u, kGuestLowMemoryEnd - sp);
                uint8_t* send = hsp + scan_length;
#if defined(_WIN32)
                MEMORY_BASIC_INFORMATION mbi;
                if (VirtualQuery(hsp, &mbi, sizeof(mbi)) == sizeof(mbi) &&
                    mbi.State == MEM_COMMIT &&
                    (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                    PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) != 0) {
                  uint8_t* rend = static_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
                  if (send > rend)
                    send = rend;  // never read past the committed page
#endif
                  char sbuf[500];
                  int soff = 0;
                  sbuf[0] = 0;
                  for (uint8_t* pp = hsp; pp + 4 <= send && soff < 460; pp += 4) {
                    uint32_t val;
                    std::memcpy(&val, pp, 4);
                    val = __builtin_bswap32(val);
                    if (val >= 0x82000000u && val < 0x84000000u) {
                      int n = std::snprintf(sbuf + soff, sizeof(sbuf) - soff, "%x ", val);
                      if (n > 0)
                        soff += n;
                    }
                  }
                  REXKRNL_INFO("GEWATCHDOG   STACK start={:#x} sp={:#x}: {}", sa, sp, sbuf);
#if defined(_WIN32)
                }
#endif
              }
            }
            if (rw) {
              // a1 (worker struct) = r28; event = a1[2] = a1+0x20 (= r29);
              // queue = a1[3]: Flink/submit = a1+0x38, Blink/processed = a1+0x3C.
              // wait is INFINITE when a1->SignalState(a1+4) != *(v3+368), v3 = *a1.
              uint32_t bw = c->r28.u32;
              auto sLD = [&](uint32_t ga) -> uint32_t {
                return (ga >= 0x1000u && ga < 0x50000000u) ? LD32(base, ga) : 0xDEADBEEFu;
              };
              uint32_t v3 = sLD(bw);
              uint32_t sig = sLD(bw + 4);
              uint32_t v3f = sLD(v3 + 368);
              uint32_t subq = sLD(bw + 0x38);
              uint32_t procq = sLD(bw + 0x3C);
              REXKRNL_INFO(
                  "GEWATCHDOG   WORKER a1={:#x} queue Flink/submit={} Blink/proc={} [{}] | "
                  "SignalState={} v3={:#x} *(v3+368)={} -> wait={}",
                  bw, subq, procq,
                  (subq == procq ? "EMPTY (producer stopped feeding)" : "PENDING (LOST WAKEUP!)"),
                  sig, v3, v3f, (sig != v3f ? "INFINITE" : "30ms-timeout"));
            }
          }
          // Rapid-sample the main game thread (start 0x8235e4a8): it spends most
          // time in the frame-limiter, so one snapshot misses the render path.
          // Sample lr many times (yielding so it keeps running) -> the set of
          // unique guest PCs = its per-frame code path, revealing which render
          // subsystem call it reaches/skips.
          for (auto& th : threads) {
            if (!th)
              continue;
            if (th->creation_params()->start_address != 0x8235E4A8u)
              continue;
            auto* ts = th->thread_state();
            if (!ts)
              continue;
            auto* mc = ts->context();
            if (!mc)
              continue;
            uint32_t seen[96];
            int ns = 0;
            for (int it = 0; it < 8000 && ns < 94; it++) {
              uint32_t pc = static_cast<uint32_t>(mc->lr);
              if (pc >= 0x82000000u && pc < 0x84000000u) {
                bool dup = false;
                for (int j = 0; j < ns; j++)
                  if (seen[j] == pc) {
                    dup = true;
                    break;
                  }
                if (!dup)
                  seen[ns++] = pc;
              }
              std::this_thread::yield();
            }
            char mb[760];
            int mo = 0;
            mb[0] = 0;
            for (int j = 0; j < ns && mo < 720; j++) {
              int n = std::snprintf(mb + mo, sizeof(mb) - mo, "%x ", seen[j]);
              if (n > 0)
                mo += n;
            }
            REXKRNL_INFO("GEWATCHDOG MAINPATH (unique lr x{}): {}", ns, mb);
            // Snapshot the main thread's full stack repeatedly with SLEEPS (no
            // spinning -> doesn't starve it, it keeps cycling). Log only snapshots
            // where it is OUTSIDE the frame-limiter -> in the per-frame render
            // path -> the render call chain + the skipped 3D-submit branch.
            {
              int logged = 0;
              for (int snap = 0; snap < 160 && logged < 12; snap++) {
                uint32_t pc = static_cast<uint32_t>(mc->lr);
                bool in_lim = (pc >= 0x823B3040u && pc <= 0x823B3540u) ||
                              (pc >= 0x82189DC0u && pc <= 0x82189E14u);
                if (!in_lim && pc >= 0x82000000u && pc < 0x84000000u) {
                  uint32_t sp = mc->r1.u32;
                  char fb[620];
                  int fo = std::snprintf(fb, sizeof(fb), "lr=%x | ", pc);
                  if (guest_low_range_valid(sp, 4)) {
                    uint8_t* hsp = base + sp;
                    uint32_t scan_length = std::min<uint32_t>(0x2800u, kGuestLowMemoryEnd - sp);
                    uint8_t* send = hsp + scan_length;
#if defined(_WIN32)
                    MEMORY_BASIC_INFORMATION mbi;
                    if (VirtualQuery(hsp, &mbi, sizeof(mbi)) == sizeof(mbi) &&
                        mbi.State == MEM_COMMIT) {
                      uint8_t* rend = static_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
                      if (send > rend)
                        send = rend;
#endif
                      for (uint8_t* pp = hsp; pp + 4 <= send && fo < 580; pp += 4) {
                        uint32_t v;
                        std::memcpy(&v, pp, 4);
                        v = __builtin_bswap32(v);
                        if (v >= 0x82000000u && v < 0x84000000u) {
                          int n = std::snprintf(fb + fo, sizeof(fb) - fo, "%x ", v);
                          if (n > 0)
                            fo += n;
                        }
                      }
#if defined(_WIN32)
                    }
#endif
                  }
                  REXKRNL_INFO("GEWATCHDOG FRAMEWORK[{}] {}", logged, fb);
                  logged++;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
              }
            }
            break;
          }
        }
      }
    } else {
      stall = 0;
      logged = false;
    }
    last_wpi = wpi;
    last_rpi = rpi;
    last_present = present;
    last_submit = submit;
  }
}

inline void ge_start_watchdog_once() {
  static std::atomic<bool> started{false};
  bool expected = false;
  if (started.compare_exchange_strong(expected, true)) {
    std::thread(ge_watchdog_thread).detach();
  }
}
}  // namespace

// NOTE: no frame-limiter / intro-wait hook. The post-intro freeze is a
// SYMPTOM of the rexglue GPU command-processor not consuming the ring (GPU
// hung -> game stops presenting -> guest time stops -> wait never clears).
// Any hook writing that shared time counter corrupts per-frame timing and
// slows the intros without fixing the GPU hang -> net-harmful, removed.
// Intros run at full speed; post-intro hits the rexglue GPU ceiling.

// sub_82198C28 frame-wait reads now = *(r9+0x58) (r9 = *(r13+0x100)), waits
// while (now - last) < 0x1388. That field is a hardware/kernel time the game
// only READS (no guest writer); rexglue never ticks it -> frozen at 0 ->
// infinite spin. Feed it the real guest tick clock (REX_QUERY_TIMEBASE, the
// same ~49.875MHz source mftb uses): write the live value to both the loaded
// register (so this iteration's compare sees it) and the memory field (so
// other readers/sub_8235EAA8 see a consistent advancing clock). (now-last)
// then measures real elapsed ticks exactly like console -> correct pacing.
void ge_dbg_now(PPCRegister& r9, PPCRegister& r30) {
  PPCContext* ctx;
  uint8_t* base;
  getcb(ctx, base);
  uint32_t t = (uint32_t)REX_QUERY_TIMEBASE();
  if (r9.u32)
    ST32(base, r9.u32 + 0x58, t);
  r30.u32 = t;

  uint32_t dev = ctx->r29.u32;
  uint32_t idblk = ctx->r11.u32;
  uint32_t ws = ctx->r31.u32;

  // Feed the freeze watchdog (stash device pointers, count polls, start thread).
  g_ge_device.store(dev, std::memory_order_relaxed);
  g_ge_idblk.store(idblk, std::memory_order_relaxed);
  g_dbgnow_calls.fetch_add(1, std::memory_order_relaxed);
  ge_start_watchdog_once();
  auto* cpp = ge_cp();
  bool primary_ring_drained = cpp && cpp->primary_ring_drained();
  uint32_t heartbeat_token = ws ? LD32(base, ws + 8u) : 0;
  auto host_now = std::chrono::steady_clock::now();
  thread_local GpuWatchdogDrainGraceState drain_grace;
  bool stable_drain_grace_elapsed = false;
  bool pending_stall_limit_elapsed = false;

  if (!cpp) {
    drain_grace = {};
  } else if (!primary_ring_drained) {
    // Any pending sample breaks continuity. A later drained sample must earn a
    // fresh grace period before the accumulated guest time is made visible.
    uint32_t read_pointer = cpp->read_ptr_index();
    bool pending_progressed = !drain_grace.observing_pending || drain_grace.wait_context != ws ||
                              drain_grace.heartbeat_token != heartbeat_token ||
                              drain_grace.pending_read_pointer != read_pointer;
    drain_grace.wait_context = ws;
    drain_grace.heartbeat_token = heartbeat_token;
    drain_grace.observing_drain = false;
    drain_grace.drain_release_recorded = false;
    if (pending_progressed) {
      drain_grace.pending_read_pointer = read_pointer;
      drain_grace.pending_progress_at = host_now;
      drain_grace.observing_pending = true;
      drain_grace.pending_release_recorded = false;
      g_gpu_wait_pending_progress_resets.fetch_add(1, std::memory_order_relaxed);
    }
    pending_stall_limit_elapsed =
        drain_grace.observing_pending &&
        host_now - drain_grace.pending_progress_at >= kGpuWatchdogPendingStallLimit;
    if (pending_stall_limit_elapsed && !drain_grace.pending_release_recorded) {
      drain_grace.pending_release_recorded = true;
      g_gpu_wait_pending_stall_releases.fetch_add(1, std::memory_order_relaxed);
    }
  } else {
    drain_grace.observing_pending = false;
    drain_grace.pending_release_recorded = false;
    bool wait_changed =
        drain_grace.wait_context != ws || drain_grace.heartbeat_token != heartbeat_token;
    if (wait_changed || !drain_grace.observing_drain) {
      drain_grace.wait_context = ws;
      drain_grace.heartbeat_token = heartbeat_token;
      drain_grace.drained_since = host_now;
      drain_grace.observing_drain = true;
      drain_grace.drain_release_recorded = false;
      g_gpu_wait_drain_grace_starts.fetch_add(1, std::memory_order_relaxed);
    }
    stable_drain_grace_elapsed =
        host_now - drain_grace.drained_since >= kGpuWatchdogStableDrainGrace;
    if (!stable_drain_grace_elapsed) {
      g_gpu_wait_drain_grace_polls.fetch_add(1, std::memory_order_relaxed);
    } else if (!drain_grace.drain_release_recorded) {
      drain_grace.drain_release_recorded = true;
      g_gpu_wait_stable_drain_releases.fetch_add(1, std::memory_order_relaxed);
    }
  }

  bool hold_watchdog_time =
      ge_hold_gpu_watchdog_time(cpp != nullptr, primary_ring_drained, stable_drain_grace_elapsed,
                                pending_stall_limit_elapsed);
  (hold_watchdog_time ? g_gpu_wait_blocked_polls : g_gpu_wait_completed_polls)
      .fetch_add(1, std::memory_order_relaxed);

  // Never let a swap-counter advance release this watchdog. XE_SWAP may be
  // followed by the writeback/fence packets that the surrounding guest wait is
  // polling; in that interval the swap counter has advanced while RPTR still
  // trails WPTR. Exposing the real clock there makes sub_82198C28 hit its
  // hardware-scale 5000-tick timeout and run the guest D3D "GPU is hung" dump
  // even though the command processor is actively retiring the remaining
  // packets.
  //
  // A drained snapshot alone is not sufficient: the producer can append the
  // next submission immediately after that snapshot. Releasing a large frozen
  // time delta on that single poll recreates the false timeout before the CP
  // can retire the new tail. Require the same wait context and saved heartbeat
  // token to observe a continuously drained ring for a short host-scale grace.
  // Any pending sample or new heartbeat token restarts the grace.
  //
  // After 60 ms of stable drain, restore the real timebase. A pending ring is
  // held only while its read pointer continues to make progress; 30 seconds
  // without progress restores the title's genuine hang detection. If the
  // command processor is unavailable, preserve the original watchdog behavior
  // as well. The title remains the sole owner of its fence, presented counter,
  // and skip bits.
  if (hold_watchdog_time) {
    if (ws) {
      r30.u32 = LD32(base, ws + 12u);
    }
    // The six GPU-completion waits poll this routine in a TIGHT busy spin. With
    // dozens of guest threads that oversubscribes the cores and starves the
    // rexglue CP worker thread -- which is the very thread that must advance the
    // ring read pointer / swap counter to satisfy (a)/(b). Result: the fence
    // never advances, the spin never exits = freeze (visual stops, audio thread
    // keeps running on its own core). Back off briefly while still waiting so
    // the CP worker reliably gets CPU without millions of scheduler yields per
    // second. The requested 50 us interval is far below a frame interval.
    std::this_thread::sleep_for(std::chrono::microseconds(50));
  }

  // The title owns its presented counter and device skip bits. The command
  // processor alone owns the ring RPTR write-back at idblk+60. Host writes to
  // any of these can race later guest work and acknowledge the wrong frame.
}

// ---------------------------------------------------------------------------
// GPU-completion fence (the real fix). Wired at the present path
// sub_821996F8 @ 0x82199948 (right after the kernel VdSwap), r31 = a1 (D3D
// device struct), r30 = v21 (cmd-buffer swap slot).
//
// At each guest present, sample rexglue's CP swap counter. The poll hook
// (ge_dbg_now) then treats the frame as GPU-complete only once the CP's
// counter has moved past this -- i.e. the just-submitted frame was really
// drawn -- so the game blocks for the real render (visible) but no longer.
void ge_diag_vdswap(PPCRegister& r31, PPCRegister& r30) {
  PPCContext* ctx;
  uint8_t* base;
  getcb(ctx, base);
  (void)ctx;
  (void)r30;
  uint32_t a1 = r31.u32;
  auto* cpp = ge_cp();
  uint32_t cpc = cpp ? cpp->swap_counter() : 0;
  g_present_cpcnt.store(cpc, std::memory_order_relaxed);
  ge_start_watchdog_once();

  static uint32_t n = 0;  // throttled fps heartbeat
  uint32_t present_index = ++n;
  static const bool force_presented_on_vdswap =
      std::getenv("GOLDENEYE_FORCE_PRESENTED_ON_VDSWAP") != nullptr;
  if (force_presented_on_vdswap && a1) {
    uint32_t submit = LD32(base, a1 + 16544u);
    uint32_t presented = LD32(base, a1 + 16552u);
    if (submit > presented) {
      ST32(base, a1 + 16552u, submit);
      static std::atomic<uint32_t> presented_updates{0};
      uint32_t update_index = presented_updates.fetch_add(1, std::memory_order_relaxed) + 1;
      if (update_index <= 16 || (update_index & 0x3F) == 0) {
        std::fprintf(stderr,
                     "[ge] VdSwap advanced presented#%u present=%u submit=%u old=%u new=%u\n",
                     update_index, present_index, submit, presented, submit);
        std::fflush(stderr);
      }
    }
  }
  static const bool submission_diagnostics =
      EnvironmentFlagEnabled("GOLDENEYE_METAL_SUBMISSION_DIAGNOSTICS");
  if (submission_diagnostics && (present_index <= 16 || (present_index & 0x3F) == 0)) {
    static uint64_t previous_blocked_polls = 0;
    static uint64_t previous_completed_polls = 0;
    uint64_t blocked_polls = g_gpu_wait_blocked_polls.load(std::memory_order_relaxed);
    uint64_t completed_polls = g_gpu_wait_completed_polls.load(std::memory_order_relaxed);
    uint64_t drain_grace_polls = g_gpu_wait_drain_grace_polls.load(std::memory_order_relaxed);
    uint64_t drain_grace_starts = g_gpu_wait_drain_grace_starts.load(std::memory_order_relaxed);
    uint64_t stable_drain_releases =
        g_gpu_wait_stable_drain_releases.load(std::memory_order_relaxed);
    uint64_t pending_progress_resets =
        g_gpu_wait_pending_progress_resets.load(std::memory_order_relaxed);
    uint64_t pending_stall_releases =
        g_gpu_wait_pending_stall_releases.load(std::memory_order_relaxed);
    uint32_t read_pointer = cpp ? cpp->read_ptr_index() : 0;
    uint32_t write_pointer = cpp ? cpp->write_ptr_index() : 0;
    std::fprintf(stderr,
                 "[ge] VdSwap hook present#%u dev=0x%08x cpcnt=%u ring=%u/%u drained=%u "
                 "gpu_wait_polls(blocked=%llu/+%llu complete=%llu/+%llu "
                 "drain_grace=%llu starts=%llu stable_releases=%llu "
                 "pending_progress=%llu stall_releases=%llu)\n",
                 present_index, a1, cpc, read_pointer, write_pointer,
                 cpp && cpp->primary_ring_drained() ? 1u : 0u,
                 static_cast<unsigned long long>(blocked_polls),
                 static_cast<unsigned long long>(blocked_polls - previous_blocked_polls),
                 static_cast<unsigned long long>(completed_polls),
                 static_cast<unsigned long long>(completed_polls - previous_completed_polls),
                 static_cast<unsigned long long>(drain_grace_polls),
                 static_cast<unsigned long long>(drain_grace_starts),
                 static_cast<unsigned long long>(stable_drain_releases),
                 static_cast<unsigned long long>(pending_progress_resets),
                 static_cast<unsigned long long>(pending_stall_releases));
    previous_blocked_polls = blocked_polls;
    previous_completed_polls = completed_polls;
    if (a1) {
      // Sequence the counter reads explicitly. Function-argument evaluation
      // order would otherwise make concurrent title updates look inverted.
      uint32_t presented = LD32(base, a1 + 16552u);
      uint32_t submit = LD32(base, a1 + 16544u);
      std::fprintf(stderr,
                   "[ge] present flags#%u +10941=0x%02x +10943=0x%02x +21516=0x%08x "
                   "+21600=%u +21604=%u +22280=0x%08x submit=%u presented=%u render_gate=0x%08x\n",
                   present_index, base[a1 + 10941u], base[a1 + 10943u], LD32(base, a1 + 21516u),
                   LD32(base, a1 + 21600u), LD32(base, a1 + 21604u), LD32(base, a1 + 22280u),
                   submit, presented, LD32(base, 0x8242043Cu));
    }
    std::fflush(stderr);
  }
  if (submission_diagnostics && (present_index & 0x3F) == 0)
    REXKRNL_INFO("GEGPU present#{} dev={:#x} cpcnt={}", n, a1, cpc);
  if (std::getenv("GOLDENEYE_PRESENT_THREAD_SAMPLE") &&
      (present_index == 64 || present_index == 128 || (present_index & 0xFF) == 0)) {
    std::thread([base, present_index]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      ge_sample_present_main_thread_path(base, present_index);
    }).detach();
  }
}

void ge_trace_render_submit(PPCRegister& r3) {
  if (!std::getenv("GOLDENEYE_TRACE_FRAME")) {
    return;
  }
  PPCContext* ctx;
  uint8_t* base;
  getcb(ctx, base);
  (void)ctx;
  uint32_t dev = r3.u32;
  static std::atomic<uint32_t> trace_count{0};
  uint32_t n = trace_count.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 16 || (n & 0x3F) == 0) {
    uint32_t render_gate = LD32(base, 0x8242043Cu);
    std::fprintf(stderr,
                 "[ge] GETRACE render_submit#%u dev=0x%08x render_gate=0x%08x "
                 "skip=0x%08x ring=%u/%u submit=%u presented=%u wait=0x%08x\n",
                 n, dev, render_gate, dev ? LD32(base, dev + 21516u) : 0,
                 dev ? LD32(base, dev + 21600u) : 0, dev ? LD32(base, dev + 21604u) : 0,
                 dev ? LD32(base, dev + 16544u) : 0, dev ? LD32(base, dev + 16552u) : 0,
                 dev ? LD32(base, dev + 22280u) : 0);
    std::fflush(stderr);
  }
}

// F3  0x830E0670 (site 0x8209F5F0 sub_8209F5D8 -> ge_cont_8209F5F4)
void ge_hook_830E0670(PPCRegister& r3, PPCRegister& r11, PPCRegister& r28) {
  PPCContext* ctx;
  uint8_t* base;
  getcb(ctx, base);
  r11.u32 = r3.u32 ^ 0x2Bu;
  r28.u32 = 0x82420000u;
  base[0x82420239u] = static_cast<uint8_t>(r11.u32 & 0xFFu);
  r28.u32 = 0x82420239u;
  r11.u32 = 0x82000000u;
  ge_cont_8209F5F4(*ctx, base);
}

// F1  0x830E0630: the r30++ (with 3/6 skip) loop-increment fragment. Hooked at
// the branch site 0x820F774C; the config jump_address sends control back to
// 0x820F7750 (cmpwi r30,8 / blt loc_820F768C) IN THE PARENT sub_820F73F8, so the
// whole loop -- including the loop-back to 0x820F768C -- stays in one function
// and resolves. (Routing through a separate ge_cont_820F7750 left that loop-back
// branch cross-function -> REX_FATAL when the loop ran, e.g. at the main menu.)
bool ge_hook_830E0630(PPCRegister& r30) {
  r30.u32 = r30.u32 + 1;
  if (r30.s32 == 3 || r30.s32 == 6)
    r30.u32 = r30.u32 + 1;
  // 0x820F7750: cmpwi r30,8 ; 0x820F7754: blt loc_820F768C (loop) else exit.
  return r30.s32 < 8;
}

// --- Hidden debug menu (TCRF: set byte 0x82189F2B 0x0F->0x00) ----------------
// In sub_821898D0: r30 = button mask, r11 = r30 & 0x100 (LB / LSHOULDER), then
//   0x82189F28  cmplwi cr6, r11, 0xF      ; r11 is only ever 0 or 0x100
//   0x82189F2C  beq    cr6, 0x82189F4C    ; so EQ never true -> dead branch
//   0x82189F30  ...LB-> debug-menu toggle handler (runs every frame in retail)
// Setting the 0xF immediate to 0 makes the compare "r11 == 0", so the beq is
// taken when LB is UP and the handler runs only when LB is PRESSED == LB toggles
// the debug menu. The recomp runs generated code (can't patch the byte), so we
// replace the branch decision here and read it live from a cvar (no restart).
//   debug ON : take branch (skip handler) iff LB up  -> handler fires on LB
//   debug OFF: never take branch                     -> identical to retail
// Hooked at 0x82189F2C with jump_on_true=0x82189F4C, jump_on_false=0x82189F30.
REXCVAR_DEFINE_BOOL(ge_debug_menu, false, "Input",
                    "Enable the hidden debug menu (press LB in-game to toggle it)");
bool ge_debug_gate(PPCRegister& r30) {
  return REXCVAR_GET(ge_debug_menu) && ((r30.u32 & 0x100u) == 0u);
}

// F2  sub_820F7968: r26 0..8 loop. sub_820F7968 = prologue only (codegen sets
// constants + r26=0). ge_f2_driver fires after the last prologue instruction
// (0x820F79EC, after, return) and drives the loop:
//   do { body; r26++; if(r26==3||r26==6) r26++; } while (r26 < 8); epilogue
// ge_body_820F79F0 = 0x820F79F0..0x820F7CFC (one iteration). Its skip branch
// (0x820F7A2C: clrlwi r11,r3,24; cmplwi r11,0; beq loc_820F7D00) becomes a
// return from the body via ge_f2_skip (return_on_true).
bool ge_f2_skip(PPCRegister& r3) {
  return (r3.u32 & 0xFFu) == 0u;  // beq loc_820F7D00 taken when (r3&0xFF)==0
}
void ge_f2_driver(PPCRegister& r26) {
  PPCContext* ctx;
  uint8_t* base;
  getcb(ctx, base);
  for (;;) {
    ge_body_820F79F0(*ctx, base);  // 0x820F79F0 one iteration
    r26.u32 = r26.u32 + 1;         // 0x830E06B0 increment
    if (r26.s32 == 3 || r26.s32 == 6)
      r26.u32 = r26.u32 + 1;
    if (r26.s32 >= 8)
      break;  // 0x820F7D08 blt not taken
  }
  ge_epi_820F7D0C(*ctx, base);  // 0x820F7D0C epilogue (ret)
}

// F4  0x830E0200: loop-increment fragment (same shape as F1). Hooked at the
// branch site 0x820C4914; the config jump_address sends control back to
// 0x820C4918 IN THE PARENT sub_820C4630 so the loop-back to 0x820C4858 resolves
// in-function instead of crossing into a ge_cont_820C4918 (-> REX_FATAL).
bool ge_hook_830E0200(PPCRegister& r31, PPCRegister& r29, PPCRegister& r28, PPCRegister& r11,
                      PPCRegister& r23, PPCRegister& r21) {
  PPCContext* ctx;
  uint8_t* base;
  getcb(ctx, base);
  if (r31.s32 == 3) {
    r31.u32 = r31.u32 + 1;
    r29.u32 = r29.u32 + 4;
  }
  r28.u32 = r11.u32 + r28.u32;
  // 0x820C4918: cmpw r31,r23 ; 0x820C491C: ble loc_820C4858 (loop) else exit.
  // The loop top 0x820C4858 (lwz r11,-0x684(r21)) is skipped on first entry
  // (0x820C4854 b loc_820C485C) -> only the loop-back reaches it, so it is not a
  // standalone block. Do its r11 reload here and jump to 0x820C485C (which IS
  // reachable / labeled) instead.
  if (r31.s32 <= r23.s32) {
    r11.u32 = LD32(base, r21.u32 - 0x684u);  // 0x820C4858: lwz r11,-0x684(r21)
    return true;                             // -> loc_820C485C (loop body)
  }
  return false;  // -> loc_820C4920 (exit)
}

// F5  0x830E04D0 (site 0x820C7450 sub_820C7390 -> ge_cont_820C7454)
void ge_hook_830E04D0(PPCRegister& r11, PPCRegister& r10, PPCRegister& r9) {
  PPCContext* ctx;
  uint8_t* base;
  getcb(ctx, base);
  if (r11.s32 == 3)
    r11.u32 = r11.u32 - 1;
  ST32(base, r9.u32 - 0x644u, r10.u32);
  ge_cont_820C7454(*ctx, base);
}

// F6  0x830E0560 (site 0x820C742C sub_820C7390 -> ge_cont_820C7430)
void ge_hook_830E0560(PPCRegister& r11, PPCRegister& r10, PPCRegister& r9) {
  PPCContext* ctx;
  uint8_t* base;
  getcb(ctx, base);
  if (r11.s32 == 3)
    r11.u32 = r11.u32 + 1;
  ST32(base, r9.u32 - 0x644u, r10.u32);
  ge_cont_820C7430(*ctx, base);
}

// F7  0x830E0460 (site 0x820A3E50 sub_820A3C20 -> ge_cont_820A3E9C)
void ge_hook_830E0460(PPCRegister& r11, PPCRegister& r4, PPCRegister& r29, PPCRegister& r7,
                      PPCRegister& r28, PPCRegister& r6, PPCRegister& r31, PPCRegister& r5,
                      PPCRegister& r27, PPCRegister& r3) {
  PPCContext* ctx;
  uint8_t* base;
  getcb(ctx, base);
  r4.s64 = static_cast<int64_t>(static_cast<int16_t>(r11.u32 & 0xFFFFu));
  r7.u64 = r29.u64;
  r6.u32 = LD32(base, r28.u32 + 0x4DE8u);
  r5.u64 = r31.u64;
  r3.u32 = LD32(base, r27.u32 + 0x4DE0u);
  sub_82144920(*ctx, base);
  r3.u32 = 0;
  ge_cont_820A3E9C(*ctx, base);
}

// F8  0x830E0750 (site 0x820B40E4 sub_820B40C0, returns; code ends in blr)
void ge_hook_830E0750(PPCRegister& r7, PPCRegister& r8, PPCRegister& r11, PPCRegister& f1) {
  PPCContext* ctx;
  uint8_t* base;
  getcb(ctx, base);
  (void)ctx;
  uint32_t v32 = LD32(base, r8.u32 - 0x564u);
  r7.u32 = v32;
  if (v32 != 0)
    return;
  uint64_t v64 = LD64(base, r8.u32 - 0x560u);
  r7.u64 = v64;
  if (v64 != 0)
    return;
  STF32(base, r11.u32 + 0x1F0u, f1.f32);
  uint32_t t = LD32(base, r11.u32 + 0x1E4u);
  r8.u32 = t;
  ST32(base, r11.u32 + 0x1ECu, t);
  r8.u32 = 0;
  ST32(base, r11.u32 + 0x200u, 0);
}

// ===========================================================================
// Mouse-look (real 1:1 keyboard/mouse looking, replacing the stick-emulation
// path). We inject raw mouse deltas straight into the guest's per-frame look
// state inside ge_bondview_control (sub_820B99E8), so the mouse drives the same
// heading/pitch the right stick does -- but without the analog deadzone/accel/
// turn-rate cap that makes the built-in MnK stick emulation feel terrible.
//
// YAW   @0x820bab6c : flt_82F1F914 (per-frame yaw delta, RADIANS) += mouse_dx.
// PITCH @0x820bb79c : bondview(+612) (pitch angle) += mouse_dy, +center suppress.
//
// Mouse and controller both work AT ONCE (additive) -- the mouse just adds to
// whatever the right stick contributes, so you can pick up or put down the pad
// freely, or play pure mouse+keyboard with no pad plugged in. Movement / buttons
// stay on rexglue's built-in keybinds -- only the *look* is custom here.
//
// While mouse-look is on we also capture the OS cursor (hidden + confined to the
// window) during play, and release it whenever the pause menu is open or the
// window loses focus.
// ===========================================================================

// Mouse-look tunables, ported from the xenia-canary mousehook cvars. The
// user-facing sensitivity multiplier is ge_mouse_sens (defined below).
REXCVAR_DEFINE_BOOL(ge_invert_x, false, "Input", "Invert mouse X (horizontal) look");
REXCVAR_DEFINE_BOOL(ge_invert_y, false, "Input", "Invert mouse Y (vertical) look");
REXCVAR_DEFINE_BOOL(ge_disable_autoaim, true, "Input",
                    "Disable auto-aim and look-ahead while mouse-look is on");
REXCVAR_DEFINE_DOUBLE(ge_menu_sensitivity, 1.0, "Input", "Mouse sensitivity in menus")
    .range(0.05, 20.0);
REXCVAR_DEFINE_DOUBLE(ge_aim_turn_distance, 0.4, "Input",
                      "Crosshair travel in aim-mode before the camera turns [0-1]")
    .range(0.0, 1.0);
REXCVAR_DEFINE_BOOL(ge_gun_sway, true, "Input", "Gun sway as the camera turns");

#if !defined(__APPLE__)
REXCVAR_DEFINE_DOUBLE(ge_mouse_sens, 1.0, "Input", "Mouse look sensitivity").range(0.05, 20.0);
// Mouse-look on/off. ON: the mouse looks (added on top of the pad -- both work
// at once, so you can put the controller down) and the cursor is captured during
// play. OFF: no mouse-look, cursor free, controller only. macOS uses the common
// MnK enable and sensitivity settings instead, avoiding duplicate controls.
REXCVAR_DEFINE_BOOL(ge_mouselook_enable, true, "Input",
                    "Mouse look (works alongside the controller; captures the cursor in-game)");
#endif

namespace {
std::mutex g_mouse_delta_mutex;
int g_mouse_dx = 0;
int g_mouse_dy = 0;
std::atomic<bool> g_mouselook_suppressed{false};  // set true while the pause menu is open
// True while the rebind menu is waiting for a key. We must swallow ALL game input
// (keyboard injection AND the real controller) so the key being bound doesn't also
// act on the game/menu -- the menu only listens for the capture.
std::atomic<bool> g_rebind_capturing{false};

// Cursor-capture state (touched from the mouse thread + SetMouselookSuppressed).
#if defined(_WIN32)
HWND g_game_hwnd = nullptr;
HCURSOR g_arrow_cursor = nullptr;
HCURSOR g_blank_cursor = nullptr;
#endif
bool g_captured = false;

bool ge_mouselook_on() {
#if defined(__APPLE__)
  return REXCVAR_QUERY(bool, mnk_mouse_enabled);
#else
  return REXCVAR_GET(ge_mouselook_enable);
#endif
}

bool ge_game_has_focus() {
#if defined(_WIN32)
  HWND fg = GetForegroundWindow();
  if (!fg)
    return false;
  DWORD pid = 0;
  GetWindowThreadProcessId(fg, &pid);
  return pid == GetCurrentProcessId();
#else
  return true;
#endif
}

// The visible game window (the foreground window while it's ours). Cached so we
// can still restore the cursor after focus has moved elsewhere.
#if defined(_WIN32)
HWND ge_game_window() {
  HWND fg = GetForegroundWindow();
  if (fg) {
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid == GetCurrentProcessId())
      g_game_hwnd = fg;
  }
  return g_game_hwnd;
}
#endif

// Active = mouse-look on, no menu up, and we own focus. Drives both delta
// collection and cursor capture.
bool ge_mouse_active() {
  return ge_mouselook_on() && !g_mouselook_suppressed.load(std::memory_order_relaxed) &&
         ge_game_has_focus();
}

void ge_add_mouse_delta(int dx, int dy) {
  std::lock_guard lock(g_mouse_delta_mutex);
  g_mouse_dx = static_cast<int>(std::clamp(static_cast<int64_t>(g_mouse_dx) + dx,
                                           static_cast<int64_t>(std::numeric_limits<int>::min()),
                                           static_cast<int64_t>(std::numeric_limits<int>::max())));
  g_mouse_dy = static_cast<int>(std::clamp(static_cast<int64_t>(g_mouse_dy) + dy,
                                           static_cast<int64_t>(std::numeric_limits<int>::min()),
                                           static_cast<int64_t>(std::numeric_limits<int>::max())));
}

void ge_clear_mouse_delta() {
  std::lock_guard lock(g_mouse_delta_mutex);
  g_mouse_dx = 0;
  g_mouse_dy = 0;
}

#if defined(_WIN32)
HCURSOR ge_make_blank_cursor() {
  // 32x32 fully transparent cursor (AND=1 / XOR=0 == transparent everywhere).
  BYTE and_mask[32 * 32 / 8];
  BYTE xor_mask[32 * 32 / 8];
  std::memset(and_mask, 0xFF, sizeof(and_mask));
  std::memset(xor_mask, 0x00, sizeof(xor_mask));
  return CreateCursor(GetModuleHandleW(nullptr), 0, 0, 32, 32, and_mask, xor_mask);
}

// Capture (hide + confine) the cursor during play; release it in menus, when
// unfocused, or when mouse-look is off. Hiding is done by swapping the game
// window's class cursor (works cross-thread, unlike ShowCursor). Safe to call
// from any thread.
void ge_update_mouse_capture() {
  HWND hwnd = ge_game_window();
  const bool want = ge_mouse_active() && hwnd != nullptr;
  if (want) {
    if (!g_captured) {
      g_captured = true;
      if (g_blank_cursor)
        SetClassLongPtrW(hwnd, GCLP_HCURSOR, (LONG_PTR)g_blank_cursor);
      REXKRNL_INFO("GEMOUSE capture ON  hwnd={}", (void*)hwnd);
    }
    RECT rc;
    if (GetClientRect(hwnd, &rc)) {
      POINT tl{rc.left, rc.top}, br{rc.right, rc.bottom};
      ClientToScreen(hwnd, &tl);
      ClientToScreen(hwnd, &br);
      RECT screen{tl.x, tl.y, br.x, br.y};
      ClipCursor(&screen);  // re-confine each tick (window may move/resize)
    }
  } else if (g_captured) {
    g_captured = false;
    if (hwnd && g_arrow_cursor)
      SetClassLongPtrW(hwnd, GCLP_HCURSOR, (LONG_PTR)g_arrow_cursor);
    ClipCursor(nullptr);
  }
}

LRESULT CALLBACK GeRawWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_INPUT) {
    RAWINPUT ri;
    UINT sz = sizeof(ri);
    if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &ri, &sz, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
        ri.header.dwType == RIM_TYPEMOUSE && (ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0) {
      static std::atomic<bool> logged{false};
      bool exp = false;
      if (logged.compare_exchange_strong(exp, true))
        REXKRNL_INFO("GEMOUSE first WM_INPUT dx={} dy={} active={}", ri.data.mouse.lLastX,
                     ri.data.mouse.lLastY, ge_mouse_active());
      // Only collect while active, so deltas don't queue while alt-tabbed/paused
      // and snap the view on return.
      if (ge_mouse_active()) {
        ge_add_mouse_delta(ri.data.mouse.lLastX, ri.data.mouse.lLastY);
      }
    }
    return 0;
  }
  if (msg == WM_TIMER) {
    ge_update_mouse_capture();
    return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

// Dedicated thread: own a message-only window + RIDEV_INPUTSINK raw mouse so we
// get true mouse deltas without touching the SDK's window/message loop. A timer
// drives the cursor capture/release each ~15ms.
void ge_mouse_thread() {
  g_arrow_cursor = LoadCursorA(nullptr, IDC_ARROW);  // IDC_ARROW is an ANSI int-resource
  g_blank_cursor = ge_make_blank_cursor();
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = GeRawWndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"GeRawMouseWnd";
  RegisterClassExW(&wc);
  HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                              wc.hInstance, nullptr);
  if (!hwnd)
    return;
  RAWINPUTDEVICE rid{};
  rid.usUsagePage = 0x01;  // generic desktop
  rid.usUsage = 0x02;      // mouse
  rid.dwFlags = RIDEV_INPUTSINK;
  rid.hwndTarget = hwnd;
  BOOL reg = RegisterRawInputDevices(&rid, 1, sizeof(rid));
  REXKRNL_INFO("GEMOUSE thread up: hwnd={} rawinput_register={} (err={})", (void*)hwnd,
               reg ? "OK" : "FAIL", reg ? 0u : GetLastError());
  if (!reg)
    return;
  SetTimer(hwnd, 1, 15, nullptr);  // ~60Hz capture-state poll
  MSG m;
  while (GetMessageW(&m, nullptr, 0, 0) > 0) {
    TranslateMessage(&m);
    DispatchMessageW(&m);
  }
}
#else
void ge_update_mouse_capture() {}

void ge_mouse_thread() {}
#endif

void ge_start_mouse_once() {
#if defined(_WIN32)
  static std::atomic<bool> started{false};
  bool expected = false;
  if (started.compare_exchange_strong(expected, true)) {
    std::thread(ge_mouse_thread).detach();
  }
#endif
}

rex::input::MouseMotionDelta ge_take_mouse_delta() {
  std::lock_guard lock(g_mouse_delta_mutex);
  rex::input::MouseMotionDelta result{g_mouse_dx, g_mouse_dy};
  g_mouse_dx = 0;
  g_mouse_dy = 0;
  return result;
}
}  // namespace

namespace ge {
// Start the raw-mouse + cursor-capture thread once, at app startup, so capture
// works regardless of whether the guest look hooks have fired yet.
void InitMouseLook() {
#if defined(_WIN32)
  REXKRNL_INFO("GEMOUSE InitMouseLook (enable={})", REXCVAR_GET(ge_mouselook_enable));
  ge_start_mouse_once();
#endif
}

// Called by the app when the pause menu opens/closes so mouse motion isn't
// turned into look while the player is in menus (the cursor is needed there).
void SetMouselookSuppressed(bool v) {
  g_mouselook_suppressed.store(v, std::memory_order_relaxed);
  if (v) {  // drop any queued motion so closing the menu doesn't snap the view
    ge_clear_mouse_delta();
  } else {
    g_rebind_capturing.store(false, std::memory_order_relaxed);  // never stick on close
  }
  ge_update_mouse_capture();  // release the cursor immediately when the menu opens
}

// Called by the rebind menu while it is waiting for a key. While true, all slot-0
// controller input is swallowed (ge_inject_keyboard) so the bound key can't act.
void SetRebindCapturing(bool v) {
  g_rebind_capturing.store(v, std::memory_order_relaxed);
}
}  // namespace ge

// ===========================================================================
// Mouse-look: faithful port of the xenia-canary mousehook
// (src/xenia/hid/winkey/hookables/goldeneye.cc, GoldeneyeGame::DoHooks) for the
// GoldenEye_Nov2007_Release build. Runs once per frame from ge_inject_keyboard,
// operating on the local player struct in guest RAM. Writes the game's own
// camera / crosshair / gun fields incrementally so it coexists with the
// controller, recoil, and the tank turret.
// ===========================================================================
namespace {
// RareGameBuildAddrs for GoldenEye_Nov2007_Release (from supported_builds[]).
constexpr uint32_t GE_MENU_XY = 0x8272B37Cu;       // menu cursor X (Y at +4)
constexpr uint32_t GE_PAUSE_FLAG = 0x82F1E70Cu;    // non-zero ~= game paused
constexpr uint32_t GE_SETTINGS_PTR = 0x83088228u;  // -> settings struct pointer
constexpr uint32_t GE_SETTINGS_BITS = 0x298u;      // bitflags offset in struct
constexpr uint32_t GE_PLAYER_PTR = 0x82F1FA98u;    // -> players[0] (host's Bond)
constexpr uint32_t GE_BONDVIEW_CUR = 0x82F1FAACu;  // -> currently-controlled view's player
constexpr uint32_t GE_OFF_WATCH = 0x2E8u;          // watch status (!=0 -> input disabled)
constexpr uint32_t GE_OFF_DISABLED = 0x80u;        // control-disabled flag (cutscene)
constexpr uint32_t GE_OFF_CAM_X = 0x254u;          // camera yaw
constexpr uint32_t GE_OFF_CAM_Y = 0x264u;          // camera pitch
constexpr uint32_t GE_OFF_CH_X = 0x10A8u;          // crosshair X
constexpr uint32_t GE_OFF_CH_Y = 0x10ACu;          // crosshair Y
constexpr uint32_t GE_OFF_GUN_X = 0x10BCu;         // gun X
constexpr uint32_t GE_OFF_GUN_Y = 0x10C0u;         // gun Y
constexpr uint32_t GE_OFF_AIM_MODE = 0x22Cu;       // aim-mode (1 = aiming)
constexpr uint32_t GE_OFF_AIM_MULT = 0x11ACu;      // aim-turn multiplier (slows when zoomed)
enum GESettingFlag {
  GE_SET_AutoAim = 0x10,
  GE_SET_LookAhead = 0x80,
};
}  // namespace

void ge_mouse_camera(uint8_t* base, rex::input::MouseMotionDelta mouse_delta) {
  // Persistent state (= GoldeneyeGame member vars in xenia).
  static uint32_t prev_pause = 0, prev_disabled = 0, prev_aim_mode = 0;
  static bool start_centering = false, disable_sway = false;
  static float centering_speed = 0.0125f;

#if defined(__APPLE__)
  // Preserve the sensitivity macOS users already configure in the native MnK
  // controls while routing motion through the title's direct camera path.
  const float sensitivity = static_cast<float>(REXCVAR_QUERY(double, mnk_sensitivity));
#else
  const float sensitivity = static_cast<float>(REXCVAR_GET(ge_mouse_sens));
#endif
  const float menu_sensitivity = static_cast<float>(REXCVAR_GET(ge_menu_sensitivity));
  const bool invert_x = REXCVAR_GET(ge_invert_x);
  const bool invert_y = REXCVAR_GET(ge_invert_y);
  const bool disable_autoaim = REXCVAR_GET(ge_disable_autoaim);
  const bool gun_sway = REXCVAR_GET(ge_gun_sway);

  // The paired sample has already been consumed once by the platform input
  // owner; use it for both the game menu and camera in this guest frame.
  const float mdx = static_cast<float>(mouse_delta.x);
  const float mdy = static_cast<float>(mouse_delta.y);

  // Move the menu selection crosshair (the game's own menus read these).
  {
    float menuX = LDF32(base, GE_MENU_XY);
    float menuY = LDF32(base, GE_MENU_XY + 4);
#if defined(__APPLE__)
    // The menu cursor is already expressed in a 1280x720 logical coordinate
    // space. Apply native macOS mouse distance directly; treating it as stick
    // velocity is what made menus sluggish and nonlinear.
    menuX += mdx * menu_sensitivity;
    menuY += mdy * menu_sensitivity;
#else
    // Preserve the established Win32 raw-input calibration.
    menuX += (mdx / 5.f) * menu_sensitivity;
    menuY += (mdy / 5.f) * menu_sensitivity;
#endif
    STF32(base, GE_MENU_XY, menuX);
    STF32(base, GE_MENU_XY + 4, menuY);
  }

  // Target the LOCAL player. Online uses Xbox System Link, where each console
  // controls its OWN Bond at a session-global index (host = players[0], clients =
  // players[1..3]). players[0] (GE_PLAYER_PTR) is therefore only the local player
  // on the host -- using it made mouse-look host-only. GE_BONDVIEW_CUR points at
  // the view the local console is actually driving, so it resolves to this
  // console's player in online play and to players[0] in single-player/the host.
  // Target the LOCAL player = the active-viewport player. player+0x904 (viewport
  // size/offset) is 0 only for the view the local console actually renders -- the
  // same signal GoldenEye's own code uses ("current player is the active
  // viewport", per the CE 3D-SFX hack). This is stable every frame and resolves
  // to players[0] on the host, players[1] on a joiner, etc., automatically.
  // (The old GE_BONDVIEW_CUR target flipped between local/remote each frame
  // because the bondview CONTROL loop cycles it across all players -- that was
  // the online jitter.)
  uint32_t player = 0;
  for (int i = 0; i < 4; ++i) {
    uint32_t p = LD32(base, 0x82F1FA98u + i * 4u);
    if (p && LD32(base, p + 0x904u) == 0u) {
      player = p;
      break;
    }
  }
  if (!player)
    player = LD32(base, GE_BONDVIEW_CUR);  // fallback (menus/boot)
  if (!player)
    player = LD32(base, GE_PLAYER_PTR);
  if (!player)
    return;

  const uint32_t game_pause_flag = LD32(base, GE_PAUSE_FLAG);

  // control-disabled (cutscene); fall back to watch-status (watch up/down).
  uint32_t game_control_disabled = LD32(base, player + GE_OFF_DISABLED);
  if (game_control_disabled == 0)
    game_control_disabled = LD32(base, player + GE_OFF_WATCH);

  // Disable auto-aim & look-ahead, only when the pause/control state changes --
  // xenia's exact behaviour. (Doing it every frame oscillated against the game's
  // per-frame auto-aim in multiplayer and caused the camera jitter.)
  if (game_pause_flag != prev_pause || game_control_disabled != prev_disabled) {
    const uint32_t sp = LD32(base, GE_SETTINGS_PTR);
    if (sp) {
      const uint32_t sva = sp + GE_SETTINGS_BITS;
      uint32_t settings = LD32(base, sva);
      if (settings & GE_SET_LookAhead)
        settings &= ~(uint32_t)GE_SET_LookAhead;
      if (disable_autoaim && (settings & GE_SET_AutoAim))
        settings &= ~(uint32_t)GE_SET_AutoAim;
      ST32(base, sva, settings);
    }
    prev_pause = game_pause_flag;
    prev_disabled = game_control_disabled;
  }

  if (game_control_disabled)
    return;

  const uint32_t aim_mode = LD32(base, player + GE_OFF_AIM_MODE);
  if (aim_mode != prev_aim_mode) {
    if (aim_mode != 0) {  // entering aim mode -> reset gun position
      STF32(base, player + GE_OFF_GUN_X, 0.f);
      STF32(base, player + GE_OFF_GUN_Y, 0.f);
    }
    // Always reset crosshair on enter/exit (else non-aim fires toward it).
    STF32(base, player + GE_OFF_CH_X, 0.f);
    STF32(base, player + GE_OFF_CH_Y, 0.f);
    prev_aim_mode = aim_mode;
  }

  const float bounds = 1.f;
  const float crosshair_multiplier = 1.f, centering_multiplier = 1.f;

  if (aim_mode == 1) {
    // #61: DIRECT 1:1 mouse aim. The old Xenia crosshair-travel mechanic moved a
    // free crosshair and only turned the camera past a threshold; the game's
    // native aim-mode auto-centering then sprang the crosshair/view back to the
    // screen centre the instant the mouse stopped (the "snaps to middle" bug).
    // Instead drive the camera straight from the mouse (same feel as hip-fire /
    // v1.2.2) and hold the crosshair + gun centred, so there is nothing for the
    // game to spring back to.
    if (mdx != 0.f || mdy != 0.f) {
      float camX = LDF32(base, player + GE_OFF_CAM_X);
      float camY = LDF32(base, player + GE_OFF_CAM_Y);
      camX += (invert_x ? -1.f : 1.f) * (mdx / 10.f) * sensitivity;
      camY -= (invert_y ? -1.f : 1.f) * (mdy / 10.f) * sensitivity;
      STF32(base, player + GE_OFF_CAM_X, camX);
      STF32(base, player + GE_OFF_CAM_Y, camY);
    }
    STF32(base, player + GE_OFF_CH_X, 0.f);
    STF32(base, player + GE_OFF_CH_Y, 0.f);
    STF32(base, player + GE_OFF_GUN_X, 0.f);
    STF32(base, player + GE_OFF_GUN_Y, 0.f);
    start_centering = false;  // nothing to centre -> no spring-back
    disable_sway = false;
  } else {
    float gX = LDF32(base, player + GE_OFF_GUN_X);
    float gY = LDF32(base, player + GE_OFF_GUN_Y);

    // Gun-centering back to the middle after aim-mode / when idle.
    if (start_centering) {
      if (gX != 0 || gY != 0) {
        if (gX > 0)
          gX -= std::min(centering_speed * centering_multiplier, gX);
        if (gX < 0)
          gX += std::min(centering_speed * centering_multiplier, -gX);
        if (gY > 0)
          gY -= std::min(centering_speed * centering_multiplier, gY);
        if (gY < 0)
          gY += std::min(centering_speed * centering_multiplier, -gY);
      }
      if (gX == 0 && gY == 0) {
        centering_speed = 0.0125f;
        start_centering = false;
        disable_sway = false;
      }
    }

    if (mdx != 0.f || mdy != 0.f) {
      float camX = LDF32(base, player + GE_OFF_CAM_X);
      float camY = LDF32(base, player + GE_OFF_CAM_Y);

      camX += (invert_x ? -1.f : 1.f) * (mdx / 10.f) * sensitivity;

      // Add 'sway' to the gun as the camera turns.
      const float gun_sway_x = ((mdx / 16000.f) * sensitivity) * bounds;
      const float gun_sway_y = ((mdy / 16000.f) * sensitivity) * bounds;
      float gun_sway_x_changed = gX + gun_sway_x;
      float gun_sway_y_changed = gY + gun_sway_y;

      if (!invert_y) {
        camY -= (mdy / 10.f) * sensitivity;
      } else {
        camY += (mdy / 10.f) * sensitivity;
        gun_sway_y_changed = gY - gun_sway_y;
      }

      STF32(base, player + GE_OFF_CAM_X, camX);
      STF32(base, player + GE_OFF_CAM_Y, camY);

      if (gun_sway && !disable_sway) {
        // Bound the sway to [0.2:-0.2] (only if it would push further OOB).
        if (gun_sway_x_changed > (0.2f * bounds) && gun_sway_x > 0)
          gun_sway_x_changed = gX;
        if (gun_sway_x_changed < -(0.2f * bounds) && gun_sway_x < 0)
          gun_sway_x_changed = gX;
        if (gun_sway_y_changed > (0.2f * bounds) && gun_sway_y > 0)
          gun_sway_y_changed = gY;
        if (gun_sway_y_changed < -(0.2f * bounds) && gun_sway_y < 0)
          gun_sway_y_changed = gY;
        gX = gun_sway_x_changed;
        gY = gun_sway_y_changed;
      }
    } else {
      if (!start_centering) {
        start_centering = true;
        centering_speed = 0.0125f;
      }
    }

    gX = std::min(gX, bounds);
    gX = std::max(gX, -bounds);
    gY = std::min(gY, bounds);
    gY = std::max(gY, -bounds);

    STF32(base, player + GE_OFF_CH_X, gX * crosshair_multiplier);
    STF32(base, player + GE_OFF_CH_Y, gY * crosshair_multiplier);
    STF32(base, player + GE_OFF_GUN_X, gX);
    STF32(base, player + GE_OFF_GUN_Y, gY);
  }
}

// ===========================================================================
// Keyboard buttons -> guest gamepad. The right stick (look) is the mouse; every
// other controller input is mapped to a rebindable keyboard key here, injected
// into the polled gamepad buffer so it works alongside a real pad. We do this
// ourselves (not via the SDK's MnK driver) so it can't fight the mouse capture.
//
// Slot-0 gamepad buffer (filled by XamInputGetState in ge_input_poll_controllers,
// Xbox360 big-endian): +0 buttons(u16), +2 LT, +3 RT, +4 LX(s16), +6 LY(s16).
// ===========================================================================
namespace {
constexpr uint32_t GE_PAD0 = 0x830C8B9Cu;  // unk_830C8B9C, slot-0 gamepad
constexpr uint32_t GE_PAD_STRIDE = 16u;
constexpr uint32_t GE_LOCAL_PAD_COUNT = 4u;

// XInput button bits (match the masks the guest unpacks).
constexpr uint16_t BTN_DPAD_UP = 0x0001, BTN_DPAD_DOWN = 0x0002, BTN_DPAD_LEFT = 0x0004,
                   BTN_DPAD_RIGHT = 0x0008, BTN_START = 0x0010, BTN_BACK = 0x0020,
                   BTN_LTHUMB = 0x0040, BTN_RTHUMB = 0x0080, BTN_LSHOULDER = 0x0100,
                   BTN_RSHOULDER = 0x0200, BTN_A = 0x1000, BTN_B = 0x2000, BTN_X = 0x4000,
                   BTN_Y = 0x8000;

ge::host_pause::InputSample ge_physical_input_sample(uint32_t user_index) {
  ge::host_pause::InputSample result;
  auto* runtime = rex::Runtime::instance();
  auto* input = runtime && runtime->input_system()
                    ? static_cast<rex::input::InputSystem*>(runtime->input_system())
                    : nullptr;
  rex::input::ControllerSnapshot snapshot;
  if (!input || !input->GetControllerSnapshot(user_index, &snapshot) || !snapshot.connected) {
    return result;
  }
  result.buttons = static_cast<uint16_t>(snapshot.raw_gamepad.buttons);
  result.left_trigger = snapshot.raw_gamepad.left_trigger;
  result.right_trigger = snapshot.raw_gamepad.right_trigger;
  result.thumb_lx = static_cast<int16_t>(snapshot.raw_gamepad.thumb_lx);
  result.thumb_ly = static_cast<int16_t>(snapshot.raw_gamepad.thumb_ly);
  result.thumb_rx = static_cast<int16_t>(snapshot.raw_gamepad.thumb_rx);
  result.thumb_ry = static_cast<int16_t>(snapshot.raw_gamepad.thumb_ry);
  return result;
}

void ge_clear_guest_pads(uint8_t* base) {
  for (uint32_t user_index = 0; user_index < GE_LOCAL_PAD_COUNT; ++user_index) {
    const uint32_t pad = GE_PAD0 + user_index * GE_PAD_STRIDE;
    ST16(base, pad + 0, 0);
    base[pad + 2] = 0;
    base[pad + 3] = 0;
    ST16(base, pad + 4, 0);
    ST16(base, pad + 6, 0);
    ST16(base, pad + 8, 0);
    ST16(base, pad + 10, 0);
  }
}

void ge_discard_mouse_motion() {
#if defined(_WIN32)
  (void)ge_take_mouse_delta();
#elif defined(__APPLE__)
  auto* runtime = rex::Runtime::instance();
  auto* input = runtime && runtime->input_system()
                    ? static_cast<rex::input::InputSystem*>(runtime->input_system())
                    : nullptr;
  rex::input::MouseMotionDelta discarded = {};
  if (input) {
    input->ConsumeApplicationMouseMotion(0, &discarded);
  }
#endif
}

bool ge_auto_start_pressed(const char* mode, uint32_t input_poll) {
  // Keep the existing startup hold intact. In periodic mode, follow it with
  // monotonic-time retries so renderer speed cannot shift the input edges.
  bool periodic = std::strcmp(mode, "periodic") == 0;
  bool menu = std::strcmp(mode, "menu") == 0;
  if (!periodic && !menu) {
    return input_poll >= 20 && input_poll < 600;
  }
  static const auto started_at = std::chrono::steady_clock::now();
  uint64_t elapsed_ms = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - started_at)
                                     .count());
  if (elapsed_ms >= 200 && elapsed_ms < 1200) {
    return true;
  }
  if (elapsed_ms < 2000) {
    return false;
  }
  // The third retry ends at 14.25 seconds. Stop the menu diagnostic before the
  // fourth retry at 20 seconds, which can immediately leave a fast-loading
  // dossier menu after the clock and renderer performance fixes.
  if (menu && elapsed_ms >= 19000) {
    return false;
  }
  return ((elapsed_ms - 2000) % 6000) < 250;
}

// Input-only mission-navigation diagnostic. This deliberately does not inspect
// or modify title menu/mission state: GOLDENEYE_AUTO_START=menu gets the title
// to its dossier normally, then GOLDENEYE_AUTO_MISSION=dam contributes five
// ordinary BTN_A edges for the default path Select Mission -> Dam -> Agent ->
// open the briefing, then contributes a normal left-stick-up cursor move before
// the final A activates Start. Both environment variables are opt-in; neither
// changes normal/native input.
//
// The state machine starts each interval from the poll that actually delivered
// the previous edge. Consequently, even if the guest stalls across a deadline,
// every press is followed by at least one release poll before the next press.
enum class AutoMissionDamPhase : uint8_t {
  kWaitForDossier,
  kPress,
  kRelease,
  kStickUp,
  kStickReleased,
  kDone,
};

struct AutoMissionDamInput {
  bool press_a = false;
  bool stick_up = false;
};

struct AutoMissionDamState {
  std::mutex mutex;
  std::chrono::steady_clock::time_point started_at = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point phase_started_at = started_at;
  AutoMissionDamPhase phase = AutoMissionDamPhase::kWaitForDossier;
  uint32_t pulse_index = 0;
  bool armed_logged = false;
};

AutoMissionDamInput ge_auto_mission_dam_input(const char* mode) {
  if (!mode || std::strcmp(mode, "dam") != 0) {
    return {};
  }

  constexpr auto kDossierSettleDelay = std::chrono::seconds(22);
  constexpr auto kPressDuration = std::chrono::milliseconds(200);
  constexpr auto kReleaseDuration = std::chrono::milliseconds(2500);
  // At full deflection, 450 ms moves the 1280x720 dossier cursor roughly the
  // 170 logical pixels (about 275 window pixels) from NEXT to START. Observe a
  // neutral poll, then leave 750 ms for the hover/focus state to settle.
  constexpr auto kStickUpDuration = std::chrono::milliseconds(450);
  constexpr auto kStickReleaseSettle = std::chrono::milliseconds(750);
  constexpr uint32_t kPulseCount = 5;
  constexpr const char* kPulseTargets[] = {"Select Mission", "Dam", "Agent", "Open briefing",
                                           "Start mission"};
  static_assert(std::size(kPulseTargets) == kPulseCount);

  static AutoMissionDamState state;
  std::lock_guard<std::mutex> lock(state.mutex);
  auto now = std::chrono::steady_clock::now();

  // Edge-only logging is inherently rate-limited: one arm line, one line per
  // press/release, and one completion line for the process lifetime.
  if (!state.armed_logged) {
    state.armed_logged = true;
    std::fprintf(stderr,
                 "[ge] GOLDENEYE_AUTO_MISSION=dam armed: input-only; waiting 22s for dossier; "
                 "five BTN_A pulses with cursor-up before final A\n");
    std::fflush(stderr);
  }

  switch (state.phase) {
    case AutoMissionDamPhase::kWaitForDossier:
      if (now - state.started_at < kDossierSettleDelay) {
        return {};
      }
      state.pulse_index = 0;
      state.phase = AutoMissionDamPhase::kPress;
      state.phase_started_at = now;
      std::fprintf(stderr, "[ge] GOLDENEYE_AUTO_MISSION=dam BTN_A press %u/%u target=\"%s\"\n",
                   state.pulse_index + 1, kPulseCount, kPulseTargets[state.pulse_index]);
      std::fflush(stderr);
      return {.press_a = true};

    case AutoMissionDamPhase::kPress:
      if (now - state.phase_started_at < kPressDuration) {
        return {.press_a = true};
      }
      // Do not clear BTN_A here: returning false merely stops contributing our
      // bit, preserving a real controller or native keyboard A that is held.
      state.phase = AutoMissionDamPhase::kRelease;
      state.phase_started_at = now;
      std::fprintf(stderr, "[ge] GOLDENEYE_AUTO_MISSION=dam BTN_A release %u/%u target=\"%s\"\n",
                   state.pulse_index + 1, kPulseCount, kPulseTargets[state.pulse_index]);
      if (state.pulse_index + 1 == std::size(kPulseTargets)) {
        state.phase = AutoMissionDamPhase::kDone;
        std::fprintf(stderr,
                     "[ge] GOLDENEYE_AUTO_MISSION=dam complete: %u/%u pulses released; "
                     "injector permanently idle\n",
                     kPulseCount, kPulseCount);
      }
      std::fflush(stderr);
      return {};

    case AutoMissionDamPhase::kRelease:
      if (now - state.phase_started_at < kReleaseDuration) {
        return {};
      }
      if (state.pulse_index == 3) {
        state.phase = AutoMissionDamPhase::kStickUp;
        state.phase_started_at = now;
        std::fprintf(stderr,
                     "[ge] GOLDENEYE_AUTO_MISSION=dam left-stick UP press before final A "
                     "ly=32767 duration_ms=450\n");
        std::fflush(stderr);
        return {.stick_up = true};
      }
      ++state.pulse_index;
      state.phase = AutoMissionDamPhase::kPress;
      state.phase_started_at = now;
      std::fprintf(stderr, "[ge] GOLDENEYE_AUTO_MISSION=dam BTN_A press %u/%u target=\"%s\"\n",
                   state.pulse_index + 1, kPulseCount, kPulseTargets[state.pulse_index]);
      std::fflush(stderr);
      return {.press_a = true};

    case AutoMissionDamPhase::kStickUp:
      if (now - state.phase_started_at < kStickUpDuration) {
        return {.stick_up = true};
      }
      // As with BTN_A, release means stop contributing the synthetic axis. The
      // freshly polled native value remains untouched on this and later polls.
      state.phase = AutoMissionDamPhase::kStickReleased;
      state.phase_started_at = now;
      std::fprintf(stderr,
                   "[ge] GOLDENEYE_AUTO_MISSION=dam left-stick UP release before final A\n");
      std::fflush(stderr);
      return {};

    case AutoMissionDamPhase::kStickReleased:
      if (now - state.phase_started_at < kStickReleaseSettle) {
        return {};
      }
      state.pulse_index = 4;
      state.phase = AutoMissionDamPhase::kPress;
      state.phase_started_at = now;
      std::fprintf(stderr, "[ge] GOLDENEYE_AUTO_MISSION=dam BTN_A press %u/%u target=\"%s\"\n",
                   state.pulse_index + 1, kPulseCount, kPulseTargets[state.pulse_index]);
      std::fflush(stderr);
      return {.press_a = true};

    case AutoMissionDamPhase::kDone:
      return {};
  }
  return {};
}

bool ge_input_active() {  // keyboard counts only when focused + not in the menu
  return !g_mouselook_suppressed.load(std::memory_order_relaxed) && ge_game_has_focus();
}

// Is any key bound to cvar `name` held down? The bind may list SEVERAL keys
// separated by commas (#63 "multiple keys per function", e.g. "W,Up") -- held if
// ANY of them is down. Each key name parses to a virtual key (== Windows VK code).
bool ge_key_down(const char* name) {
  std::string binds = rex::cvar::GetFlagByName(name);
  if (binds.empty())
    return false;
  size_t start = 0;
  while (start <= binds.size()) {
    size_t comma = binds.find(',', start);
    std::string one =
        binds.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
    while (!one.empty() && (one.front() == ' ' || one.front() == '\t'))
      one.erase(one.begin());
    while (!one.empty() && (one.back() == ' ' || one.back() == '\t'))
      one.pop_back();
    if (!one.empty()) {
      rex::ui::VirtualKey vk = rex::ui::ParseVirtualKey(one);
#if defined(_WIN32)
      if (vk != rex::ui::VirtualKey::kNone &&
          (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0)
        return true;
#else
      (void)vk;
#endif
    }
    if (comma == std::string::npos)
      break;
    start = comma + 1;
  }
  return false;
}
}  // namespace

// Keyboard binds. Defaults are a placeholder layout; the user's preferred layout
// is hard-set at boot (ge_app OnConfigurePaths) and rebindable in the menu.
REXCVAR_DEFINE_BOOL(ge_keyboard_enable, true, "Input", "Map keyboard keys to controller buttons");
REXCVAR_DEFINE_STRING(ge_key_mv_up, "W", "Input/Keybinds", "Move forward (left stick up)");
REXCVAR_DEFINE_STRING(ge_key_mv_down, "S", "Input/Keybinds", "Move back (left stick down)");
REXCVAR_DEFINE_STRING(ge_key_mv_left, "A", "Input/Keybinds", "Move left (left stick left)");
REXCVAR_DEFINE_STRING(ge_key_mv_right, "D", "Input/Keybinds", "Move right (left stick right)");
REXCVAR_DEFINE_STRING(ge_key_a, "Space", "Input/Keybinds", "A button");
REXCVAR_DEFINE_STRING(ge_key_b, "Control", "Input/Keybinds", "B button");
REXCVAR_DEFINE_STRING(ge_key_x, "R", "Input/Keybinds", "X button");
REXCVAR_DEFINE_STRING(ge_key_y, "E", "Input/Keybinds", "Y button");
REXCVAR_DEFINE_STRING(ge_key_lt, "RMB", "Input/Keybinds", "Left trigger");
REXCVAR_DEFINE_STRING(ge_key_rt, "LMB", "Input/Keybinds", "Right trigger");
REXCVAR_DEFINE_STRING(ge_key_lb, "Q", "Input/Keybinds", "Left shoulder");
REXCVAR_DEFINE_STRING(ge_key_rb, "F", "Input/Keybinds", "Right shoulder");
REXCVAR_DEFINE_STRING(ge_key_l3, "C", "Input/Keybinds", "Left stick press");
REXCVAR_DEFINE_STRING(ge_key_r3, "V", "Input/Keybinds", "Right stick press");
REXCVAR_DEFINE_STRING(ge_key_dup, "Up", "Input/Keybinds", "D-pad up");
REXCVAR_DEFINE_STRING(ge_key_ddown, "Down", "Input/Keybinds", "D-pad down");
REXCVAR_DEFINE_STRING(ge_key_dleft, "Left", "Input/Keybinds", "D-pad left");
REXCVAR_DEFINE_STRING(ge_key_dright, "Right", "Input/Keybinds", "D-pad right");
REXCVAR_DEFINE_STRING(ge_key_start, "Return", "Input/Keybinds", "Start button");
REXCVAR_DEFINE_STRING(ge_key_back, "Tab", "Input/Keybinds", "Back button");
// Right analog stick (look/aim) as keyboard binds (#63). Unbound by default so
// they never fight mouse-look; bind them (e.g. arrow keys) for keyboard-only look.
// They feed the guest's native right-stick, so they work alongside the mouse.
REXCVAR_DEFINE_STRING(ge_key_look_up, "", "Input/Keybinds", "Look up (right stick up)");
REXCVAR_DEFINE_STRING(ge_key_look_down, "", "Input/Keybinds", "Look down (right stick down)");
REXCVAR_DEFINE_STRING(ge_key_look_left, "", "Input/Keybinds", "Look left (right stick left)");
REXCVAR_DEFINE_STRING(ge_key_look_right, "", "Input/Keybinds", "Look right (right stick right)");

ge::host_pause::InputSample ge_resume_input_sample(uint8_t* base) {
  // Raw SDL state stays visible while host UI suppresses guest input. Merge it
  // with the already-polled guest pad so keyboard/mouse emulation is covered as
  // soon as it becomes active again.
  ge::host_pause::InputSample result;
  auto merge_axis = [](int16_t* destination, int16_t candidate) {
    if (std::abs(static_cast<int>(candidate)) > std::abs(static_cast<int>(*destination))) {
      *destination = candidate;
    }
  };
  for (uint32_t user_index = 0; user_index < GE_LOCAL_PAD_COUNT; ++user_index) {
    const ge::host_pause::InputSample physical = ge_physical_input_sample(user_index);
    result.buttons |= physical.buttons;
    result.left_trigger = std::max(result.left_trigger, physical.left_trigger);
    result.right_trigger = std::max(result.right_trigger, physical.right_trigger);
    merge_axis(&result.thumb_lx, physical.thumb_lx);
    merge_axis(&result.thumb_ly, physical.thumb_ly);
    merge_axis(&result.thumb_rx, physical.thumb_rx);
    merge_axis(&result.thumb_ry, physical.thumb_ry);

    const uint32_t pad = GE_PAD0 + user_index * GE_PAD_STRIDE;
    result.buttons |= LD16(base, pad + 0);
    result.left_trigger = std::max(result.left_trigger, base[pad + 2]);
    result.right_trigger = std::max(result.right_trigger, base[pad + 3]);
    merge_axis(&result.thumb_lx, static_cast<int16_t>(LD16(base, pad + 4)));
    merge_axis(&result.thumb_ly, static_cast<int16_t>(LD16(base, pad + 6)));
    merge_axis(&result.thumb_rx, static_cast<int16_t>(LD16(base, pad + 8)));
    merge_axis(&result.thumb_ry, static_cast<int16_t>(LD16(base, pad + 10)));
  }

#if defined(_WIN32)
  // Win32's title-specific keyboard path is injected below this transition
  // gate, so sample its physical binds explicitly as well.
  if (REXCVAR_GET(ge_keyboard_enable)) {
    auto merge_button = [&](const char* cvar, uint16_t button) {
      if (ge_key_down(cvar)) {
        result.buttons |= button;
      }
    };
    merge_button("ge_key_a", BTN_A);
    merge_button("ge_key_b", BTN_B);
    merge_button("ge_key_x", BTN_X);
    merge_button("ge_key_y", BTN_Y);
    merge_button("ge_key_lb", BTN_LSHOULDER);
    merge_button("ge_key_rb", BTN_RSHOULDER);
    merge_button("ge_key_l3", BTN_LTHUMB);
    merge_button("ge_key_r3", BTN_RTHUMB);
    merge_button("ge_key_dup", BTN_DPAD_UP);
    merge_button("ge_key_ddown", BTN_DPAD_DOWN);
    merge_button("ge_key_dleft", BTN_DPAD_LEFT);
    merge_button("ge_key_dright", BTN_DPAD_RIGHT);
    merge_button("ge_key_start", BTN_START);
    merge_button("ge_key_back", BTN_BACK);
    result.left_trigger = ge_key_down("ge_key_lt") ? UINT8_MAX : result.left_trigger;
    result.right_trigger = ge_key_down("ge_key_rt") ? UINT8_MAX : result.right_trigger;
    if (ge_key_down("ge_key_mv_left"))
      result.thumb_lx = -INT16_MAX;
    if (ge_key_down("ge_key_mv_right"))
      result.thumb_lx = INT16_MAX;
    if (ge_key_down("ge_key_mv_up"))
      result.thumb_ly = INT16_MAX;
    if (ge_key_down("ge_key_mv_down"))
      result.thumb_ly = -INT16_MAX;
    if (ge_key_down("ge_key_look_left"))
      result.thumb_rx = -INT16_MAX;
    if (ge_key_down("ge_key_look_right"))
      result.thumb_rx = INT16_MAX;
    if (ge_key_down("ge_key_look_up"))
      result.thumb_ry = INT16_MAX;
    if (ge_key_down("ge_key_look_down"))
      result.thumb_ry = -INT16_MAX;
  }
#endif
  return result;
}

// Runs once per controller poll, after XamInputGetState fills the slot-0 buffer
// and before the guest dispatches it. OR our keyboard buttons in, and set the
// left stick / triggers when their keys are held (pad input is preserved).
void ge_mouse_camera(uint8_t* base, rex::input::MouseMotionDelta mouse_delta);  // defined above
void ge_apply_ce_data_patches(uint8_t* base);                                   // ge_ce_patches.cpp

void ge_inject_keyboard(PPCRegister& /*r11*/) {
  PPCContext* ctx;
  uint8_t* base;
  getcb(ctx, base);
  (void)ctx;

  // Apply BeanTools community DATA bug-fixes once, before any level loads its
  // setup/fog/BG data. The data segment is live in guest RAM by the first input
  // poll (menu), which precedes any level load.
  static bool ce_patched = false;
  if (!ce_patched) {
    ce_patched = true;
    ge_apply_ce_data_patches(base);
    REXKRNL_INFO("GECE community data bug-fixes applied");
  }

  // Host UI requests are consumed here because this hook runs on GoldenEye's
  // game thread. The bridge preserves the live PPC register context around the
  // verified retail routines it invokes.
  const ge::host_pause::ProcessResult pause_result = ge::host_pause::ProcessGameThread(*ctx, base);
  ge::testing::ProcessTestingToolRequests(*ctx, base);

  // The host menu may close while a controller, mouse button or keyboard key is
  // still held. Wait for complete neutral input, keep the final neutral poll
  // swallowed, and discard mouse motion accumulated behind the transition.
  // This runs only after the retail resume is acknowledged.
  static ge::host_pause::ResumeInputLatch resume_input_latch;
  if (pause_result.input_resume_pulse || resume_input_latch.active()) {
    const ge::host_pause::InputSample physical_input = ge_resume_input_sample(base);
    if (pause_result.input_resume_pulse) {
      resume_input_latch.Arm(physical_input);
    }
    if (resume_input_latch.ShouldSuppress(physical_input)) {
      ge_clear_guest_pads(base);
      ge_discard_mouse_motion();
      return;
    }
  }

  // Rebind capture: the menu is listening for a key to bind. Swallow every
  // local controller slot (buttons, triggers, both sticks) so the key/button being
  // bound doesn't also drive the game, and skip keyboard injection + mouse-look.
  if (g_rebind_capturing.load(std::memory_order_relaxed)) {
    ge_clear_guest_pads(base);
    return;
  }

  // Windows submits raw input directly. GoldenEye's macOS app submits native
  // relative window events and tells the common MnK driver not to duplicate
  // that motion as a guest right-stick pulse.
#if defined(_WIN32)
  ge_start_mouse_once();
  if (ge_mouse_active())
    ge_mouse_camera(base, ge_take_mouse_delta());
#elif defined(__APPLE__)
  rex::input::MouseMotionDelta mouse_delta = {};
  auto* runtime = rex::Runtime::instance();
  auto* input = runtime && runtime->input_system()
                    ? static_cast<rex::input::InputSystem*>(runtime->input_system())
                    : nullptr;
  if (input && input->ConsumeApplicationMouseMotion(0, &mouse_delta) &&
      !g_mouselook_suppressed.load(std::memory_order_relaxed)) {
    ge_mouse_camera(base, mouse_delta);
  }
#endif

  static std::atomic<uint32_t> auto_start_poll{0};
  const char* auto_start_mode = std::getenv("GOLDENEYE_AUTO_START");
  if (auto_start_mode) {
    uint32_t input_poll = auto_start_poll.fetch_add(1, std::memory_order_relaxed) + 1;
    if (ge_auto_start_pressed(auto_start_mode, input_poll)) {
      ST16(base, GE_PAD0 + 0, LD16(base, GE_PAD0 + 0) | BTN_START);
      static std::atomic<bool> logged_auto_start{false};
      bool expected = false;
      if (logged_auto_start.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        std::fprintf(stderr, "[ge] GOLDENEYE_AUTO_START=%s injecting Start\n", auto_start_mode);
        std::fflush(stderr);
      }
    }
  }

  const char* auto_mission_mode = std::getenv("GOLDENEYE_AUTO_MISSION");
  AutoMissionDamInput auto_mission = ge_auto_mission_dam_input(auto_mission_mode);
  if (auto_mission.press_a) {
    ST16(base, GE_PAD0 + 0, LD16(base, GE_PAD0 + 0) | BTN_A);
  }
  if (auto_mission.stick_up) {
    // Merge in the requested positive direction instead of clearing/replacing
    // the whole gamepad. A stronger positive native value wins (32767 is the
    // diagnostic maximum), and release performs no write at all.
    constexpr int16_t kAutoMissionStickUp = 32767;
    int16_t native_ly = static_cast<int16_t>(LD16(base, GE_PAD0 + 6));
    int16_t merged_ly = std::max(native_ly, kAutoMissionStickUp);
    if (merged_ly != native_ly) {
      ST16(base, GE_PAD0 + 6, static_cast<uint16_t>(merged_ly));
    }
  }

  if (!REXCVAR_GET(ge_keyboard_enable) || !ge_input_active())
    return;

  uint16_t add = 0;
  if (ge_key_down("ge_key_a"))
    add |= BTN_A;
  if (ge_key_down("ge_key_b"))
    add |= BTN_B;
  if (ge_key_down("ge_key_x"))
    add |= BTN_X;
  if (ge_key_down("ge_key_y"))
    add |= BTN_Y;
  if (ge_key_down("ge_key_lb"))
    add |= BTN_LSHOULDER;
  if (ge_key_down("ge_key_rb"))
    add |= BTN_RSHOULDER;
  if (ge_key_down("ge_key_l3"))
    add |= BTN_LTHUMB;
  if (ge_key_down("ge_key_r3"))
    add |= BTN_RTHUMB;
  if (ge_key_down("ge_key_dup"))
    add |= BTN_DPAD_UP;
  if (ge_key_down("ge_key_ddown"))
    add |= BTN_DPAD_DOWN;
  if (ge_key_down("ge_key_dleft"))
    add |= BTN_DPAD_LEFT;
  if (ge_key_down("ge_key_dright"))
    add |= BTN_DPAD_RIGHT;
  if (ge_key_down("ge_key_start"))
    add |= BTN_START;
  if (ge_key_down("ge_key_back"))
    add |= BTN_BACK;
  if (add)
    ST16(base, GE_PAD0 + 0, LD16(base, GE_PAD0 + 0) | add);

  if (ge_key_down("ge_key_lt"))
    base[GE_PAD0 + 2] = 0xFF;
  if (ge_key_down("ge_key_rt"))
    base[GE_PAD0 + 3] = 0xFF;

  int16_t lx = 0, ly = 0;
  if (ge_key_down("ge_key_mv_left"))
    lx = -32767;
  if (ge_key_down("ge_key_mv_right"))
    lx = 32767;
  if (ge_key_down("ge_key_mv_up"))
    ly = 32767;
  if (ge_key_down("ge_key_mv_down"))
    ly = -32767;
  if (lx)
    ST16(base, GE_PAD0 + 4, static_cast<uint16_t>(lx));
  if (ly)
    ST16(base, GE_PAD0 + 6, static_cast<uint16_t>(ly));

  // Right stick (look/aim) -> slot-0 gamepad RX(+8)/RY(+10), s16 BE (#63). Feeds
  // the guest's native right-stick look, so it coexists with mouse-look.
  int16_t rx = 0, ry = 0;
  if (ge_key_down("ge_key_look_left"))
    rx = -32767;
  if (ge_key_down("ge_key_look_right"))
    rx = 32767;
  if (ge_key_down("ge_key_look_up"))
    ry = 32767;
  if (ge_key_down("ge_key_look_down"))
    ry = -32767;
  if (rx)
    ST16(base, GE_PAD0 + 8, static_cast<uint16_t>(rx));
  if (ry)
    ST16(base, GE_PAD0 + 10, static_cast<uint16_t>(ry));
}

namespace {
constexpr uint32_t kGeProtectedLowPageEnd = 0x10000u;

constexpr bool ge_packed_data_load_needs_recovery(uint32_t callback_result, uint32_t load_address) {
  return callback_result == 0 || load_address < kGeProtectedLowPageEnd;
}

static_assert(ge_packed_data_load_needs_recovery(0, 2));
static_assert(ge_packed_data_load_needs_recovery(1, 3));
static_assert(ge_packed_data_load_needs_recovery(0xFFFFFFFFu, 1));
static_assert(!ge_packed_data_load_needs_recovery(0x10000u, 0x10002u));

bool ge_should_log_sparse_recovery(uint64_t hit) {
  // Keep the first reports detailed, then retain sparse evidence if a bad
  // resource remains active for many frames without flooding the tester log.
  return hit <= 16 || (hit & (hit - 1)) == 0;
}

struct GeCleanupCallbackSnapshot {
  uint64_t r1;
  uint64_t r26;
  uint64_t r27;
  uint64_t r28;
  uint64_t r29;
  uint64_t r30;
  uint64_t r31;
  uint32_t object;
  uint32_t callback;
};

constexpr size_t kGeCleanupCallbackMaximumDepth = 64;

struct GeCleanupCallbackStack {
  std::array<GeCleanupCallbackSnapshot, kGeCleanupCallbackMaximumDepth> entries{};
  size_t depth = 0;
  size_t overflow_depth = 0;
};

// A callback may recursively run the same cleanup path. Keep an allocation-free
// stack per host thread so every ordinary nested return restores its own caller
// without risking a heap exception while the guest holds two critical sections.
thread_local GeCleanupCallbackStack g_ge_cleanup_callback_stack;
std::atomic<uint64_t> g_ge_cleanup_callback_repairs{0};
std::atomic<uint64_t> g_ge_cleanup_callback_stack_errors{0};

void ge_log_cleanup_callback_stack_error(std::string_view reason, uint32_t guest_sp, size_t depth) {
  const uint64_t hit =
      g_ge_cleanup_callback_stack_errors.fetch_add(1, std::memory_order_relaxed) + 1;
  if (!ge_should_log_sparse_recovery(hit)) {
    return;
  }
  REXKRNL_WARN("[GE-GUARD-823CFC00-v1] callback snapshot {} hit={} depth={} guest_sp=0x{:08X}",
               reason, hit, depth, guest_sp);
  if (auto* logger = rex::GetLoggerRaw(rex::log::krnl())) {
    logger->flush();
  }
}

bool ge_recover_packed_data_load(uint32_t load_site, uint32_t callback_result,
                                 uint32_t stream_offset, uint32_t load_address, uint32_t owner,
                                 uint32_t guest_sp, std::atomic<uint64_t>& hits,
                                 PPCRegister& result) {
  if (!ge_packed_data_load_needs_recovery(callback_result, load_address)) {
    return false;
  }

  // Callers already interpret a zero return as an empty packed-data result.
  // The configured hook then jumps to sub_823DFB70's normal epilogue so its
  // guest stack frame and saved registers are restored exactly as usual.
  result.u64 = 0;

  const uint64_t hit = hits.fetch_add(1, std::memory_order_relaxed) + 1;
  if (ge_should_log_sparse_recovery(hit)) {
    const char* reason = callback_result == 0 ? "null-callback" : "protected-low-page";
    REXKRNL_WARN(
        "[GE-GUARD-823DFB70-v1] recovered site=0x{:08X} hit={} reason={} "
        "owner=0x{:08X} callback=0x{:08X} stream_offset=0x{:08X} "
        "load=0x{:08X} guest_sp=0x{:08X}",
        load_site, hit, reason, owner, callback_result, stream_offset, load_address, guest_sp);
    if (auto* logger = rex::GetLoggerRaw(rex::log::krnl())) {
      logger->flush();
    }
  }
  return true;
}

bool ge_recover_packed_data_purecall(uint32_t call_site, uint32_t callback_target,
                                     uint32_t stream_offset, uint32_t owner, uint32_t guest_sp,
                                     std::atomic<uint64_t>& hits, PPCRegister& result) {
  if (!ge::crash_guards::RecoverPackedDataPureVirtualDispatch(callback_target, result.u64)) {
    return false;
  }

  // A resource whose vtable+16 slot has become _purecall is already being
  // destroyed or is otherwise unavailable. This accessor already defines zero
  // as "no packed data", so return that value before the title enters its CRT
  // termination path. The configured hook jumps through the original epilogue.
  const uint64_t hit = hits.fetch_add(1, std::memory_order_relaxed) + 1;
  if (ge_should_log_sparse_recovery(hit)) {
    REXKRNL_WARN(
        "[GE-GUARD-823DFB70-purecall-v1] recovered site=0x{:08X} hit={} "
        "owner=0x{:08X} callback=0x{:08X} stream_offset=0x{:08X} guest_sp=0x{:08X}",
        call_site, hit, owner, callback_target, stream_offset, guest_sp);
    if (auto* logger = rex::GetLoggerRaw(rex::log::krnl())) {
      logger->flush();
    }
  }
  return true;
}
}  // namespace

// sub_823CFC00 walks two intrusive cleanup lists while holding the owner's
// critical sections. The virtual cleanup callback at 0x823CFC84 is required by
// the PPC ABI to preserve r1 and r26-r31. At least one real callback path
// returns with r28 cleared, which turns the next owner load at 0x823CFC94 into
// a protected-low-page access at guest 0x70. Snapshot every live nonvolatile
// register immediately before the callback and restore it immediately after;
// this preserves the original loop and its ordinary lock-release epilogue.
void ge_cleanup_callback_enter(PPCRegister& r1, PPCRegister& r26, PPCRegister& r27,
                               PPCRegister& r28, PPCRegister& r29, PPCRegister& r30,
                               PPCRegister& r31, PPCRegister& r3, PPCRegister& r11) {
  if (g_ge_cleanup_callback_stack.overflow_depth != 0 ||
      g_ge_cleanup_callback_stack.depth == kGeCleanupCallbackMaximumDepth) {
    ++g_ge_cleanup_callback_stack.overflow_depth;
    ge_log_cleanup_callback_stack_error("overflow", r1.u32, g_ge_cleanup_callback_stack.depth);
    return;
  }
  g_ge_cleanup_callback_stack.entries[g_ge_cleanup_callback_stack.depth++] = {
      r1.u64, r26.u64, r27.u64, r28.u64, r29.u64, r30.u64, r31.u64, r3.u32, r11.u32,
  };
}

void ge_cleanup_callback_leave(PPCRegister& r1, PPCRegister& r26, PPCRegister& r27,
                               PPCRegister& r28, PPCRegister& r29, PPCRegister& r30,
                               PPCRegister& r31) {
  if (g_ge_cleanup_callback_stack.overflow_depth != 0) {
    --g_ge_cleanup_callback_stack.overflow_depth;
    return;
  }
  if (g_ge_cleanup_callback_stack.depth == 0) {
    ge_log_cleanup_callback_stack_error("underflow", r1.u32, 0);
    return;
  }

  const GeCleanupCallbackSnapshot saved =
      g_ge_cleanup_callback_stack.entries[--g_ge_cleanup_callback_stack.depth];
  uint32_t changed = 0;
  changed |= r1.u64 != saved.r1 ? 1u << 0 : 0;
  changed |= r26.u64 != saved.r26 ? 1u << 1 : 0;
  changed |= r27.u64 != saved.r27 ? 1u << 2 : 0;
  changed |= r28.u64 != saved.r28 ? 1u << 3 : 0;
  changed |= r29.u64 != saved.r29 ? 1u << 4 : 0;
  changed |= r30.u64 != saved.r30 ? 1u << 5 : 0;
  changed |= r31.u64 != saved.r31 ? 1u << 6 : 0;

  const uint64_t returned_r28 = r28.u64;
  r1.u64 = saved.r1;
  r26.u64 = saved.r26;
  r27.u64 = saved.r27;
  r28.u64 = saved.r28;
  r29.u64 = saved.r29;
  r30.u64 = saved.r30;
  r31.u64 = saved.r31;

  if (changed == 0) {
    return;
  }
  const uint64_t hit = g_ge_cleanup_callback_repairs.fetch_add(1, std::memory_order_relaxed) + 1;
  if (!ge_should_log_sparse_recovery(hit)) {
    return;
  }
  REXKRNL_WARN(
      "[GE-GUARD-823CFC00-v1] repaired callback ABI hit={} changed=0x{:02X} "
      "owner=0x{:08X} object=0x{:08X} callback=0x{:08X} "
      "returned_r28=0x{:08X} guest_sp=0x{:08X}",
      hit, changed, static_cast<uint32_t>(saved.r28), saved.object, saved.callback,
      static_cast<uint32_t>(returned_r28), static_cast<uint32_t>(saved.r1));
  if (auto* logger = rex::GetLoggerRaw(rex::log::krnl())) {
    logger->flush();
  }
}

// Guard both vtable+16 calls before dispatch. A purecall target here means this
// packed-data resource has reached a base/destructed state. Returning the
// accessor's existing empty value is safe; intercepting _purecall globally is
// deliberately avoided.
bool ge_skip_packed_data_purecall_header(PPCRegister& r11, PPCRegister& r3, PPCRegister& r31,
                                         PPCRegister& r1) {
  static std::atomic<uint64_t> hits{0};
  return ge_recover_packed_data_purecall(0x823DFBA8u, r11.u32, 0, r31.u32, r1.u32, hits, r3);
}

bool ge_skip_packed_data_purecall_value(PPCRegister& r11, PPCRegister& r3, PPCRegister& r30,
                                        PPCRegister& r31, PPCRegister& r1) {
  static std::atomic<uint64_t> hits{0};
  return ge_recover_packed_data_purecall(0x823DFBD0u, r11.u32, r30.u32, r31.u32, r1.u32, hits, r3);
}

// First packed-data callback load: lbz r11,2(r3) at 0x823DFBAC.
bool ge_guard_packed_data_header(PPCRegister& r3, PPCRegister& r31, PPCRegister& r1) {
  static std::atomic<uint64_t> hits{0};
  const uint32_t callback_result = r3.u32;
  const uint32_t load_address = callback_result + 2u;
  return ge_recover_packed_data_load(0x823DFBACu, callback_result, 0, load_address, r31.u32, r1.u32,
                                     hits, r3);
}

// Second packed-data callback load: lbz r3,2(r3+r30) at 0x823DFBD8. The hook
// runs at 0x823DFBD4, before the add, so no invalid effective address is formed.
bool ge_guard_packed_data_value(PPCRegister& r3, PPCRegister& r30, PPCRegister& r31,
                                PPCRegister& r1) {
  static std::atomic<uint64_t> hits{0};
  const uint32_t callback_result = r3.u32;
  const uint32_t stream_offset = r30.u32;
  const uint32_t load_address = callback_result + stream_offset + 2u;
  return ge_recover_packed_data_load(0x823DFBD8u, callback_result, stream_offset, load_address,
                                     r31.u32, r1.u32, hits, r3);
}

// Startup asset/property table path: sub_823AE250 stores into a lookup result at
// 0x823AE290. Missing optional entries can return null; on macOS that becomes a
// host SIGBUS at guest base+0x14. Skip only the store and let the caller continue.
bool ge_skip_null_attr_store(PPCRegister& r11) {
  if (r11.u32 != 0) {
    return false;
  }
  static std::atomic<uint32_t> logs{0};
  if (logs.fetch_add(1, std::memory_order_relaxed) < 16) {
    REXKRNL_WARN("GE startup: skipped null attr store in sub_823AE250");
  }
  return true;
}

bool ge_skip_null_attr_first(PPCRegister& r3) {
  if (r3.u32 != 0) {
    return false;
  }
  static std::atomic<uint32_t> logs{0};
  if (logs.fetch_add(1, std::memory_order_relaxed) < 16) {
    REXKRNL_WARN("GE startup: skipped null attr first-field load");
  }
  return true;
}

bool ge_skip_null_attr_second(PPCRegister& r3) {
  if (r3.u32 != 0) {
    return false;
  }
  static std::atomic<uint32_t> logs{0};
  if (logs.fetch_add(1, std::memory_order_relaxed) < 16) {
    REXKRNL_WARN("GE startup: skipped null attr second-field load");
  }
  return true;
}

bool ge_skip_null_attr_reloc_table(PPCRegister& r8) {
  if (r8.u32 != 0) {
    return false;
  }
  static std::atomic<uint32_t> logs{0};
  if (logs.fetch_add(1, std::memory_order_relaxed) < 16) {
    REXKRNL_WARN("GE startup: skipped null attr relocation table");
  }
  return true;
}

bool ge_skip_null_memset_dst(PPCRegister& r3) {
  if (r3.u32 != 0) {
    return false;
  }
  static std::atomic<uint32_t> logs{0};
  if (logs.fetch_add(1, std::memory_order_relaxed) < 16) {
    REXKRNL_WARN("GE startup: skipped memset with null destination");
  }
  return true;
}

bool ge_skip_null_memcpy_ptr(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5) {
  if (r5.u32 != 0 && r3.u32 != 0 && r4.u32 != 0) {
    return false;
  }
  static std::atomic<uint32_t> logs{0};
  if (logs.fetch_add(1, std::memory_order_relaxed) < 16) {
    REXKRNL_WARN("GE startup: skipped memcpy with null pointer or zero length");
  }
  return true;
}

bool ge_skip_null_bitset_write(PPCRegister& r3, PPCRegister& r4) {
  // This hook only exists to bypass a null startup edge case. Bitset helpers
  // are hot during title-data loading, so valid calls must not pay for atomic
  // diagnostics on every bit operation.
  if (r3.u32 != 0) {
    return false;
  }
  static std::atomic<uint32_t> calls{0};
  uint32_t call_index = calls.fetch_add(1, std::memory_order_relaxed) + 1;
  if (call_index <= 48 || (call_index & 0x3FFFF) == 0) {
    std::fprintf(stderr, "[ge] bitset_write#%u dst=0x%08x bit=%u\n", call_index, r3.u32, r4.u32);
    std::fflush(stderr);
  }
  static std::atomic<uint32_t> logs{0};
  if (logs.fetch_add(1, std::memory_order_relaxed) < 16) {
    REXKRNL_WARN("GE startup: skipped null bitset write");
  }
  return true;
}

bool ge_skip_null_bitset_test(PPCRegister& r3, PPCRegister& r4) {
  // See ge_skip_null_bitset_write: non-null calls are the overwhelmingly
  // common path and must remain effectively free.
  if (r3.u32 != 0) {
    return false;
  }
  static std::atomic<uint32_t> calls{0};
  uint32_t call_index = calls.fetch_add(1, std::memory_order_relaxed) + 1;
  if (call_index <= 48 || (call_index & 0x3FFFF) == 0) {
    std::fprintf(stderr, "[ge] bitset_test#%u src=0x%08x bit=%u\n", call_index, r3.u32, r4.u32);
    std::fflush(stderr);
  }
  static std::atomic<uint32_t> logs{0};
  if (logs.fetch_add(1, std::memory_order_relaxed) < 16) {
    REXKRNL_WARN("GE startup: null bitset test returned false");
  }
  r3.u32 = 0;
  return true;
}

void ge_clamp_startup_list_count(PPCRegister& r10, PPCRegister& r11, PPCRegister& r31) {
  constexpr uint32_t kMaxReasonableStartupItems = 0x10000u;
  if (r10.u32 <= kMaxReasonableStartupItems) {
    return;
  }

  PPCContext* ctx;
  uint8_t* base;
  getcb(ctx, base);
  (void)ctx;

  uint32_t list_base = 0;
  if (r11.u32 != 0) {
    list_base = LD32(base, r11.u32 + 4);
    ST32(base, r11.u32, 0);
  }
  static std::atomic<uint32_t> logs{0};
  if (logs.fetch_add(1, std::memory_order_relaxed) < 32) {
    std::fprintf(stderr,
                 "[ge] clamped startup list count=%u header=0x%08x base=0x%08x owner=0x%08x\n",
                 r10.u32, r11.u32, list_base, r31.u32);
    std::fflush(stderr);
  }
  r10.u32 = 0;
}

bool ge_skip_null_startup_item(PPCRegister& r3) {
  static std::atomic<uint32_t> calls{0};
  uint32_t call_index = calls.fetch_add(1, std::memory_order_relaxed) + 1;
  if (call_index <= 48 || (call_index & 0x3FFFF) == 0) {
    std::fprintf(stderr, "[ge] startup_item#%u ptr=0x%08x\n", call_index, r3.u32);
    std::fflush(stderr);
  }
  if (r3.u32 >= 0x10000u && r3.u32 < 0x60000000u) {
    return false;
  }
  if (r3.u32 >= 0x80000000u && r3.u32 < 0xFFF00000u) {
    return false;
  }
  static std::atomic<uint32_t> logs{0};
  if (logs.fetch_add(1, std::memory_order_relaxed) < 16) {
    REXKRNL_WARN("GE startup: skipped invalid startup list item");
  }
  return true;
}

bool ge_skip_null_state_table(PPCRegister& r11, PPCRegister& r19) {
  if (r19.u32 != 0) {
    return false;
  }
  static std::atomic<uint32_t> logs{0};
  if (logs.fetch_add(1, std::memory_order_relaxed) < 16) {
    REXKRNL_WARN("GE startup: skipped null state table lookup index={:#x}", r11.u32);
  }
  return true;
}

// ===========================================================================
// BeanTools Community Edition CODE fixes (instruction patches replicated as
// midasm hooks; the recomp runs generated C++ so the xex bytes can't be patched
// directly). Addresses/values 1:1 with finalizer.c. Data-only CE fixes live in
// ge_ce_patches.cpp.
// ===========================================================================

// fix_door_volume_clamp @0x820DD814: `li r3,0` -> `li r3,1` (min volume for
// distant doors; 0 overflows). After-hook forces r3 = 1.
void ge_ce_door_vol(PPCRegister& r3) {
  r3.u32 = 1;
}

// remove_beta_string_at_logo @0x820ED678: `ori r3,r3,0x9D97` -> `...0x9CE3`
// (point the GoldenEye-logo string id at the empty string). Replace low half.
void ge_ce_beta_str(PPCRegister& r3) {
  r3.u32 = (r3.u32 & 0xFFFF0000u) | 0x9CE3u;
}

// extend_audio_distance, store site 0x8214438C: original `stfs f0,0x5C(r31)`
// stored a small default scaler; CE makes X3DEmitter->CurveDistanceScaler =
// 6500.0f. Re-store 6500.0f (0x45CB2000) to r31+0x5C after the original store.
void ge_ce_audio_dist(PPCRegister& r31) {
  PPCContext* ctx;
  uint8_t* base;
  getcb(ctx, base);
  (void)ctx;
  ST32(base, r31.u32 + 0x5Cu, 0x45CB2000u);  // 6500.0f
}

// hardcode_near_clip_to_2, per-fog store site 0x82117B44: original
// `stfs f0,0x14(r11)` writes the fog entry's near-clip into the global. CE NOOPs
// it and pins the global to 2.0f. Can't NOOP a store in the recomp, so re-write
// the just-stored slot (r11+0x14 == the near-clip global) back to 2.0f each load.
void ge_ce_near_clip(PPCRegister& r11) {
  PPCContext* ctx;
  uint8_t* base;
  getcb(ctx, base);
  (void)ctx;
  ST32(base, r11.u32 + 0x14u, 0x40000000u);  // 2.0f
}

// remove_original_graphics_mode_blur @0x82188E70: CE NOOPs `bne cr6,+0x19C` so
// the blur path is never taken. Branch-replace -> always fall through.
//   jump_on_true  = 0x8218900C (original target, never taken)
//   jump_on_false = 0x82188E74 (fall through)
bool ge_ce_blur(PPCRegister& /*r3*/) {
  return false;
}

// remove_original_graphics_mode_from_intro @0x8209972C: CE turns
// `bne cr6,0x82099750` into an unconditional `b 0x82099750` so the intro reads
// the current graphics-mode flag. Branch-replace -> always take.
//   jump_on_true  = 0x82099750 (always)
//   jump_on_false = 0x82099730 (unused)
bool ge_ce_intro_gfx(PPCRegister& /*r3*/) {
  return true;
}

// ===========================================================================
// BeanTools Community Edition MP / network hack-functions, re-implemented as
// midasm hooks (the recomp can't add the new 0x830E guest code, so each hack's
// logic is replicated in C++ -- the same pattern as ge_hook_830E0xxx). Game
// functions are called directly via their generated sub_ symbols. 1:1 with
// finalizer.c.
// ===========================================================================
namespace {
constexpr uint32_t GE_NET_FLAG = 0x830CAEA0u;
}  // namespace

// disable_doors_autoclosing_on_mp @0x820E4F1C (after `lwz r11,0xE8(r30)` loads
// the door open-tick): in a network session, force it to 0 so doors never
// auto-close. Outside a session, keep the loaded value.
void ge_ce_mp_door(PPCRegister& r11) {
  PPCContext* ctx;
  uint8_t* base;
  getcb(ctx, base);
  (void)ctx;
  if (base[GE_NET_FLAG] != 0)
    r11.u32 = 0;
}

// disable_player_collisions_for_network_mp @0x820CDFA4 (replaces `bl sub_820B3E90`,
// the player-collision-radius calc): run it normally outside a network session;
// in one, skip it so players pass through each other. The CE hack tail-returns
// from the enclosing function in both cases, so return=true.
void ge_ce_mp_collision() {
  PPCContext* ctx;
  uint8_t* base;
  getcb(ctx, base);
  if (base[GE_NET_FLAG] == 0)
    sub_820B3E90(*ctx, base);
}

// fix_golden_gun_respawn_visiblity_flag @0x820CF940 (replaces cmpwi/bne): keep
// the respawning weapon's invisible flag only for the golden gun in the MWTGG
// scenario (so it stays hidden until grabbed); otherwise clear it so weapons
// reappear. MP scenario id @0x82F61084; weapon id in r11 (golden gun = 0x13).
//   jump_on_true  = 0x820CF948 (keep invisible: GG path)
//   jump_on_false = 0x820CF94C (clear flag: normal path)
bool ge_ce_golden_gun(PPCRegister& r11) {
  PPCContext* ctx;
  uint8_t* base;
  getcb(ctx, base);
  (void)ctx;
  return LD32(base, 0x82F61084u) == 3u && r11.u32 == 0x13u;
}

// make_mp_always_use_p2_fog @0x82117CB0 (after `cmpwi cr6,r3,1` @0x82117CAC; r3 =
// active player count): with 2+ players, force the fog index to 2 (P2 fog) for a
// consistent look; 1 player keeps the original path.
//   jump_on_true  = 0x82117CB8 (2+ players: continue with r3=2)
//   jump_on_false = 0x82117CB4 (1 player: original `li r3,0`)
bool ge_ce_p2_fog(PPCRegister& r3) {
  if (r3.s32 != 1) {
    r3.u32 = 2;
    return true;
  }
  return false;
}

// fix_network_armor_bug @0x8216BC1C (after `stw r12,0x64(r30)`): re-implements
// the CE `cal_dam` armor hack (the patch ships its C source). When an armor prop
// is processed, award it to the NEAREST player within 10m -- fixes armor not
// being granted to remote players in network MP. r30 = armor prop pointer.
// Offsets from armor_fix_code.h: prop.type@+3, prop.pos@+0x58, prop.armorval@+0x84;
// player coords ptr@+0x1AC, coord.pos@+0xC, player.armor@+0x1E8. Float consts:
// 50.0@0x82000B90, 1000.0@0x8200371C (=10m), 1e6@0x82003F0C.
void ge_ce_armor_fix(PPCRegister& r30) {
  PPCContext* ctx;
  uint8_t* base;
  getcb(ctx, base);
  (void)ctx;
  const uint32_t prop = r30.u32;
  if (base[prop + 3] != 0x15)
    return;  // not an armor prop
  const float f50 = LDF32(base, 0x82000B90u);
  const float f1000 = LDF32(base, 0x8200371Cu);
  float nearest = LDF32(base, 0x82003F0Cu);  // 1,000,000
  const float px = LDF32(base, prop + 0x58u);
  const float py = LDF32(base, prop + 0x5Cu);
  const float pz = LDF32(base, prop + 0x60u);
  int pick = -1;
  for (int i = 0; i < 4; ++i) {
    const uint32_t pl = LD32(base, 0x82F1FA98u + i * 4u);
    if (!pl)
      continue;
    const uint32_t coords = LD32(base, pl + 0x1ACu);
    const float dx = LDF32(base, coords + 0x0Cu) - px;
    const float dy = (LDF32(base, coords + 0x10u) - f50) - py;
    const float dz = LDF32(base, coords + 0x14u) - pz;
    const float test = sqrtf(dx * dx + dy * dy + dz * dz);
    if (test < nearest) {
      nearest = test;
      pick = i;
    }
  }
  if (pick < 0 || nearest > f1000)
    return;  // nearest player >10m away (or none)
  const uint32_t winner = LD32(base, 0x82F1FA98u + pick * 4u);
  STF32(base, winner + 0x1E8u, LDF32(base, prop + 0x84u));  // grant armorval
}

// increase_mp_characters: bump the unlocked MP character count from 0x21 to 0x32
// (`li r11,0x21` -> `li r11,0x32`) at the two unlock sites (0x820EF350 SP-clear,
// 0x82106C54 system-link). The new character struct data is written in
// ge_ce_patches.cpp (mpchars_altsandbonus -> 0x8272BA80).
void ge_ce_mp_charcount(PPCRegister& r11) {
  r11.u32 = 0x32u;
}

// add_sfx_to_remote_player_weapons @0x8216E25C (runs BEFORE the original
// `add r11,r10,r11`): play the firing SFX for a REMOTE player's weapon so you
// hear other players shoot online. The CE hack saved/restored every register
// around the SFX calls; we snapshot/restore the whole PPC context so the
// remote-fire (tracer-spawn) function continues undisturbed -- the SFX is a pure
// side effect. r11 = remote player struct pointer.
//   paused flag @0x830633EC; remote-fire gate player+0x2044; old sound-buffer
//   slots player+0xAFC / +0xB00; current weapon player+0x928; weapon-stats array
//   @0x82421968 stride 0x38 (model flag +0x08, stats ptr +0x0C, sound id +0x26);
//   play=sub_82144920, free=sub_82144970/sub_82144A08, set-loc=sub_821448F8;
//   solo-fullscreen screen flag @0x8272B424; player coords player+0x1AC (+0xC).
void ge_ce_remote_weapon_sfx(PPCRegister& r11) {
  PPCContext* ctx;
  uint8_t* base;
  getcb(ctx, base);
  const uint32_t player = r11.u32;
  if (!player)
    return;
  if (LD32(base, 0x830633ECu) != 0)
    return;  // game paused
  if (LD32(base, player + 0x2044u) != 0)
    return;  // remote-fire gate

  PPCContext saved = *ctx;  // the SFX calls clobber volatile regs; restore after

  auto deactivate = [&](uint32_t slot) {
    uint32_t buf = LD32(base, slot);
    if (!buf)
      return;
    ctx->r3.u32 = buf;
    sub_82144970(*ctx, base);
    if (ctx->r3.u32 == 0)
      return;
    ctx->r3.u32 = LD32(base, slot);
    sub_82144A08(*ctx, base);
  };
  deactivate(player + 0x0AFCu);  // free old sound buffer 1
  deactivate(player + 0x0B00u);  // free old sound buffer 2

  const uint32_t snd_slot = player + 0x0B00u;  // play into buffer-2 slot
  const uint32_t channel = 0x1461u;

  const uint32_t weapon = LD32(base, player + 0x928u);
  const uint32_t entry = 0x82421968u + weapon * 0x38u;  // weapon stats entry
  if (LD32(base, entry + 0x08u) != 0) {
    *ctx = saved;
    return;
  }  // no model -> no sfx
  const uint32_t stats = LD32(base, entry + 0x0Cu);
  if (stats == 0) {
    *ctx = saved;
    return;
  }  // null stats
  const uint32_t sound_id = LD16(base, stats + 0x26u);  // weapon sound id
  if ((int32_t)sound_id > 0x105) {
    *ctx = saved;
    return;
  }  // illegal range

  ctx->r3.u32 = LD32(base, 0x83064DE0u);
  ctx->r4.u32 = sound_id;
  ctx->r5.u32 = snd_slot;
  ctx->r6.u32 = LD32(base, 0x83064DE8u);
  ctx->r7.u32 = 0x820036A8u;
  ctx->r8.u32 = channel;
  sub_82144920(*ctx, base);  // play sfx -> r3 = sound buffer
  const uint32_t buf = ctx->r3.u32;
  if (buf != 0 && LD32(base, 0x8272B424u) == 3u) {  // solo full-screen -> 3D pos
    const uint32_t coord = LD32(base, player + 0x01ACu);
    ctx->r3.u32 = buf;
    ctx->r4.u32 = coord + 0x0Cu;
    sub_821448F8(*ctx, base);  // set 3D location
  }

  *ctx = saved;  // restore -> remote-fire function continues unaffected
}

// set_mp_sfx_to_use_player_location: the 4 SFX call sites (gasp 0x820BF264,
// slapper 0x820CDC5C, knife 0x820ACF54, item-equip 0x820AC4D0) all originally
// `bl sub_82144920` (play sfx). CE redirects each through a helper that plays the
// sound AND positions it at the emitting player's 3D location, so in
// split-screen-solo/online you hear other players' actions directionally. This
// hook IS that helper: it plays the sfx (args already in ctx from the caller),
// then sets the 3D location -- but only when another player is the source (not
// the local/active-viewport player, whose own sounds stay centered). Registered
// at all 4 sites with jump_address = site+4 to replace the original bl. No reg
// save needed: the original was itself a bl, so volatiles are already clobbered.
// Shared helper: play the sfx (args already in ctx) and 3D-position it at the
// emitting player -- but only when the source is a NON-local player (the local/
// active-viewport player's own sounds stay centered).
static void ge_ce_play_at_location(PPCContext* ctx, uint8_t* base) {
  sub_82144920(*ctx, base);          // play sfx (caller's args in ctx)
  const uint32_t buf = ctx->r3.u32;  // sound buffer handle
  if (buf == 0)
    return;  // null buffer -> done
  if (LD32(base, 0x8272B424u) != 3u)
    return;  // not solo full-screen view
  if (LD32(base, 0x82F1FA9Cu) == 0 && LD64(base, 0x82F1FAA0u) == 0)
    return;                                      // single-player -> no positioning
  const uint32_t cur = LD32(base, 0x82F1FAACu);  // current player
  if (LD32(base, cur + 0x904u) == 0)
    return;  // local active viewport -> centered
  const uint32_t coord = LD32(base, cur + 0x1ACu);
  ctx->r3.u32 = buf;
  ctx->r4.u32 = coord + 0x0Cu;  // -> player world location
  sub_821448F8(*ctx, base);     // set 3D location
  ctx->r3.u32 = buf;            // leave buffer in r3 for downstream
}

void ge_ce_sfx_3d() {
  PPCContext* ctx;
  uint8_t* base;
  getcb(ctx, base);
  ST32(base, 0x824203ACu, 0);  // your own sound -> your death -> death tune plays
  ge_ce_play_at_location(ctx, base);
}

// Trigger Gasps If Local Player Damaged, Else Argh (set_mp_sfx, gasp half) @
// 0x820BF408 (replaces `bl sub_82144920`). When the damaged player is a REMOTE
// player (solo-fullscreen + MP + not the local viewport + has a model), play a
// gender-appropriate "argh" at their 3D location and flag this death as NOT
// yours (0x824203AC=1) so the death tune is suppressed for it; otherwise it's
// your own gasp (flag=0 -> death tune plays). jump_address skips the original bl.
//   chr = player+0x1AC, model = chr+0x08, bodynum = model+0x0F; body-info array
//   0x82729020 stride 0x24, gender +0x18; argh index female 0x83062BF4 (0..2,
//   +0x0D) / male 0x83062BF8 (0..0x18, +0x86).
void ge_ce_gasp() {
  PPCContext* ctx;
  uint8_t* base;
  getcb(ctx, base);
  uint32_t model = 0;
  if (LD32(base, 0x8272B424u) == 3u) {  // solo full-screen
    const bool mp = (LD32(base, 0x82F1FA9Cu) != 0) || (LD64(base, 0x82F1FAA0u) != 0);
    if (mp) {
      const uint32_t cur = LD32(base, 0x82F1FAACu);
      if (LD32(base, cur + 0x904u) != 0) {  // not the local viewport
        const uint32_t chr = LD32(base, cur + 0x1ACu);
        if (chr)
          model = LD32(base, chr + 0x08u);
      }
    }
  }
  if (model != 0) {               // remote player damaged
    ST32(base, 0x824203ACu, 1u);  // not your death
    const uint32_t bodynum = base[model + 0x0Fu];
    const uint32_t gender = base[0x82729020u + bodynum * 0x24u + 0x18u];
    ctx->r5.u32 = 0;
    uint32_t arghid;
    if (gender == 0u) {  // female
      int32_t i = (int32_t)LD32(base, 0x83062BF4u) + 1;
      if (i > 2)
        i = 0;
      ST32(base, 0x83062BF4u, (uint32_t)i);
      arghid = (uint32_t)i + 0x0Du;
    } else {  // male
      int32_t i = (int32_t)LD32(base, 0x83062BF8u) + 1;
      if (i > 0x18)
        i = 0;
      ST32(base, 0x83062BF8u, (uint32_t)i);
      arghid = (uint32_t)i + 0x86u;
    }
    ctx->r4.u32 = arghid;
    ge_ce_play_at_location(ctx, base);  // argh at their location
  } else {                              // your own gasp
    ST32(base, 0x824203ACu, 0u);        // your death -> death tune plays
    sub_82144920(*ctx, base);           // play gasp (caller's args)
  }
}

// only_trigger_mp_death_tune_for_your_kills_and_yourself @0x820BFB04 (the
// `bl <play death tune>`; r3 already = 6 from the vanilla `li r3,6` at 0x820BFB00).
// Skip the death tune when 0x824203AC is set (the gasp hook flagged this death as
// another player's). jump_on_true skips the bl; no jump_on_false -> falls through
// and plays it for your own deaths.
bool ge_ce_death_tune() {
  PPCContext* ctx;
  uint8_t* base;
  getcb(ctx, base);
  (void)ctx;
  return LD32(base, 0x824203ACu) != 0u;
}

// reset_internal_cheat_state float relocation @0x8209D88C: the death-tune logic
// reads a 5.0f that originally lived at the address CE now repurposes as the
// bypass flag (0x824203AC). After the original `lfs f1,0x3AC(r11)`, reload f1
// from +0x3A8 instead (where ge_ce_patches stashes the 5.0f). r11 = 0x82420000.
void ge_ce_killtune_float(PPCRegister& r11, PPCRegister& f1) {
  PPCContext* ctx;
  uint8_t* base;
  getcb(ctx, base);
  (void)ctx;
  f1.f64 = (double)LDF32(base, r11.u32 + 0x3A8u);
}

// fix_watch_volume_sliders_range: the watch volume sliders only spanned half the
// real 0-100 range. CE reads the stored byte and halves it for display, and
// doubles the slider value before storing (entering the save routine past its
// clamp). READ hooks replace `bl <vol read>` (r3 = settings ptr -> vol byte >> 1);
// SAVE hooks replace `bl <vol save>` (double r4, then run the save routine from
// its mid-point continuation so the doubled value isn't clamped back). Music vol
// byte = settings+0x295, fx vol = settings+0x294. All use jump_address = site+4.
void ge_ce_watch_music_read(PPCRegister& r3) {
  PPCContext* ctx;
  uint8_t* base;
  getcb(ctx, base);
  (void)ctx;
  r3.u32 = (uint32_t)(base[r3.u32 + 0x295u] >> 1);
}
void ge_ce_watch_sfx_read(PPCRegister& r3) {
  PPCContext* ctx;
  uint8_t* base;
  getcb(ctx, base);
  (void)ctx;
  r3.u32 = (uint32_t)(base[r3.u32 + 0x294u] >> 1);
}
void ge_ce_watch_music_save() {
  PPCContext* ctx;
  uint8_t* base;
  getcb(ctx, base);
  ctx->r4.u32 = ctx->r4.u32 + ctx->r4.u32;  // double the slider value
  ge_cont_82184E18(*ctx, base);             // save routine past its clamp
}
void ge_ce_watch_sfx_save() {
  PPCContext* ctx;
  uint8_t* base;
  getcb(ctx, base);
  ctx->r4.u32 = ctx->r4.u32 + ctx->r4.u32;
  ge_cont_82184E48(*ctx, base);
}

// Optional command-provenance diagnostics. These hooks observe the D3D9
// submission descriptors before the original bodies clobber their argument
// registers. They never execute packets themselves: the title-owned primary
// ring is the sole authoritative submission path.
extern "C" void __imp__sub_8219CAF8(PPCContext& ctx, uint8_t* base);
extern "C" void sub_8219CAF8(PPCContext& ctx, uint8_t* base) {
  const uint32_t desc_ea = ctx.r4.u32;
  const uint32_t desc_count = ctx.r5.u32;
  static const bool submission_diagnostics =
      EnvironmentFlagEnabled("GOLDENEYE_METAL_SUBMISSION_DIAGNOSTICS");
  if (submission_diagnostics) {
    auto* gs = ge_gs();
    if (gs && gs->name() == "Metal") {
      static std::atomic<uint32_t> calls{0};
      const uint32_t call_index = calls.fetch_add(1, std::memory_order_relaxed) + 1;
      if (call_index <= 64) {
        std::fprintf(stderr, "[ge-kickoff]#%u count=%u desc_ea=0x%08x\n", call_index, desc_count,
                     desc_ea);
        if (desc_ea && desc_count && desc_count <= 4096u) {
          for (uint32_t descriptor_index = 0; descriptor_index < std::min(desc_count, 16u);
               ++descriptor_index) {
            const uint32_t descriptor_address = desc_ea + descriptor_index * 8u;
            const uint32_t length = REX_LOAD_U32(descriptor_address) & 0xFFFFFFu;
            const uint32_t raw_address = REX_LOAD_U32(descriptor_address + 4u);
            std::fprintf(stderr, "  [ge-kickoff-desc]#%u.%u raw=0x%08x len=%u phys=0x%08x\n",
                         call_index, descriptor_index, raw_address, length,
                         raw_address & 0x1FFFFFFFu);
          }
        }
        std::fflush(stderr);
      }
    }
  }
  __imp__sub_8219CAF8(ctx, base);
}

extern "C" void __imp__sub_8219CF88(PPCContext& ctx, uint8_t* base);
extern "C" void sub_8219CF88(PPCContext& ctx, uint8_t* base) {
  const uint32_t ib_addr = ctx.r5.u32;
  const uint32_t ib_len = ctx.r6.u32;
  static const bool submission_diagnostics =
      EnvironmentFlagEnabled("GOLDENEYE_METAL_SUBMISSION_DIAGNOSTICS");
  if (submission_diagnostics) {
    auto* gs = ge_gs();
    if (gs && gs->name() == "Metal") {
      static std::atomic<uint32_t> logs{0};
      const uint32_t log_index = logs.fetch_add(1, std::memory_order_relaxed) + 1;
      if (log_index <= 96 || (log_index & 0x3FFu) == 0) {
        std::fprintf(stderr, "[ge-flush]#%u ib_addr=0x%08x ib_len=%u\n", log_index, ib_addr,
                     ib_len);
        std::fflush(stderr);
      }
    }
  }
  __imp__sub_8219CF88(ctx, base);
}
