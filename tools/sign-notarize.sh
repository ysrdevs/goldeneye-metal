#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd -P)"

VERSION="${VERSION:-${APP_VERSION:-0.1.1}}"
BUILD_NUMBER="${BUILD_NUMBER:-2}"
BUNDLE_IDENTIFIER="${BUNDLE_IDENTIFIER:-io.github.ysrdevs.goldeneye-metal}"
MACOS_DEPLOYMENT_TARGET="${MACOS_DEPLOYMENT_TARGET:-14.0}"

APP="$REPO_ROOT/vendor/GoldenEye-Recomp/out/build/macos-arm64-release/dist/GoldenEye Metal.app"
RUNTIME="$APP/Contents/Frameworks/librexruntime.dylib"
RELEASE_DIR="$REPO_ROOT/release"
ZIP="$RELEASE_DIR/GoldenEye-Metal-${VERSION}-macos-arm64.zip"
DMG="$RELEASE_DIR/GoldenEye-Metal-${VERSION}-macos-arm64.dmg"
ZIP_TEMP="$RELEASE_DIR/.GoldenEye-Metal-${VERSION}-release.zip"
DMG_TEMP="$RELEASE_DIR/.GoldenEye-Metal-${VERSION}-release.dmg"
APP_UPLOAD_ZIP="$RELEASE_DIR/.GoldenEye-Metal-${VERSION}-app-notarization.zip"
DMG_STAGE="$RELEASE_DIR/.GoldenEye-Metal-${VERSION}-dmg-root"
APP_RESULT="$RELEASE_DIR/GoldenEye-Metal-${VERSION}-app-notary-result.plist"
APP_LOG="$RELEASE_DIR/GoldenEye-Metal-${VERSION}-app-notary-log.json"
DMG_RESULT="$RELEASE_DIR/GoldenEye-Metal-${VERSION}-dmg-notary-result.plist"
DMG_LOG="$RELEASE_DIR/GoldenEye-Metal-${VERSION}-dmg-notary-log.json"

usage() {
  cat <<'EOF'
Build, Developer-ID sign, notarize, staple, and package GoldenEye Metal.

Usage:
  SIGN_IDENTITY="Developer ID Application: YUVRAJ SINGH (9RCV543M32)" \
  NOTARY_PROFILE="cyberconsole-notary" \
  ./tools/sign-notarize.sh

The notarization profile must already exist in Keychain. The script never
accepts or stores an Apple Account password. It rebuilds and verifies the app,
then writes a notarized ZIP and DMG under release/.

Optional build environment variables:
  VERSION, BUILD_NUMBER, BUNDLE_IDENTIFIER, MACOS_DEPLOYMENT_TARGET,
  BUILD_JOBS, SPIRV_CROSS_PREFIX
EOF
}

fail() {
  printf 'sign-notarize.sh: %s\n' "$1" >&2
  exit 1
}

step() {
  printf '\n==> %s\n' "$1"
}

cleanup() {
  /bin/rm -f "$APP_UPLOAD_ZIP"
  /bin/rm -f "$ZIP_TEMP" "$DMG_TEMP"
  /bin/rm -rf "$DMG_STAGE"
}

require_command() {
  command -v "$1" >/dev/null 2>&1 || fail "required command not found: $1"
}

show_notary_result() {
  result_file="$1"
  if [ -s "$result_file" ] && /usr/bin/plutil -lint "$result_file" >/dev/null 2>&1; then
    /usr/bin/plutil -p "$result_file" >&2 || true
  fi
}

submit_and_require_accepted() {
  artifact="$1"
  result_file="$2"
  log_file="$3"
  label="$4"

  /bin/rm -f "$result_file" "$log_file"
  if /usr/bin/xcrun notarytool submit "$artifact" \
      --keychain-profile "$NOTARY_PROFILE" \
      --wait --output-format plist >"$result_file"; then
    :
  else
    submit_status=$?
    show_notary_result "$result_file"
    fail "$label notarization command failed with status $submit_status"
  fi

  /usr/bin/plutil -lint "$result_file" >/dev/null ||
    fail "$label notarization returned an invalid result"
  notary_status="$(/usr/bin/plutil -extract status raw -o - "$result_file")"
  submission_id="$(/usr/bin/plutil -extract id raw -o - "$result_file")"

  [ -n "$submission_id" ] || fail "$label notarization did not return a submission ID"
  printf '%s notarization: %s (%s)\n' "$label" "$notary_status" "$submission_id"

  if [ "$notary_status" != "Accepted" ]; then
    /usr/bin/xcrun notarytool log \
      --keychain-profile "$NOTARY_PROFILE" \
      "$submission_id" "$log_file" || true
    fail "$label notarization was not accepted; inspect $result_file and $log_file"
  fi

  /usr/bin/xcrun notarytool log \
    --keychain-profile "$NOTARY_PROFILE" \
    "$submission_id" "$log_file"
}

if [ "${1:-}" = "--help" ] || [ "${1:-}" = "-h" ]; then
  usage
  exit 0
fi
[ "$#" -eq 0 ] || fail "unknown argument: $1 (use --help)"

[ "$(/usr/bin/uname -s)" = "Darwin" ] || fail "this script requires macOS"
[ -n "${SIGN_IDENTITY:-}" ] || fail "SIGN_IDENTITY is required (use --help for the exact command)"
[ -n "${NOTARY_PROFILE:-}" ] || fail "NOTARY_PROFILE is required (use --help for the exact command)"

