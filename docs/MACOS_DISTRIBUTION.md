# macOS application distribution

The `goldeneye_macos_app` target creates an **unsigned**, self-contained Apple
Silicon application at:

```text
vendor/GoldenEye-Recomp/out/build/macos-arm64-release/dist/GoldenEye Metal.app
```

The bundle contains the native game executable, `librexruntime.dylib`, its
property list, original app icon, and license notices. It copies no XEX,
extracted asset tree, generated C++ source, or standalone game-data files into
the bundle, and it has no network downloader. The native executable is built
from the release owner's local generated integration.

## Automated release-owner workflow

From the repository root, run:

```sh
./launcher/build-app.sh
SIGN_IDENTITY="Developer ID Application: YUVRAJ SINGH (9RCV543M32)" \
NOTARY_PROFILE="cyberconsole-notary" \
./tools/sign-notarize.sh
```

The release script invokes `build-app.sh` itself, so the first command is
useful for testing the unsigned app but is optional immediately before the
second command. It signs nested code before the outer app, notarizes and
staples the app, creates the distributable ZIP from that stapled app, then
creates, signs, notarizes, staples, and Gatekeeper-assesses the DMG. It writes:

```text
release/GoldenEye-Metal-0.1.0-macos-arm64.zip
release/GoldenEye-Metal-0.1.0-macos-arm64.dmg
```

The scripts accept `VERSION`, `BUILD_NUMBER`, `BUNDLE_IDENTIFIER`, and
`MACOS_DEPLOYMENT_TARGET` overrides. `release/` is ignored by Git. The scripts
never accept an Apple password and never download an XEX or game assets. The
release owner is responsible for confirming the right to distribute the
compiled output produced from their local generated integration; this document
is not legal advice.

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

The staging step also gathers license files for statically linked source
dependencies. `build-app.sh` downloads a pinned official SPIRV-Cross source
archive, verifies its SHA-256, builds it locally for the declared deployment
target, and includes its license. It fails if that license cannot be found,
preventing an incomplete binary distribution.

The official Apple Silicon presets currently target macOS 14.0 and record the
same minimum version in the bundle metadata. Keep the values in the root
`CMakePresets.json` and `vendor/GoldenEye-Recomp/CMakePresets.json` aligned.
Raise both deliberately when the native code adopts an API that requires a
newer macOS release.

All linked objects must support the advertised deployment target. A Homebrew
SPIRV-Cross bottle installed on a newer macOS release may target that newer
release even when the final executable reports `minos 14.0`. The automated
build avoids that mismatch and audits every object in the three SPIRV-Cross
archives before configuring the app. A custom `SPIRV_CROSS_PREFIX` override is
also audited and is rejected if any object requires a newer macOS version.

## 1. Build and verify the unsigned app

Complete code generation first, then run:

```sh
./launcher/build-app.sh
```

The script configures the SDK and game release, builds the pinned graphics
dependency and native runtime, then invokes `goldeneye_macos_app_verify`. The
verification target checks bundle metadata, Apple Silicon slices, portable
dynamic-library linkage and RPATH, and confirms that no recognizable game
content was staged. It intentionally does not sign anything.

Open this unsigned build and exercise the first-launch import with a local
backup stored outside the application before starting the release process.
Quit, reopen it, and confirm that the imported cache launches without showing
the setup window again. During both setup and gameplay, confirm Command-Q,
Command-W or the red close button, the application menu's Quit item, and Dock
Quit all end the process without Force Quit. Do not modify any file inside the
app after it has been signed.

## 2. Store notarization credentials once

The existing `cyberconsole-notary` Keychain profile can be reused for this
project because it represents the same Apple developer account and team. If it
ever needs to be recreated, use an app-specific password and let `notarytool`
prompt securely:

```sh
xcrun notarytool store-credentials "cyberconsole-notary" \
  --apple-id "YOUR_APPLE_ACCOUNT_EMAIL" \
  --team-id "9RCV543M32"
```

Do not put an app-specific password directly in a command or script. Do not
commit Apple Account credentials, private keys, certificates, or notarization
output. The profile name is not a secret; the credential stored in Keychain is.

## 3. What the release script verifies

The automated release deliberately performs the complete sequence:

1. Recreate and validate the unsigned app.
2. Sign `librexruntime.dylib`, then the outer app, with hardened runtime and a
   trusted timestamp. No JIT or library-validation exceptions are used.
3. Submit a temporary app ZIP and require Apple's explicit `Accepted` status.
4. Staple, validate, and Gatekeeper-assess the app.
5. Create the public ZIP from the stapled app.
6. Create a DMG containing only the stapled app and an Applications shortcut.
7. Sign, verify, notarize, staple, validate, and Gatekeeper-assess the DMG.

The Apple result plists and full notarization logs are retained under the
ignored `release/` directory. Temporary upload and DMG-staging files are
removed automatically. Final-looking ZIP and DMG names are replaced only after
every signing, notarization, and validation check succeeds, so a rejected or
invalid candidate does not overwrite the last completed release. The
application needs no custom entitlements, and the script does not use
`codesign --deep` to perform signing.

Before publishing, mount the final DMG on a separate macOS user account or a
clean Mac, drag the app to Applications, launch it through Finder, import only
user-supplied game data, and test keyboard, mouse, and at least one controller.
