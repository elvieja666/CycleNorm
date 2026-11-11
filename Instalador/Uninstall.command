#!/bin/bash
# Removes CycleNorm and its settings. Files you exported (and ~/Music/CycleNorm) are not touched.
osascript -e 'quit app "CycleNorm"' 2>/dev/null
rm -rf "/Applications/CycleNorm.app"
rm -f "$HOME/Library/Application Support/CycleNorm.settings"
echo "CycleNorm uninstalled. (The ~/Music/CycleNorm folder with files dragged to your DAW is kept; delete it by hand if you want.)"
echo ""
read -n 1 -s -r -p "Press any key to close..."
