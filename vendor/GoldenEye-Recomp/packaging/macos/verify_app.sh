#!/bin/bash

set -euo pipefail

APP_BUNDLE="${1:-}"

fail() {
  printf 'GoldenEye Metal app verification failed: %s\n' "$1" >&2
  exit 1
}

[ "$(uname -s)" = "Darwin" ] || fail "this verifier requires macOS"
[ -n "$APP_BUNDLE" ] || fail "usage: verify_app.sh /path/to/GoldenEye\\ Metal.app"
[ -d "$APP_BUNDLE" ] || fail "bundle does not exist: $APP_BUNDLE"

CONTENTS="$APP_BUNDLE/Contents"
INFO_PLIST="$CONTENTS/Info.plist"
EXECUTABLE="$CONTENTS/MacOS/GoldenEye"
RUNTIME="$CONTENTS/Frameworks/librexruntime.dylib"
ICON="$CONTENTS/Resources/GoldenEyeMetal.icns"
ICON_AUDIT_DIR="$(/usr/bin/mktemp -d "${TMPDIR:-/tmp}/goldeneye-icon.XXXXXX")"
trap '/bin/rm -rf "$ICON_AUDIT_DIR"' EXIT

[ -f "$INFO_PLIST" ] || fail "Contents/Info.plist is missing"
[ -x "$EXECUTABLE" ] || fail "Contents/MacOS/GoldenEye is missing or not executable"
[ -f "$RUNTIME" ] || fail "Contents/Frameworks/librexruntime.dylib is missing"
[ -f "$ICON" ] || fail "Contents/Resources/GoldenEyeMetal.icns is missing"
[ -f "$CONTENTS/Resources/Licenses/SPIRV-Cross-LICENSE.txt" ] ||
  fail "the SPIRV-Cross binary license is missing"
[ -f "$CONTENTS/Resources/Licenses/sdl3__LICENSE.txt" ] ||
  fail "the SDL binary license is missing"
[ -f "$CONTENTS/Resources/Licenses/FFmpeg__LICENSE.md" ] ||
  fail "the FFmpeg binary license is missing"

/usr/bin/iconutil -c iconset "$ICON" -o "$ICON_AUDIT_DIR/GoldenEyeMetal.iconset" ||
  fail "the application icon is not a valid ICNS file"
[ -f "$ICON_AUDIT_DIR/GoldenEyeMetal.iconset/icon_512x512@2x.png" ] ||
  fail "the application icon is missing its 1024-pixel representation"

if /usr/bin/find "$CONTENTS/MacOS" -mindepth 1 -maxdepth 1 \
    ! -name GoldenEye -print -quit | /usr/bin/grep -q .; then
  fail "Contents/MacOS contains an unexpected file"
fi
if /usr/bin/find "$CONTENTS/Frameworks" -mindepth 1 -maxdepth 1 \
    ! -name librexruntime.dylib -print -quit | /usr/bin/grep -q .; then
  fail "Contents/Frameworks contains an unexpected file"
fi
if /usr/bin/find "$CONTENTS/Resources" -mindepth 1 -maxdepth 1 \
    ! -name GoldenEyeMetal.icns ! -name Licenses -print -quit |
    /usr/bin/grep -q .; then
  fail "Contents/Resources contains an unexpected file"
fi
if /usr/bin/find "$CONTENTS/Resources/Licenses" -type f -size +2M \
    -print -quit | /usr/bin/grep -q .; then
  fail "an unexpectedly large file is staged as a license notice"
fi
if /usr/bin/find "$CONTENTS" -type l -print -quit | /usr/bin/grep -q .; then
  fail "the application bundle contains an unexpected symbolic link"
fi

/usr/bin/plutil -lint "$INFO_PLIST" >/dev/null || fail "Info.plist is invalid"

plist_value() {
  /usr/libexec/PlistBuddy -c "Print :$1" "$INFO_PLIST" 2>/dev/null
}

[ "$(plist_value CFBundleExecutable)" = "GoldenEye" ] ||
  fail "CFBundleExecutable must be GoldenEye"
[ "$(plist_value CFBundlePackageType)" = "APPL" ] ||
  fail "CFBundlePackageType must be APPL"
[ -n "$(plist_value CFBundleIdentifier)" ] || fail "CFBundleIdentifier is empty"
[ "$(plist_value CFBundleIconFile)" = "GoldenEyeMetal" ] ||
  fail "CFBundleIconFile must be GoldenEyeMetal"
