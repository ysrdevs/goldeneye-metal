# macOS application distribution

The `goldeneye_macos_app` target creates an **unsigned**, self-contained Apple
Silicon application at:

```text
vendor/GoldenEye-Recomp/out/build/macos-arm64-release/dist/GoldenEye Metal.app
```

The bundle contains the native game executable, `librexruntime.dylib`, its
property list, original app icon, and license notices. It does not contain an
XEX, extracted game assets, generated game data, or a network downloader.

On first launch, the native setup window accepts a user-selected backup ZIP,
its Xbox LIVE/STFS package, or an extracted game-data folder. The ZIP path is
strictly local: the launcher extracts only the single package member, verifies
the exact supported SHA-256 identity, imports into
`~/Library/Application Support/GoldenEye Metal/Game Data`, and removes the
temporary package. A valid cache or remembered extracted folder skips setup on
later launches. Holding Option while opening the app forces the setup window to
appear again; a newly selected extracted folder is remembered ahead of the
previous cache.

The import window exposes Cancel while work is active. Package extraction is
bounded by size and time, game-file extraction checks cancellation between
chunks, and atomic publication leaves an existing valid cache untouched on
failure or cancellation. Later launches remove strictly named abandoned import
artifacts after a six-hour safety window and reject a cache whose marker, file
count, byte total, executable, or critical resources no longer match.

The staging step also gathers the license files for statically linked source
dependencies and the separately installed SPIRV-Cross package. It fails if
the SPIRV-Cross license cannot be found, preventing an incomplete binary
distribution.

The official Apple Silicon presets currently target macOS 14.0 and record the
same minimum version in the bundle metadata. Keep the values in the root
`CMakePresets.json` and `vendor/GoldenEye-Recomp/CMakePresets.json` aligned.
Raise both deliberately when the native code adopts an API that requires a
newer macOS release.

All linked objects must support the advertised deployment target. In
particular, a Homebrew SPIRV-Cross package installed on a newer macOS release
may have been compiled for that newer release. Treat a linker warning such as
"was built for newer macOS version" as a release blocker: rebuild
SPIRV-Cross with `CMAKE_OSX_DEPLOYMENT_TARGET=14.0`, build the release on a
macOS 14-compatible build host, or deliberately raise the application's
minimum version. A final binary reporting `minos 14.0` does not by itself make
newer static-library objects compatible with macOS 14.

## 1. Build and verify the unsigned app

Complete code generation first, then configure the release with the version,
build number, and final bundle identifier you intend to ship:

```sh
cmake --preset macos-arm64-release
cmake --build --preset macos-arm64-release \
  --target rexruntime --parallel

cmake -S vendor/GoldenEye-Recomp --preset macos-arm64-release \
  -DGOLDENEYE_VERSION=0.1.0 \
  -DGOLDENEYE_BUILD_NUMBER=1 \
  -DGOLDENEYE_BUNDLE_IDENTIFIER=io.github.ysrdevs.goldeneye-metal

cmake --build vendor/GoldenEye-Recomp/out/build/macos-arm64-release \
  --target goldeneye_macos_app_verify --parallel
```

The verification target checks the bundle metadata, Apple Silicon slices,
portable dynamic-library linkage and RPATH, and confirms that no recognizable
game-content files were staged. It intentionally does not sign anything.

Open this unsigned build and exercise the first-launch import with a local
backup stored outside the application before starting the release process.
Quit, reopen it, and confirm that the imported cache launches without showing
the setup window again. Do not modify any file inside the app after it has
been signed.

## 2. Sign the app with hardened runtime

The following commands are for the release owner to run. Replace the identity
with the exact Developer ID Application identity shown by Keychain Access or
`security find-identity -v -p codesigning`.

```sh
export APP="$PWD/vendor/GoldenEye-Recomp/out/build/macos-arm64-release/dist/GoldenEye Metal.app"
export DEVELOPER_ID_APPLICATION='Developer ID Application: Your Name (TEAMID)'

/usr/bin/codesign --force --options runtime --timestamp \
  --sign "$DEVELOPER_ID_APPLICATION" \
  "$APP/Contents/Frameworks/librexruntime.dylib"

/usr/bin/codesign --force --options runtime --timestamp \
  --sign "$DEVELOPER_ID_APPLICATION" \
  "$APP"

/usr/bin/codesign --verify --deep --strict --verbose=2 "$APP"
```

Sign nested code first and the outer app last. Do not use `codesign --deep` to
perform the signing; it can conceal missing or incorrectly signed nested code.
This application does not use the App Sandbox and currently needs no custom
entitlements. Gatekeeper assessment is performed after notarization; an
otherwise valid Developer ID app may still be reported as unnotarized at this
stage.

## 3. Create and sign the DMG

Create a clean staging directory so the disk image contains only the app and
an Applications shortcut:

```sh
export RELEASE_DIR="$PWD/release"
export DMG="$RELEASE_DIR/GoldenEye-Metal-0.1.0-macos-arm64.dmg"
export DMG_STAGE="$RELEASE_DIR/dmg-root"

rm -rf "$DMG_STAGE"
mkdir -p "$DMG_STAGE" "$RELEASE_DIR"
/usr/bin/ditto "$APP" "$DMG_STAGE/GoldenEye Metal.app"
ln -s /Applications "$DMG_STAGE/Applications"

/usr/bin/hdiutil create \
  -volname "GoldenEye Metal" \
  -srcfolder "$DMG_STAGE" \
  -format UDZO -ov "$DMG"

/usr/bin/codesign --force --timestamp \
  --sign "$DEVELOPER_ID_APPLICATION" "$DMG"
/usr/bin/codesign --verify --verbose=2 "$DMG"
/usr/bin/hdiutil verify "$DMG"
```

The `release/` directory is ignored by Git. Never add imported or extracted
game data to the DMG staging directory.

## 4. Store notarization credentials once

Use an app-specific password for the Apple Account associated with the
Developer ID team. This stores the credential in the login keychain rather
than in a shell script or repository:

```sh
xcrun notarytool store-credentials "goldeneye-metal-notary" \
  --apple-id "YOUR_APPLE_ACCOUNT_EMAIL" \
  --team-id "YOUR_TEAM_ID"
```

`notarytool` securely prompts for the app-specific password. Do not put that
password directly on the command line, where it may remain in shell history.
Do not commit Apple Account credentials, private keys, certificates, or
notarization output containing private account information.

## 5. Notarize, staple, and assess the DMG

```sh
xcrun notarytool submit "$DMG" \
  --keychain-profile "goldeneye-metal-notary" \
  --wait

# Copy the submission ID printed above, then inspect its complete log.
xcrun notarytool log "SUBMISSION_ID" \
  --keychain-profile "goldeneye-metal-notary"

xcrun stapler staple "$DMG"
xcrun stapler validate "$DMG"
/usr/sbin/spctl --assess --type open \
  --context context:primary-signature --verbose=4 "$DMG"
```

`notarytool submit --wait` must report `Accepted` before stapling. Inspect the
successful log as well and confirm that the application and embedded runtime
appear as expected. If Apple rejects the submission, use the log to correct
the reported issue, rebuild from the unsigned app, and repeat the signing
process.

Before publishing, mount the final DMG on a separate macOS user account or a
clean Mac, drag the app to Applications, launch it through Finder, import only
user-supplied game data, and test keyboard, mouse, and at least one controller.
