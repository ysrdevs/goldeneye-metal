#!/bin/bash

# Double-click this file in Finder to launch GoldenEye Metal. Game data is
# discovered locally and is never copied into the source tree.

set -u

ROOT="$(cd "$(dirname "$0")" && pwd -P)"
GAME_BINARY="$ROOT/vendor/GoldenEye-Recomp/out/build/macos-arm64-release/GoldenEye"
RUNTIME_DIR="$ROOT/out/macos-arm64"
RUNTIME_LIBRARY="$RUNTIME_DIR/librexruntime.dylib"
ASSET_XEX="$ROOT/vendor/GoldenEye-Recomp/assets/default.xex"
PREFERENCE_DIR="$HOME/Library/Application Support/GoldenEye Metal"
PREFERENCE_FILE="$PREFERENCE_DIR/game-data-root"

show_error() {
  local message="$1"
  printf '\nGoldenEye Metal could not start.\n\n%s\n\n' "$message" >&2

  if [ "${GOLDENEYE_LAUNCHER_NO_DIALOG:-0}" != "1" ] && [ -x /usr/bin/osascript ]; then
    /usr/bin/osascript - "$message" <<'APPLESCRIPT' >/dev/null 2>&1 || true
on run argv
  display alert "GoldenEye Metal" message (item 1 of argv) as critical buttons {"OK"}
end run
APPLESCRIPT
  fi

  if [ -t 0 ] && [ "${GOLDENEYE_LAUNCHER_NO_PAUSE:-0}" != "1" ]; then
    read -r -p "Press Return to close this window... " _unused
  fi
  exit 1
}

is_game_data_root() {
  [ -n "$1" ] && [ -f "$1/default.xex" ] && [ -d "$1/files" ]
}

