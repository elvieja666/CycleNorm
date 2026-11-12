#!/bin/bash
# Toolkit installer for Mac. Keep it in the same folder as Toolkit.app (as it comes in the zip) and double-click it.
cd "$(dirname "$0")"
if [ ! -d "Toolkit.app" ]; then
  echo "Toolkit.app was not found next to this file."
else
  echo "Installing Toolkit.app into /Applications ..."
  osascript -e 'quit app "Toolkit"' 2>/dev/null
  osascript -e 'quit app "CycleNorm"' 2>/dev/null
  rm -rf "/Applications/CycleNorm.app"                                        # the old name of this app
  rm -rf "/Applications/Toolkit.app"
  cp -R "Toolkit.app" "/Applications/"
  xattr -cr "/Applications/Toolkit.app"                                       # remove macOS quarantine (downloaded from the internet)
  codesign --force --deep --sign - "/Applications/Toolkit.app" 2>/dev/null    # local signature, so macOS lets it open
  echo ""
  echo "Done. Toolkit is in Applications."
  echo ""
  open "/Applications/Toolkit.app"
fi
