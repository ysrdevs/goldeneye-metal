#!/usr/bin/env bash
# Deterministic first-Dam Metal benchmark. Results are never written into game data.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
PARSER="$ROOT/tools/metal_profile_parser.py"
EXECUTABLE="${GOLDENEYE_BENCH_EXECUTABLE:-$ROOT/vendor/GoldenEye-Recomp/out/build/macos-arm64-release/GoldenEye}"
RUNTIME_DIR="${GOLDENEYE_BENCH_RUNTIME_DIR:-$ROOT/out/macos-arm64}"
GAME_DATA="${GOLDENEYE_BENCH_GAME_DATA:-$HOME/Library/Application Support/GoldenEye Metal/Game Data}"
TIMEOUT_S="${GOLDENEYE_BENCH_TIMEOUT_S:-600}"
POLL_S="${GOLDENEYE_BENCH_POLL_S:-2}"
WARMUP="${GOLDENEYE_BENCH_WARMUP_WINDOWS:-8}"
MEASURE="${GOLDENEYE_BENCH_MEASURE_WINDOWS:-48}"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
OUT="$ROOT/out/benchmarks/$STAMP"
LOG="$OUT/raw.log"
PID=""

fail() { printf 'benchmark-dam: %s\n' "$*" >&2; exit 2; }
terminate() {
  if [[ -n "$PID" ]] && kill -0 "$PID" 2>/dev/null; then
    kill -TERM "$PID" 2>/dev/null || true
    for _ in {1..10}; do kill -0 "$PID" 2>/dev/null || return 0; sleep 1; done
    kill -KILL "$PID" 2>/dev/null || true
  fi
}
trap terminate EXIT INT TERM

[[ -x "$EXECUTABLE" ]] || fail "executable is not runnable: $EXECUTABLE (override GOLDENEYE_BENCH_EXECUTABLE)"
[[ -d "$RUNTIME_DIR" ]] || fail "runtime dylib directory does not exist: $RUNTIME_DIR (override GOLDENEYE_BENCH_RUNTIME_DIR)"
[[ -f "$GAME_DATA/default.xex" ]] || fail "game data must contain default.xex: $GAME_DATA (override GOLDENEYE_BENCH_GAME_DATA)"
mkdir -p "$OUT"

export REX_GPU=metal REX_INPUT_BACKEND=none REX_MNK_MODE=false
export GOLDENEYE_AUTO_START=menu GOLDENEYE_AUTO_MISSION=dam GOLDENEYE_METAL_PROFILE=1 REX_METAL_SHOW_FPS=false
# These names are the runtime's CVar-to-environment mapping (see src/core/cvar.cpp).
export REX_WINDOW_WIDTH=1280 REX_WINDOW_HEIGHT=720 REX_VIDEO_MODE_WIDTH=1280 REX_VIDEO_MODE_HEIGHT=720
export REX_GPU_VSYNC=false REX_MAX_FPS=60 REX_FULLSCREEN=false
export DYLD_LIBRARY_PATH="$RUNTIME_DIR${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"

DYLIB="$RUNTIME_DIR/librexruntime.dylib"
[[ -f "$DYLIB" ]] || fail "runtime dylib does not exist: $DYLIB"
python3 "$PARSER" --metadata "$OUT/metadata.json" --executable "$EXECUTABLE" --dylib "$DYLIB" \
  --xex "$GAME_DATA/default.xex" --repo "$ROOT" --data-root "$GAME_DATA"

printf 'DAM benchmark: %s\noutput: %s\n' "$EXECUTABLE" "$OUT"
"$EXECUTABLE" --game_data_root "$GAME_DATA" --gpu metal >"$LOG" 2>&1 &
PID=$!
START="$(date +%s)"
STATUS=1
while kill -0 "$PID" 2>/dev/null; do
  if python3 "$PARSER" "$LOG" --warmup "$WARMUP" --measure "$MEASURE" --ready; then
    STATUS=0
    printf 'Required Dam windows collected; stopping GoldenEye cleanly.\n'
    terminate
    break
  fi
  if (( $(date +%s) - START >= TIMEOUT_S )); then
    printf 'Benchmark timeout after %ss.\n' "$TIMEOUT_S" >&2
    break
  fi
  sleep "$POLL_S"
done
wait "$PID" 2>/dev/null || true
PID=""

if python3 "$PARSER" "$LOG" --output "$OUT" --warmup "$WARMUP" --measure "$MEASURE"; then
  printf 'PASS: %s\n' "$OUT/summary.txt"
  trap - EXIT INT TERM
  exit 0
fi
printf 'FAIL: %s\n' "$OUT/summary.txt" >&2
trap - EXIT INT TERM
exit 1
