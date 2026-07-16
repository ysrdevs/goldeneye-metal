#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd -P)"

VERSION="${VERSION:-${APP_VERSION:-0.1.0}}"
BUILD_NUMBER="${BUILD_NUMBER:-1}"
BUNDLE_IDENTIFIER="${BUNDLE_IDENTIFIER:-io.github.ysrdevs.goldeneye-metal}"
MACOS_DEPLOYMENT_TARGET="${MACOS_DEPLOYMENT_TARGET:-14.0}"

SPIRV_CROSS_TAG="vulkan-sdk-1.4.350.1"
SPIRV_CROSS_ARCHIVE_NAME="SPIRV-Cross-${SPIRV_CROSS_TAG}.tar.gz"
SPIRV_CROSS_URL="https://github.com/KhronosGroup/SPIRV-Cross/archive/refs/tags/${SPIRV_CROSS_TAG}.tar.gz"
SPIRV_CROSS_SHA256="21057934ede32fe90a63dc304fdce0f2a6cb4f0ca685a72ed36a73aac6f72ad5"
SPIRV_CROSS_RECIPE_VERSION="1"

APP="$REPO_ROOT/vendor/GoldenEye-Recomp/out/build/macos-arm64-release/dist/GoldenEye Metal.app"
DOWNLOAD_TEMP=""
AUDIT_TEMP=""

usage() {
  cat <<'EOF'
Build and verify the unsigned GoldenEye Metal macOS app.

Usage:
  ./launcher/build-app.sh

Optional environment variables:
  VERSION                  App version (default: 0.1.0)
  BUILD_NUMBER             Numeric bundle build number (default: 1)
  BUNDLE_IDENTIFIER        Reverse-DNS bundle ID
  MACOS_DEPLOYMENT_TARGET  Minimum macOS version (default: 14.0)
  BUILD_JOBS               Parallel build jobs (default: logical CPU count)
  SPIRV_CROSS_PREFIX       Absolute path to a compatible SPIRV-Cross install

By default the script downloads the official SPIRV-Cross source archive,
verifies its pinned SHA-256, and builds it locally for the declared minimum
macOS version. It downloads no XEX or game assets and copies no XEX or
extracted game-data files into the application bundle.
EOF
}

fail() {
  printf 'build-app.sh: %s\n' "$1" >&2
  exit 1
}

step() {
  printf '\n==> %s\n' "$1"
}

cleanup() {
  if [ -n "$DOWNLOAD_TEMP" ] && [ -e "$DOWNLOAD_TEMP" ]; then
    /bin/rm -f "$DOWNLOAD_TEMP"
  fi
  if [ -n "$AUDIT_TEMP" ] && [ -e "$AUDIT_TEMP" ]; then
    /bin/rm -f "$AUDIT_TEMP"
  fi
}
trap cleanup EXIT

require_command() {
  command -v "$1" >/dev/null 2>&1 || fail "required command not found: $1"
}

archive_checksum_is_valid() {
  [ -f "$1" ] || return 1
  printf '%s  %s\n' "$SPIRV_CROSS_SHA256" "$1" |
    /usr/bin/shasum -a 256 -c - >/dev/null 2>&1
}

version_is_at_most() {
  /usr/bin/awk -v actual="$1" -v maximum="$2" 'BEGIN {
    actual_count = split(actual, a, ".")
    maximum_count = split(maximum, b, ".")
    count = actual_count > maximum_count ? actual_count : maximum_count
    for (i = 1; i <= count; i++) {
      av = a[i] + 0
      bv = b[i] + 0
      if (av < bv) exit 0
      if (av > bv) exit 1
    }
    exit 0
  }'
}

