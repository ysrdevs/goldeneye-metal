#!/usr/bin/env bash
# Deterministic first-Dam Metal benchmark. Results are never written into game data.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
PARSER="$ROOT/tools/metal_profile_parser.py"
APP_CONTENTS="$ROOT/vendor/GoldenEye-Recomp/out/build/macos-arm64-release/dist/GoldenEye Metal.app/Contents"
EXECUTABLE="${GOLDENEYE_BENCH_EXECUTABLE:-$APP_CONTENTS/MacOS/GoldenEye}"
RUNTIME_DIR="${GOLDENEYE_BENCH_RUNTIME_DIR:-$APP_CONTENTS/Frameworks}"
GAME_DATA="${GOLDENEYE_BENCH_GAME_DATA:-$HOME/Library/Application Support/GoldenEye Metal/Game Data}"
TIMEOUT_S="${GOLDENEYE_BENCH_TIMEOUT_S:-600}"
POLL_S="${GOLDENEYE_BENCH_POLL_S:-2}"
WARMUP="${GOLDENEYE_BENCH_WARMUP_WINDOWS:-8}"
MEASURE="${GOLDENEYE_BENCH_MEASURE_WINDOWS:-48}"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
BENCHMARK_ROOT="${GOLDENEYE_BENCH_OUTPUT_ROOT:-$ROOT/out/benchmarks}"
OUT=""
LOG=""
USER_DATA=""
CACHE="${GOLDENEYE_BENCH_CACHE_DIR:-$ROOT/out/benchmarks/cache}"
PID=""
PGID=""
STOP_REQUESTED=0
FORCED_KILL=0
TERMINATION_REQUEST_SENT=0
DESCENDANT_CLEANUP_REQUIRED=0
PROCESS_STATUS=""
HARNESS_FAILURE=""

fail() { printf 'benchmark-dam: %s\n' "$*" >&2; exit 2; }
leader_alive() { [[ -n "$PID" ]] && kill -0 "$PID" 2>/dev/null; }
process_group_alive() { [[ -n "$PGID" ]] && kill -0 -- "-$PGID" 2>/dev/null; }
terminate() {
  if process_group_alive; then
    if ! leader_alive; then
      DESCENDANT_CLEANUP_REQUIRED=1
    fi
    # Prefer the same normal AppKit termination path used by Command-Q. A raw
    # SIGTERM is only a fallback for non-AppKit fixtures or a stuck app.
    if leader_alive && [[ "$(uname -s)" == "Darwin" && "${GOLDENEYE_BENCH_NATIVE_QUIT:-1}" == "1" ]] &&
      python3 "$ROOT/tools/render_regression.py" terminate --pid "$PID" >/dev/null 2>&1; then
      TERMINATION_REQUEST_SENT=1
      printf 'Native AppKit quit accepted.\n'
      for _ in {1..20}; do process_group_alive || return 0; sleep 1; done
      if ! leader_alive; then
        DESCENDANT_CLEANUP_REQUIRED=1
      fi
    fi
    if process_group_alive; then
      if kill -TERM -- "-$PGID" 2>/dev/null; then
        TERMINATION_REQUEST_SENT=1
      else
        return 0
      fi
      for _ in {1..10}; do process_group_alive || return 0; sleep 1; done
    fi
    if process_group_alive; then
      kill -KILL -- "-$PGID" 2>/dev/null || true
      FORCED_KILL=1
    fi
  fi
}
trap terminate EXIT INT TERM

case "${GOLDENEYE_BENCH_RESET_CACHE:-0}" in 0 | 1) ;; *) fail "GOLDENEYE_BENCH_RESET_CACHE must be 0 or 1" ;; esac
case "${GOLDENEYE_BENCH_ALLOW_STALE_BUILD:-0}" in 0 | 1) ;; *) fail "GOLDENEYE_BENCH_ALLOW_STALE_BUILD must be 0 or 1" ;; esac
case "${GOLDENEYE_BENCH_NATIVE_QUIT:-1}" in 0 | 1) ;; *) fail "GOLDENEYE_BENCH_NATIVE_QUIT must be 0 or 1" ;; esac

PARSER_ARGS=(--warmup "$WARMUP" --measure "$MEASURE")
if [[ -n "${GOLDENEYE_BENCH_MIN_FPS:-}" ]]; then
  PARSER_ARGS+=(--min-mean-fps "$GOLDENEYE_BENCH_MIN_FPS")
fi
if [[ -n "${GOLDENEYE_BENCH_MAX_P99_FRAME_MS:-}" ]]; then
  PARSER_ARGS+=(--max-window-p99-ms "$GOLDENEYE_BENCH_MAX_P99_FRAME_MS")
fi
if [[ -n "${GOLDENEYE_BENCH_MAX_FPS_CV_PERCENT:-}" ]]; then
  PARSER_ARGS+=(--max-window-fps-cv-percent "$GOLDENEYE_BENCH_MAX_FPS_CV_PERCENT")
fi
python3 "$PARSER" --validate-options "${PARSER_ARGS[@]}" \
  --harness-timeout-seconds "$TIMEOUT_S" --harness-poll-seconds "$POLL_S" ||
  fail "invalid numeric benchmark option"

[[ -x "$EXECUTABLE" ]] || fail "executable is not runnable: $EXECUTABLE (override GOLDENEYE_BENCH_EXECUTABLE)"
[[ -d "$RUNTIME_DIR" ]] || fail "runtime dylib directory does not exist: $RUNTIME_DIR (override GOLDENEYE_BENCH_RUNTIME_DIR)"
[[ -f "$GAME_DATA/default.xex" ]] || fail "game data must contain default.xex: $GAME_DATA (override GOLDENEYE_BENCH_GAME_DATA)"
mkdir -p "$BENCHMARK_ROOT"
OUT="$(mktemp -d "$BENCHMARK_ROOT/$STAMP.XXXXXX")"
LOG="$OUT/raw.log"
USER_DATA="$OUT/user-data"
mkdir -p "$USER_DATA" "$OUT/home" "$OUT/tmp"