[[ "$VERSION" =~ ^[0-9]+\.[0-9]+(\.[0-9]+)?$ ]] ||
  fail "VERSION must contain two or three numeric components"
[[ "$BUILD_NUMBER" =~ ^[0-9]+(\.[0-9]+)?(\.[0-9]+)?$ ]] ||
  fail "BUILD_NUMBER must contain one to three numeric components"
[[ "$MACOS_DEPLOYMENT_TARGET" =~ ^[0-9]+\.[0-9]+(\.[0-9]+)?$ ]] ||
  fail "MACOS_DEPLOYMENT_TARGET must be a numeric macOS version"

require_command /usr/bin/codesign
require_command /usr/bin/ditto
require_command /usr/bin/hdiutil
require_command /usr/bin/plutil
require_command /usr/bin/security
require_command /usr/bin/xcrun
require_command /usr/sbin/spctl

if ! /usr/bin/security find-identity -v -p codesigning |
    /usr/bin/grep -F -- "\"$SIGN_IDENTITY\"" >/dev/null; then
  fail "Developer ID signing identity is not available in the current Keychain: $SIGN_IDENTITY"
fi

cd "$REPO_ROOT"
[ ! -L "$RELEASE_DIR" ] || fail "release directory must not be a symbolic link: $RELEASE_DIR"
/bin/mkdir -p "$RELEASE_DIR"
[ ! -L "$RELEASE_DIR" ] || fail "release directory must not be a symbolic link: $RELEASE_DIR"
[ ! -d "$ZIP" ] || fail "release ZIP path must not be a directory: $ZIP"
[ ! -d "$DMG" ] || fail "release DMG path must not be a directory: $DMG"
trap cleanup EXIT

step "Rebuilding and verifying the unsigned app"
VERSION="$VERSION" \
BUILD_NUMBER="$BUILD_NUMBER" \
BUNDLE_IDENTIFIER="$BUNDLE_IDENTIFIER" \
MACOS_DEPLOYMENT_TARGET="$MACOS_DEPLOYMENT_TARGET" \
  "$REPO_ROOT/launcher/build-app.sh"

[ -d "$APP" ] || fail "app was not created: $APP"
[ -f "$RUNTIME" ] || fail "bundled runtime is missing: $RUNTIME"

/bin/rm -f "$ZIP_TEMP" "$DMG_TEMP" "$APP_UPLOAD_ZIP"
/bin/rm -rf "$DMG_STAGE"

step "Signing the native runtime and application"
/usr/bin/codesign --force --options runtime --timestamp \
  --sign "$SIGN_IDENTITY" "$RUNTIME"
/usr/bin/codesign --force --options runtime --timestamp \
  --sign "$SIGN_IDENTITY" "$APP"
/usr/bin/codesign --verify --deep --strict --verbose=2 "$APP"

step "Notarizing the application"
/usr/bin/ditto -c -k --sequesterRsrc --keepParent "$APP" "$APP_UPLOAD_ZIP"
submit_and_require_accepted "$APP_UPLOAD_ZIP" "$APP_RESULT" "$APP_LOG" "App"

step "Stapling and assessing the application"
/usr/bin/xcrun stapler staple -v "$APP"
/usr/bin/xcrun stapler validate -v "$APP"
/usr/bin/codesign --verify --deep --strict --verbose=2 "$APP"
/usr/sbin/spctl --assess --type execute --verbose=4 "$APP"

step "Creating the distributable ZIP from the stapled application"
/usr/bin/ditto -c -k --sequesterRsrc --keepParent "$APP" "$ZIP_TEMP"

step "Creating and signing the DMG"
/bin/mkdir -p "$DMG_STAGE"
/usr/bin/ditto "$APP" "$DMG_STAGE/GoldenEye Metal.app"
/bin/ln -s /Applications "$DMG_STAGE/Applications"
/usr/bin/hdiutil create \
  -volname "GoldenEye Metal" \
  -srcfolder "$DMG_STAGE" \
  -fs HFS+ -format UDZO -ov "$DMG_TEMP"
/usr/bin/codesign --force --timestamp --sign "$SIGN_IDENTITY" "$DMG_TEMP"
/usr/bin/codesign --verify --verbose=2 "$DMG_TEMP"
/usr/bin/hdiutil verify "$DMG_TEMP"

step "Notarizing the DMG"
submit_and_require_accepted "$DMG_TEMP" "$DMG_RESULT" "$DMG_LOG" "DMG"

step "Stapling and assessing the DMG"
/usr/bin/xcrun stapler staple -v "$DMG_TEMP"
/usr/bin/xcrun stapler validate -v "$DMG_TEMP"
/usr/bin/codesign --verify --verbose=2 "$DMG_TEMP"
/usr/bin/hdiutil verify "$DMG_TEMP"
/usr/sbin/spctl --assess --type open \
  --context context:primary-signature --verbose=4 "$DMG_TEMP"

step "Publishing verified release artifacts"
/bin/mv -f "$ZIP_TEMP" "$ZIP"
/bin/mv -f "$DMG_TEMP" "$DMG"

printf '\nRelease artifacts are ready:\n'
printf '  %s\n' "$ZIP"
printf '  %s\n' "$DMG"
printf '\nApple notarization results and logs are in:\n  %s\n' "$RELEASE_DIR"