audit_spirv_archive() {
  archive="$1"
  found_macho=0
  AUDIT_TEMP="$(/usr/bin/mktemp "${TMPDIR:-/tmp}/goldeneye-spirv-object.XXXXXX")"

  while IFS= read -r member; do
    [ -n "$member" ] || continue
    if ! /usr/bin/ar -p "$archive" "$member" >"$AUDIT_TEMP" 2>/dev/null; then
      continue
    fi

    object_minimum="$({
      /usr/bin/vtool -show-build "$AUDIT_TEMP" 2>/dev/null || true
    } | /usr/bin/awk '$1 == "minos" { print $2; exit }')"
    [ -n "$object_minimum" ] || continue
    found_macho=1

    if ! version_is_at_most "$object_minimum" "$MACOS_DEPLOYMENT_TARGET"; then
      fail "$(basename "$archive") contains an object built for macOS $object_minimum, newer than the app target $MACOS_DEPLOYMENT_TARGET"
    fi
  done < <(/usr/bin/ar -t "$archive")

  /bin/rm -f "$AUDIT_TEMP"
  AUDIT_TEMP=""
  [ "$found_macho" -eq 1 ] || fail "could not verify the macOS target in $archive"
}

build_pinned_spirv_cross() {
  dependency_root="$REPO_ROOT/out/deps/spirv-cross/${SPIRV_CROSS_TAG}-macos-${MACOS_DEPLOYMENT_TARGET}-arm64"
  download_dir="$REPO_ROOT/out/deps/downloads"
  archive="$download_dir/$SPIRV_CROSS_ARCHIVE_NAME"
  source_dir="$dependency_root/src"
  build_dir="$dependency_root/build"
  install_dir="$dependency_root/install"

  /bin/mkdir -p "$download_dir"

  if ! archive_checksum_is_valid "$archive"; then
    if [ -e "$archive" ]; then
      printf 'Discarding a SPIRV-Cross archive with the wrong checksum.\n'
      /bin/rm -f "$archive"
    fi

    step "Downloading pinned SPIRV-Cross source"
    DOWNLOAD_TEMP="$(/usr/bin/mktemp "$download_dir/.spirv-cross.XXXXXX")"
    /usr/bin/curl --fail --location --retry 3 --output "$DOWNLOAD_TEMP" \
      "$SPIRV_CROSS_URL"
    archive_checksum_is_valid "$DOWNLOAD_TEMP" ||
      fail "downloaded SPIRV-Cross archive did not match the pinned SHA-256"
    /bin/mv "$DOWNLOAD_TEMP" "$archive"
    DOWNLOAD_TEMP=""
  fi

  if [ ! -f "$source_dir/.goldeneye-source-$SPIRV_CROSS_SHA256" ]; then
    step "Extracting SPIRV-Cross"
    /bin/rm -rf "$source_dir"
    /bin/mkdir -p "$source_dir"
    /usr/bin/tar -xzf "$archive" --strip-components=1 -C "$source_dir"
    /usr/bin/touch "$source_dir/.goldeneye-source-$SPIRV_CROSS_SHA256"
  fi

  if [ ! -f "$install_dir/include/spirv_cross/spirv_msl.hpp" ] ||
     [ ! -f "$install_dir/lib/libspirv-cross-msl.a" ] ||
     [ ! -f "$install_dir/lib/libspirv-cross-glsl.a" ] ||
     [ ! -f "$install_dir/lib/libspirv-cross-core.a" ] ||
     [ ! -f "$install_dir/share/doc/spirv-cross/LICENSE" ] ||
     [ ! -f "$install_dir/.goldeneye-recipe-$SPIRV_CROSS_RECIPE_VERSION" ]; then
    step "Building SPIRV-Cross for macOS $MACOS_DEPLOYMENT_TARGET"
    /bin/rm -rf "$build_dir" "$install_dir"

    cmake -S "$source_dir" -B "$build_dir" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="$install_dir" \
      -DCMAKE_INSTALL_LIBDIR=lib \
      -DCMAKE_OSX_ARCHITECTURES=arm64 \
      -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOS_DEPLOYMENT_TARGET" \
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
      -DCMAKE_DISABLE_FIND_PACKAGE_Git=TRUE \
      -DSPIRV_CROSS_STATIC=ON \
      -DSPIRV_CROSS_SHARED=OFF \
      -DSPIRV_CROSS_CLI=OFF \
      -DSPIRV_CROSS_ENABLE_TESTS=OFF \
      -DSPIRV_CROSS_ENABLE_GLSL=ON \
      -DSPIRV_CROSS_ENABLE_MSL=ON \
      -DSPIRV_CROSS_ENABLE_HLSL=OFF \
      -DSPIRV_CROSS_ENABLE_CPP=OFF \
      -DSPIRV_CROSS_ENABLE_REFLECT=OFF \
      -DSPIRV_CROSS_ENABLE_C_API=OFF \
      -DSPIRV_CROSS_ENABLE_UTIL=OFF \
      -DSPIRV_CROSS_FORCE_PIC=ON

    cmake --build "$build_dir" --parallel "$BUILD_JOBS"
    cmake --install "$build_dir"
    cmake -E make_directory "$install_dir/share/doc/spirv-cross"
    cmake -E copy_if_different \
      "$source_dir/LICENSE" "$install_dir/share/doc/spirv-cross/LICENSE"
    /usr/bin/touch \
      "$install_dir/.goldeneye-recipe-$SPIRV_CROSS_RECIPE_VERSION"
  else
    printf 'Using cached SPIRV-Cross built for macOS %s.\n' \
      "$MACOS_DEPLOYMENT_TARGET"
  fi

  SPIRV_CROSS_PREFIX_RESOLVED="$install_dir"
}

