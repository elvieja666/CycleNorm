#!/bin/bash
# Removes Toolkit and its settings. Files you exported (and ~/Music/Toolkit) are not touched.
osascript -e 'quit app "Toolkit"' 2>/dev/null
rm -rf "/Applications/Toolkit.app" "/Applications/CycleNorm.app"
rm -f "$HOME/Library/Application Support/Toolkit.settings" "$HOME/Library/Application Support/CycleNorm.settings"
echo "Toolkit uninstalled. (The ~/Music/Toolkit folder with files dragged to your DAW is kept; delete it by hand if you want.)"
