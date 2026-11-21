#!/usr/bin/env bash
# iOS / iPadOS build.
#   scripts/build_ios.sh simulator        unsigned build for the iOS Simulator (no Apple account needed)
#   TEAM_ID=ABCDE12345 scripts/build_ios.sh device   signed .ipa for devices you registered (needs an Apple Developer account)
set -euo pipefail
cd "$(dirname "$0")/.."
MODE="${1:-simulator}"

cmake --preset ios
PROJ="build/ios/PatchBay.xcodeproj"

if [ "$MODE" = "simulator" ]; then
  xcodebuild -project "$PROJ" -scheme PatchBay -configuration Release -sdk iphonesimulator \
             -destination 'generic/platform=iOS Simulator' CODE_SIGNING_ALLOWED=NO build
  echo "Simulator app is under ~/Library/Developer/Xcode/DerivedData/*/Build/Products/Release-iphonesimulator/"
else
  : "${TEAM_ID:?set TEAM_ID to your Apple Developer team id}"
  xcodebuild -project "$PROJ" -scheme PatchBay -configuration Release -sdk iphoneos \
             -archivePath build/PatchBay.xcarchive DEVELOPMENT_TEAM="$TEAM_ID" -allowProvisioningUpdates archive
  sed "s/__TEAM_ID__/$TEAM_ID/" scripts/ExportOptions.plist > build/ExportOptions.plist
  xcodebuild -exportArchive -archivePath build/PatchBay.xcarchive -exportOptionsPlist build/ExportOptions.plist \
             -exportPath build/ipa -allowProvisioningUpdates
  echo "IPA: build/ipa/PatchBay.ipa"
fi