if [ "${1:-}" = "--help" ] || [ "${1:-}" = "-h" ]; then
  usage
  exit 0
fi
[ "$#" -eq 0 ] || fail "unknown argument: $1 (use --help)"

[ "$(/usr/bin/uname -s)" = "Darwin" ] || fail "this script requires macOS"
[ "$(/usr/bin/uname -m)" = "arm64" ] || fail "this release build requires Apple Silicon"

require_command cmake
require_command git
require_command /usr/bin/ar
require_command /usr/bin/curl
require_command /usr/bin/shasum
require_command /usr/bin/tar
require_command /usr/bin/vtool

[[ "$VERSION" =~ ^[0-9]+\.[0-9]+(\.[0-9]+)?$ ]] ||
  fail "VERSION must contain two or three numeric components"
[[ "$BUILD_NUMBER" =~ ^[0-9]+(\.[0-9]+)?(\.[0-9]+)?$ ]] ||
  fail "BUILD_NUMBER must contain one to three numeric components"
[[ "$MACOS_DEPLOYMENT_TARGET" =~ ^[0-9]+\.[0-9]+(\.[0-9]+)?$ ]] ||
  fail "MACOS_DEPLOYMENT_TARGET must be a numeric macOS version"

if [ ! -f "$REPO_ROOT/vendor/GoldenEye-Recomp/generated/rexglue.cmake" ] ||
   [ ! -f "$REPO_ROOT/vendor/GoldenEye-Recomp/generated/sources.cmake" ]; then
  fail "generated game integration is missing; run rexglue codegen with your authorized compatible XEX first"
fi

if [ -z "${BUILD_JOBS:-}" ]; then
  BUILD_JOBS="$(/usr/sbin/sysctl -n hw.logicalcpu 2>/dev/null || printf '8')"
fi
printf '%s\n' "$BUILD_JOBS" | /usr/bin/grep -Eq '^[1-9][0-9]*$' ||
  fail "BUILD_JOBS must be a positive integer"

cd "$REPO_ROOT"

