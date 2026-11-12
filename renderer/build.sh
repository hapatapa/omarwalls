#!/usr/bin/env bash
# Build webwall-host (QtWebEngine/QtMultimedia wallpaper renderer) and
# shader-host (pure-Vulkan shadertoy renderer) for this plugin.
# Safe to re-run; skips a build when the binary is newer than its sources
# (pass `rebuild` to force).
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FORCE="${1:-}"

# --- webwall-host (Qt) ---
if [[ "$FORCE" == "rebuild" || ! -x "$REPO/webwall-host" || "$REPO/renderer.cpp" -nt "$REPO/webwall-host" ]]; then
  g++ -fPIC -o "$REPO/webwall-host" "$REPO/renderer.cpp" \
    $(pkg-config --cflags --libs Qt6Quick Qt6Qml Qt6WebEngineQuick Qt6WebEngineCore Qt6Multimedia)
  echo "webwall-host built"
else
  echo "webwall-host up to date"
fi

# --- shader-host (Vulkan) ---
if [[ "$FORCE" == "rebuild" || ! -x "$REPO/shader-host" || "$REPO/shader-host.cpp" -nt "$REPO/shader-host" \
      || "$REPO/viewporter-protocol.c" -nt "$REPO/shader-host" ]]; then
  cd "$REPO"
  if [[ ! -f "$REPO/viewporter-protocol.c" || ! -f "$REPO/viewporter-client-protocol.h" ]]; then
    wayland-scanner client-header /usr/share/wayland-protocols/stable/viewporter/viewporter.xml \
      "$REPO/viewporter-client-protocol.h"
    wayland-scanner private-code /usr/share/wayland-protocols/stable/viewporter/viewporter.xml \
      "$REPO/viewporter-protocol.c"
  fi
  TMPD="$(mktemp -d)"
  trap 'rm -rf "$TMPD"' EXIT
  gcc -O2 -c wlr-layer-shell-unstable-v1-protocol.c -o "$TMPD/wlr.o"
  gcc -O2 -c xdg-shell-protocol.c -o "$TMPD/xdg.o"
  gcc -O2 -c viewporter-protocol.c -o "$TMPD/viewporter.o"
  g++ -std=c++17 -O2 -fPIC -I"$REPO" -o "$REPO/shader-host" \
    "$REPO/shader-host.cpp" "$TMPD/wlr.o" "$TMPD/xdg.o" "$TMPD/viewporter.o" \
    $(pkg-config --cflags --libs wayland-client shaderc) -lvulkan
  rm -rf "$TMPD"
  echo "shader-host built"
else
  echo "shader-host up to date"
fi

echo "done → $REPO/{webwall-host,shader-host}"