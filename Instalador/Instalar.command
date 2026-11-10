#!/bin/bash
# Instalador de CycleNorm para Mac. Dejalo en la misma carpeta que CycleNorm.app (como viene en el zip) y hace doble clic.
cd "$(dirname "$0")" || exit 1

if [ ! -d "CycleNorm.app" ]; then
  echo "No encontre CycleNorm.app al lado de este archivo."
else
  echo "Instalando CycleNorm.app en /Applications ..."
  osascript -e 'quit app "CycleNorm"' 2>/dev/null
  rm -rf "/Applications/CycleNorm.app"
  cp -R "CycleNorm.app" "/Applications/"
  xattr -cr "/Applications/CycleNorm.app"                                  # saca el bloqueo de macOS (descargado de internet)
  codesign --force --deep --sign - "/Applications/CycleNorm.app" 2>/dev/null  # firma local, para que macOS la deje abrir
  echo ""
  echo "Listo. CycleNorm esta en Aplicaciones."
  echo "Arquitectura de esta Mac: $(uname -m)"
  open "/Applications/CycleNorm.app"
fi
echo ""
read -n 1 -s -r -p "Toca una tecla para cerrar..."