if [[ "${GOLDENEYE_BENCH_RESET_CACHE:-0}" == "1" ]]; then
  [[ -z "${GOLDENEYE_BENCH_CACHE_DIR:-}" ]] ||
    fail "GOLDENEYE_BENCH_RESET_CACHE=1 may only reset the benchmark's default cache"
  rm -rf "$CACHE"
fi
if [[ -d "$CACHE" ]] && find "$CACHE" -mindepth 1 -print -quit | grep -q .; then
  export GOLDENEYE_BENCH_CACHE_MODE=warm
else
  export GOLDENEYE_BENCH_CACHE_MODE=cold
fi
mkdir -p "$CACHE"

export REX_GPU=metal REX_INPUT_BACKEND=none REX_MNK_MODE=false
export GOLDENEYE_AUTO_START=menu GOLDENEYE_AUTO_MISSION=dam GOLDENEYE_METAL_PROFILE=1 REX_METAL_SHOW_FPS=false
# Never share logs, configuration, saves, or pipeline caches with a player's
# live game. The supplied game-data directory remains the only shared input.
export HOME="$OUT/home" TMPDIR="$OUT/tmp"
export REX_USER_DATA_ROOT="$USER_DATA" REX_CACHE_PATH="$CACHE"
# These names are the runtime's CVar-to-environment mapping (see src/core/cvar.cpp).
export REX_WINDOW_WIDTH=1280 REX_WINDOW_HEIGHT=720 REX_VIDEO_MODE_WIDTH=1280 REX_VIDEO_MODE_HEIGHT=720
export REX_GPU_VSYNC=false REX_MAX_FPS=60 REX_FULLSCREEN=false
export DYLD_LIBRARY_PATH="$RUNTIME_DIR${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"

DYLIB="$RUNTIME_DIR/librexruntime.dylib"
[[ -f "$DYLIB" ]] || fail "runtime dylib does not exist: $DYLIB"

if [[ "${GOLDENEYE_BENCH_ALLOW_STALE_BUILD:-0}" != "1" ]]; then
  python3 "$PARSER" --check-freshness --repo "$ROOT" --dylib "$DYLIB" \
    --executable "$EXECUTABLE" || fail "tracked sources are newer than the benchmark artifacts"
fi

python3 "$PARSER" --metadata "$OUT/metadata.json" --executable "$EXECUTABLE" --dylib "$DYLIB" \
  --xex "$GAME_DATA/default.xex" --repo "$ROOT" --data-root "$GAME_DATA"

printf 'DAM benchmark: %s\noutput: %s\ncache: %s (%s)\n' \
  "$EXECUTABLE" "$OUT" "$CACHE" "$GOLDENEYE_BENCH_CACHE_MODE"
python3 "$ROOT/tools/process_group_exec.py" \
  "$EXECUTABLE" --game_data_root "$GAME_DATA" --gpu metal >"$LOG" 2>&1 &
PID=$!
PGID=$PID
START="$(date +%s)"
READY=0
while kill -0 "$PID" 2>/dev/null; do
  if python3 "$PARSER" "$LOG" "${PARSER_ARGS[@]}" --ready; then
    READY=1
    STOP_REQUESTED=1
    printf 'Required Dam windows collected; stopping the benchmark process.\n'
    terminate
    break
  fi
  if (( $(date +%s) - START >= TIMEOUT_S )); then
    printf 'Benchmark timeout after %ss.\n' "$TIMEOUT_S" >&2
    HARNESS_FAILURE="benchmark timed out after ${TIMEOUT_S}s before satisfying all requirements"
    STOP_REQUESTED=1
    terminate
    break
  fi
  sleep "$POLL_S"
done
if process_group_alive && ! leader_alive; then
  terminate
fi
if wait "$PID" 2>/dev/null; then
  PROCESS_STATUS=0
else
  PROCESS_STATUS=$?
fi
PID=""

if [[ -z "$HARNESS_FAILURE" ]]; then
  if (( STOP_REQUESTED == 0 )); then
    HARNESS_FAILURE="benchmark process exited before the harness requested termination (status $PROCESS_STATUS)"
  elif (( FORCED_KILL != 0 )); then
    HARNESS_FAILURE="benchmark process required SIGKILL during harness termination"
  elif (( DESCENDANT_CLEANUP_REQUIRED != 0 )); then
    HARNESS_FAILURE="benchmark process left a child running after its main application exited"
  elif (( READY != 0 && TERMINATION_REQUEST_SENT == 0 )); then
    HARNESS_FAILURE="benchmark process exited before the harness could request termination (status $PROCESS_STATUS)"
  elif (( READY != 0 )) && [[ "$PROCESS_STATUS" != "0" && "$PROCESS_STATUS" != "143" ]]; then
    HARNESS_FAILURE="benchmark process returned unexpected status $PROCESS_STATUS after requested termination"
  fi
fi
PGID=""

FINAL_PARSER_ARGS=("${PARSER_ARGS[@]}")
if [[ -n "$HARNESS_FAILURE" ]]; then
  FINAL_PARSER_ARGS+=(--external-failure "$HARNESS_FAILURE")
fi
if python3 "$PARSER" "$LOG" --output "$OUT" "${FINAL_PARSER_ARGS[@]}"; then
  printf 'PASS: %s\n' "$OUT/summary.txt"
  trap - EXIT INT TERM
  exit 0
fi
printf 'FAIL: %s\n' "$OUT/summary.txt" >&2
trap - EXIT INT TERM
exit 1
