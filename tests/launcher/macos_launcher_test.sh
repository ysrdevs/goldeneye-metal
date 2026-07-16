#!/bin/bash

set -eu

SOURCE_ROOT="$(cd "$(dirname "$0")/../.." && pwd -P)"
SANDBOX="$(mktemp -d "${TMPDIR:-/tmp}/GoldenEye launcher.XXXXXX")"
SANDBOX="$(cd "$SANDBOX" && pwd -P)"
trap 'rm -rf "$SANDBOX"' EXIT

fail() {
  printf 'launcher test failed: %s\n' "$1" >&2
  exit 1
}

mkdir -p \
  "$SANDBOX/vendor/GoldenEye-Recomp/out/build/macos-arm64-release" \
  "$SANDBOX/vendor/GoldenEye-Recomp/assets" \
  "$SANDBOX/out/macos-arm64" \
  "$SANDBOX/data root/files" \
  "$SANDBOX/home"
cp "$SOURCE_ROOT/Launch GoldenEye.command" "$SANDBOX/Launch GoldenEye.command"
touch "$SANDBOX/out/macos-arm64/librexruntime.dylib"
touch "$SANDBOX/data root/default.xex"

GAME_BINARY="$SANDBOX/vendor/GoldenEye-Recomp/out/build/macos-arm64-release/GoldenEye"
RESULT_FILE="$SANDBOX/result.txt"
cat >"$GAME_BINARY" <<'FAKE_GAME'
#!/bin/bash
{
  printf 'gpu=%s\n' "${REX_GPU:-}"
  printf 'input_backend=%s\n' "${REX_INPUT_BACKEND:-}"
  printf 'mnk=%s\n' "${REX_MNK_MODE:-}"
  printf 'game_data_env=%s\n' "${REX_GAME_DATA_ROOT:-}"
  printf 'auto_start=%s\n' "${GOLDENEYE_AUTO_START-unset}"
  printf 'auto_mission=%s\n' "${GOLDENEYE_AUTO_MISSION-unset}"
  for argument in "$@"; do
    printf 'arg=%s\n' "$argument"
  done
} >"$GOLDENEYE_LAUNCHER_TEST_OUTPUT"
FAKE_GAME
chmod +x "$GAME_BINARY"

(
  cd "$SANDBOX"
  HOME="$SANDBOX/home" \
  GOLDENEYE_GAME_DATA_ROOT="./data root" \
  REX_GAME_DATA_ROOT="$SANDBOX/wrong data" \
  REX_INPUT_BACKEND="none" \
  GOLDENEYE_AUTO_START="periodic" \
  GOLDENEYE_AUTO_MISSION="dam" \
  GOLDENEYE_LAUNCHER_TEST_OUTPUT="$RESULT_FILE" \
  GOLDENEYE_LAUNCHER_NO_DIALOG=1 \
  GOLDENEYE_LAUNCHER_NO_PAUSE=1 \
  /bin/bash "./Launch GoldenEye.command" >/dev/null
)

EXPECTED_ROOT="$SANDBOX/data root"
grep -Fqx 'gpu=metal' "$RESULT_FILE" || fail "Metal was not selected"
grep -Fqx 'input_backend=sdl' "$RESULT_FILE" || fail "SDL controller input was not enabled"
grep -Fqx 'mnk=true' "$RESULT_FILE" || fail "MnK was not enabled"
grep -Fqx "game_data_env=$EXPECTED_ROOT" "$RESULT_FILE" ||
  fail "the validated canonical game-data root was not exported"
grep -Fqx 'auto_start=unset' "$RESULT_FILE" || fail "auto-start was inherited"
grep -Fqx 'auto_mission=unset' "$RESULT_FILE" || fail "auto-mission was inherited"
grep -Fqx 'arg=--game_data_root' "$RESULT_FILE" || fail "game-data argument is missing"
grep -Fqx "arg=$EXPECTED_ROOT" "$RESULT_FILE" || fail "game-data argument was not canonical"
grep -Fqx 'arg=--gpu' "$RESULT_FILE" || fail "GPU argument is missing"
grep -Fqx 'arg=metal' "$RESULT_FILE" || fail "GPU argument is not Metal"

rm -f "$RESULT_FILE"
ln -s "$SANDBOX/data root/default.xex" \
  "$SANDBOX/vendor/GoldenEye-Recomp/assets/default.xex"
(
  cd "$SANDBOX"
  HOME="$SANDBOX/home" \
  GOLDENEYE_LAUNCHER_TEST_OUTPUT="$RESULT_FILE" \
  GOLDENEYE_LAUNCHER_NO_DIALOG=1 \
  GOLDENEYE_LAUNCHER_NO_PAUSE=1 \
  /bin/bash "./Launch GoldenEye.command" >/dev/null
)
grep -Fqx "game_data_env=$EXPECTED_ROOT" "$RESULT_FILE" ||
  fail "assets/default.xex discovery did not resolve its game-data root"
