#!/usr/bin/env bash
# Builds PatchBay.app (universal arm64 + x86_64) and wraps it in PatchBay.dmg. Run on a Mac with Xcode + CMake.
#   scripts/build_macos_dmg.sh
# Optional environment:
#   CODESIGN_IDENTITY   "Developer ID Application: Your Name (TEAMID)"  (default: ad-hoc, fine for your own Mac only)
#   NOTARY_PROFILE      a `xcrun notarytool store-credentials` profile name; enables notarisation + stapling
set -euo pipefail
cd "$(dirname "$0")/.."

cmake --preset macos -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_BUILD_TYPE=Release
cmake --build build/macos --config Release --parallel

APP="$(find build/macos -type d -name 'PatchBay.app' -path '*Release*' | head -1)"
[ -n "$APP" ] || { echo "PatchBay.app not found in build/macos" >&2; exit 1; }

IDENTITY="${CODESIGN_IDENTITY:--}"
codesign --force --deep --options runtime --timestamp=none -s "$IDENTITY" "$APP"
[ "$IDENTITY" = "-" ] || codesign --force --deep --options runtime --timestamp -s "$IDENTITY" "$APP"

STAGE="$(mktemp -d)"
cp -R "$APP" "$STAGE/"
ln -s /Applications "$STAGE/Applications"
rm -f PatchBay.dmg
hdiutil create -volname "PatchBay" -srcfolder "$STAGE" -ov -format UDZO PatchBay.dmg
rm -rf "$STAGE"

if [ -n "${NOTARY_PROFILE:-}" ] && [ "$IDENTITY" != "-" ]; then
  codesign -s "$IDENTITY" --timestamp PatchBay.dmg
  xcrun notarytool submit PatchBay.dmg --keychain-profile "$NOTARY_PROFILE" --wait
  xcrun stapler staple PatchBay.dmg
fi
echo "Built $(pwd)/PatchBay.dmg"