if [ -n "${SPIRV_CROSS_PREFIX:-}" ]; then
  case "$SPIRV_CROSS_PREFIX" in
    /*) SPIRV_CROSS_PREFIX_RESOLVED="$SPIRV_CROSS_PREFIX" ;;
    *) fail "SPIRV_CROSS_PREFIX must be an absolute path" ;;
  esac
  printf 'Using SPIRV-Cross override: %s\n' "$SPIRV_CROSS_PREFIX_RESOLVED"
else
  build_pinned_spirv_cross
fi

SPIRV_INCLUDE="$SPIRV_CROSS_PREFIX_RESOLVED/include"
SPIRV_MSL="$SPIRV_CROSS_PREFIX_RESOLVED/lib/libspirv-cross-msl.a"
SPIRV_GLSL="$SPIRV_CROSS_PREFIX_RESOLVED/lib/libspirv-cross-glsl.a"
SPIRV_CORE="$SPIRV_CROSS_PREFIX_RESOLVED/lib/libspirv-cross-core.a"
SPIRV_LICENSE=""
for license_candidate in \
  "$SPIRV_CROSS_PREFIX_RESOLVED/share/doc/spirv-cross/LICENSE" \
  "$SPIRV_CROSS_PREFIX_RESOLVED/share/doc/spirv-cross/LICENSE.txt" \
  "$SPIRV_CROSS_PREFIX_RESOLVED/share/doc/spirv-cross/LICENSE.md" \
  "$SPIRV_CROSS_PREFIX_RESOLVED/opt/spirv-cross/LICENSE" \
  "$SPIRV_CROSS_PREFIX_RESOLVED/opt/spirv-cross/LICENSE.txt" \
  "$SPIRV_CROSS_PREFIX_RESOLVED/opt/spirv-cross/LICENSE.md" \
  "$SPIRV_CROSS_PREFIX_RESOLVED/LICENSE" \
  "$SPIRV_CROSS_PREFIX_RESOLVED/LICENSE.txt" \
  "$SPIRV_CROSS_PREFIX_RESOLVED/LICENSE.md"; do
  if [ -f "$license_candidate" ]; then
    SPIRV_LICENSE="$license_candidate"
    break
  fi
done

[ -f "$SPIRV_INCLUDE/spirv_cross/spirv_msl.hpp" ] ||
  fail "SPIRV-Cross MSL headers are missing from $SPIRV_INCLUDE"
[ -f "$SPIRV_MSL" ] || fail "SPIRV-Cross MSL library is missing: $SPIRV_MSL"
[ -f "$SPIRV_GLSL" ] || fail "SPIRV-Cross GLSL library is missing: $SPIRV_GLSL"
[ -f "$SPIRV_CORE" ] || fail "SPIRV-Cross core library is missing: $SPIRV_CORE"
[ -n "$SPIRV_LICENSE" ] ||
  fail "SPIRV-Cross license is missing beside $SPIRV_CROSS_PREFIX_RESOLVED"

step "Auditing SPIRV-Cross deployment targets"
audit_spirv_archive "$SPIRV_MSL"
audit_spirv_archive "$SPIRV_GLSL"
audit_spirv_archive "$SPIRV_CORE"

SPIRV_CMAKE_ARGS=(
  "-DSPIRV_CROSS_INCLUDE_DIR=$SPIRV_INCLUDE"
  "-DSPIRV_CROSS_MSL_LIBRARY=$SPIRV_MSL"
  "-DSPIRV_CROSS_GLSL_LIBRARY=$SPIRV_GLSL"
  "-DSPIRV_CROSS_CORE_LIBRARY=$SPIRV_CORE"
)

step "Configuring the GoldenEye app and native SDK"
cmake -S vendor/GoldenEye-Recomp --preset macos-arm64-release \
  "-DCMAKE_OSX_DEPLOYMENT_TARGET=$MACOS_DEPLOYMENT_TARGET" \
  "-DGOLDENEYE_VERSION=$VERSION" \
  "-DGOLDENEYE_BUILD_NUMBER=$BUILD_NUMBER" \
  "-DGOLDENEYE_BUNDLE_IDENTIFIER=$BUNDLE_IDENTIFIER" \
  "-DGOLDENEYE_SPIRV_CROSS_LICENSE=$SPIRV_LICENSE" \
  "${SPIRV_CMAKE_ARGS[@]}"

step "Building and verifying the unsigned app"
cmake --build vendor/GoldenEye-Recomp/out/build/macos-arm64-release \
  --target goldeneye_macos_app_verify --parallel "$BUILD_JOBS"

[ -d "$APP" ] || fail "the verified app was not created at $APP"

printf '\nUnsigned app is ready:\n  %s\n' "$APP"
printf '\nTo sign, notarize, and package it, run:\n'
printf '  SIGN_IDENTITY="Developer ID Application: YUVRAJ SINGH (9RCV543M32)" \\\n'
printf '  NOTARY_PROFILE="cyberconsole-notary" \\\n'
printf '  ./tools/sign-notarize.sh\n'