resolve_path() {
  local path="$1"
  local directory
  local target
  local hops=0

  while [ -L "$path" ]; do
    hops=$((hops + 1))
    [ "$hops" -le 32 ] || return 1
    directory="$(cd "$(dirname "$path")" && pwd -P)" || return 1
    target="$(readlink "$path")" || return 1
    case "$target" in
      /*) path="$target" ;;
      *) path="$directory/$target" ;;
    esac
  done

  directory="$(cd "$(dirname "$path")" && pwd -P)" || return 1
  printf '%s/%s\n' "$directory" "$(basename "$path")"
}

remember_game_data_root() {
  if [ "${GOLDENEYE_LAUNCHER_DRY_RUN:-0}" = "1" ]; then
    return
  fi
  mkdir -p "$PREFERENCE_DIR" || return
  printf '%s\n' "$1" >"$PREFERENCE_FILE"
}

choose_game_data_root() {
  if [ "${GOLDENEYE_LAUNCHER_SKIP_CHOOSER:-0}" = "1" ] || [ ! -x /usr/bin/osascript ]; then
    return 1
  fi

  /usr/bin/osascript <<'APPLESCRIPT'
try
  set selectedFolder to choose folder with prompt "Choose your complete GoldenEye game-data folder. It must contain default.xex and the files folder."
  return POSIX path of selectedFolder
on error number -128
  return ""
end try
APPLESCRIPT
}

[ "$(uname -s)" = "Darwin" ] ||
  show_error "This launcher requires macOS on Apple Silicon."

if [ "$(uname -m)" != "arm64" ]; then
  apple_silicon="$(/usr/sbin/sysctl -n hw.optional.arm64 2>/dev/null || printf '0')"
  [ "$apple_silicon" = "1" ] ||
    show_error "This build requires an Apple Silicon Mac."
fi

if [ ! -x "$GAME_BINARY" ]; then
  show_error $'The game has not been built yet. From the repository root, run:\n\ncmake -S vendor/GoldenEye-Recomp --preset macos-arm64-release\ncmake --build vendor/GoldenEye-Recomp/out/build/macos-arm64-release --target ge --parallel\n\nIf generated game source is missing, complete the code-generation steps in README.md first.'
fi

[ -f "$RUNTIME_LIBRARY" ] ||
  show_error "The native runtime library is missing. Build the macos-arm64-release targets described in README.md, then try again."

GAME_DATA_ROOT=""
GAME_DATA_SOURCE=""

if is_game_data_root "${GOLDENEYE_GAME_DATA_ROOT:-}"; then
  GAME_DATA_ROOT="$GOLDENEYE_GAME_DATA_ROOT"
  GAME_DATA_SOURCE="GOLDENEYE_GAME_DATA_ROOT"
elif is_game_data_root "${REX_GAME_DATA_ROOT:-}"; then
  GAME_DATA_ROOT="$REX_GAME_DATA_ROOT"
  GAME_DATA_SOURCE="REX_GAME_DATA_ROOT"
elif [ -f "$PREFERENCE_FILE" ]; then
  IFS= read -r saved_game_data <"$PREFERENCE_FILE" || saved_game_data=""
  if is_game_data_root "$saved_game_data"; then
    GAME_DATA_ROOT="$saved_game_data"
    GAME_DATA_SOURCE="saved selection"
  fi
fi

if [ -z "$GAME_DATA_ROOT" ] && { [ -e "$ASSET_XEX" ] || [ -L "$ASSET_XEX" ]; }; then
  resolved_xex="$(resolve_path "$ASSET_XEX" 2>/dev/null || true)"
  if [ -n "$resolved_xex" ]; then
    resolved_root="$(dirname "$resolved_xex")"
    if is_game_data_root "$resolved_root"; then
      GAME_DATA_ROOT="$resolved_root"
      GAME_DATA_SOURCE="assets/default.xex"
    fi
  fi
fi

if [ -z "$GAME_DATA_ROOT" ]; then
  selected_game_data="$(choose_game_data_root)" || selected_game_data=""
  selected_game_data="${selected_game_data%/}"
  if [ -z "$selected_game_data" ]; then
    show_error "No game-data folder was selected. Reopen the launcher and choose the complete folder containing default.xex and files/."
  fi
  if ! is_game_data_root "$selected_game_data"; then
    show_error "The selected folder is incomplete. Choose a folder that directly contains both default.xex and the files directory."
  fi
  GAME_DATA_ROOT="$(cd "$selected_game_data" && pwd -P)"
  GAME_DATA_SOURCE="Finder selection"
  remember_game_data_root "$GAME_DATA_ROOT"
fi

# Make every source absolute before changing to the repository directory. The
# runtime also inherits this canonical path so an in-game restart keeps using
# the exact folder that the launcher validated.
GAME_DATA_ROOT="$(cd "$GAME_DATA_ROOT" && pwd -P)" ||
  show_error "The game-data folder is no longer available: $GAME_DATA_ROOT"

export REX_GPU="metal"
export REX_MNK_MODE="true"
export REX_GAME_DATA_ROOT="$GAME_DATA_ROOT"
export DYLD_LIBRARY_PATH="$RUNTIME_DIR${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"

# Rendering diagnostics inject controller input and are intentionally excluded
# from an interactive launch, even if the Terminal inherited them.
unset GOLDENEYE_AUTO_START GOLDENEYE_AUTO_MISSION

printf '\nGoldenEye Metal\n'
printf '  Game data: %s (%s)\n' "$GAME_DATA_ROOT" "$GAME_DATA_SOURCE"
printf '  Defaults:  WASD move | Space A | Shift B | Return Start | Esc settings\n'
printf '  Mouse:     move to look | left fire | right aim\n\n'

if [ "${GOLDENEYE_LAUNCHER_DRY_RUN:-0}" = "1" ]; then
  printf 'Dry run; launch command:\n  '
  printf '%q ' "$GAME_BINARY" --game_data_root "$GAME_DATA_ROOT" --gpu metal
  printf '\n'
  exit 0
fi

cd "$ROOT" || show_error "The repository directory is no longer available: $ROOT"
exec "$GAME_BINARY" --game_data_root "$GAME_DATA_ROOT" --gpu metal

show_error "The GoldenEye process could not be started."
