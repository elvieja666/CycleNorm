#!/bin/bash
# Borra CycleNorm y sus ajustes. Los archivos que exportaste (y ~/Music/CycleNorm) no se tocan.
osascript -e 'quit app "CycleNorm"' 2>/dev/null
rm -rf "/Applications/CycleNorm.app"
rm -f "$HOME/Library/Application Support/CycleNorm.settings"
echo "CycleNorm desinstalado. (La carpeta ~/Music/CycleNorm con los archivos arrastrados al DAW queda; borrala a mano si queres.)"
echo ""
read -n 1 -s -r -p "Toca una tecla para cerrar..."
