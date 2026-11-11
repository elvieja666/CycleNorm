#!/bin/bash
# CycleNorm installer for Mac. Keep it in the same folder as CycleNorm.app (as it comes in the zip) and double-click it.
cd "$(dirname "$0")" || exit 1

if [ ! -d "CycleNorm.app" ]; then
  echo "CycleNorm.app was not found next to this file."
else
  echo "Installing CycleNorm.app into /Applications ..."
  osascript -e 'quit app "CycleNorm"' 2>/dev/null
  rm -rf "/Applications/CycleNorm.app"
  cp -R "CycleNorm.app" "/Applications/"
  xattr -cr "/Applications/CycleNorm.app"                                     # remove macOS quarantine (downloaded from the internet)
  codesign --force --deep --sign - "/Applications/CycleNorm.app" 2>/dev/null  # local signature, so macOS lets it open
  echo ""
  echo "Done. CycleNorm is in Applications."
  echo "This Mac's architecture: $(uname -m)"
  open "/Applications/CycleNorm.app"
fi
echo ""
read -n 1 -s -r -p "Press any key to close..."