[ -n "$(plist_value CFBundleShortVersionString)" ] ||
  fail "CFBundleShortVersionString is empty"
[ -n "$(plist_value CFBundleVersion)" ] || fail "CFBundleVersion is empty"
PLIST_MINIMUM_SYSTEM_VERSION="$(plist_value LSMinimumSystemVersion)"
[ -n "$PLIST_MINIMUM_SYSTEM_VERSION" ] || fail "LSMinimumSystemVersion is empty"

for binary in "$EXECUTABLE" "$RUNTIME"; do
  /usr/bin/file "$binary" | /usr/bin/grep -q 'Mach-O 64-bit' ||
    fail "not a 64-bit Mach-O binary: $binary"
  /usr/bin/lipo -archs "$binary" | /usr/bin/grep -Eq '(^|[[:space:]])arm64($|[[:space:]])' ||
    fail "arm64 slice is missing: $binary"

  while IFS= read -r dependency; do
    case "$dependency" in
      @*|/System/Library/*|/usr/lib/*) ;;
      *) fail "non-portable dependency in $binary: $dependency" ;;
    esac
  done < <(
    /usr/bin/otool -L "$binary" |
      /usr/bin/tail -n +2 |
      /usr/bin/sed 's/^[[:space:]]*//; s/[[:space:]]*(compatibility.*$//'
  )

  if /usr/bin/strings "$binary" |
      /usr/bin/grep -E '/Users/[^/[:space:]]+/|/home/[^/[:space:]]+/' >/dev/null; then
    fail "a build-owner home-directory path is embedded in $binary"
  fi
done

for binary in "$EXECUTABLE" "$RUNTIME"; do
  BINARY_MINIMUM_SYSTEM_VERSION="$(${VTOOL:-/usr/bin/vtool} -show-build "$binary" |
    /usr/bin/awk '$1 == "minos" { print $2; exit }')"
  [ "$BINARY_MINIMUM_SYSTEM_VERSION" = "$PLIST_MINIMUM_SYSTEM_VERSION" ] ||
    fail "LSMinimumSystemVersion ($PLIST_MINIMUM_SYSTEM_VERSION) does not match $binary ($BINARY_MINIMUM_SYSTEM_VERSION)"
done

/usr/bin/otool -L "$EXECUTABLE" |
  /usr/bin/grep -q '@rpath/librexruntime.dylib' ||
  fail "the executable does not load the bundled runtime through @rpath"
/usr/bin/otool -D "$RUNTIME" |
  /usr/bin/tail -n +2 |
  /usr/bin/grep -Fxq '@rpath/librexruntime.dylib' ||
  fail "the bundled runtime has a non-portable install name"

RPATHS="$(
  /usr/bin/otool -l "$EXECUTABLE" |
    /usr/bin/awk '
      $1 == "cmd" && $2 == "LC_RPATH" { want_path = 1; next }
      want_path && $1 == "path" { print $2; want_path = 0 }
    '
)"
[ "$RPATHS" = '@executable_path/../Frameworks' ] ||
  fail "expected only @executable_path/../Frameworks, found: ${RPATHS:-<none>}"

if /usr/bin/find "$CONTENTS" \( -iname '*.zip' -o -iname '*.7z' -o -iname '*.rar' \
    -o -iname '*.iso' -o -iname '*.xex' -o -iname '*.xbe' -o -iname '*.xcp' \
    -o -iname '*.xzp' -o -iname '*.stfs' -o -iname '*.xwb' \) \
    -print -quit | /usr/bin/grep -q .; then
  fail "game-content files were included in the app bundle"
fi
if [ -d "$CONTENTS/MacOS/files" ] || [ -d "$CONTENTS/Resources/files" ]; then
  fail "an extracted game-data directory was included in the app bundle"
fi

printf 'GoldenEye Metal app verification passed.\n'
printf '  Bundle: %s\n' "$APP_BUNDLE"
printf '  Version: %s (%s)\n' \
  "$(plist_value CFBundleShortVersionString)" "$(plist_value CFBundleVersion)"
printf '  Identifier: %s\n' "$(plist_value CFBundleIdentifier)"
printf '  Minimum macOS: %s\n' "$PLIST_MINIMUM_SYSTEM_VERSION"
printf '  Runtime: bundled and portable\n'
printf '  Recognizable standalone game data: not staged\n'
