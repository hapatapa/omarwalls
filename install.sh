#!/usr/bin/env bash
# One-time live-install setup for the hapatapa.omarwalls plugin.
#
# `omarchy plugin add` clones and validates a plugin but has no post-install
# hook, so the service runs this on first load. It is idempotent: link the CLI
# into ~/.local/bin, add the login autostart line, and write the
# menu > Style > Omarwalls row into the user's Omarchy menu extension.
set -euo pipefail

PLUGIN_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CLI="$PLUGIN_DIR/bin/omarchy-webwall"
BIN_DIR="${HOME}/.local/bin"
AUTOSTART="${HOME}/.config/hypr/autostart.lua"
MENU_EXT="${HOME}/.config/omarchy/extensions/omarchy-menu.jsonc"
MARK_START='// omarchy-webwall-menu-start'
MARK_END='// omarchy-webwall-menu-end'

# --- 1. CLI on PATH -------------------------------------------------------
mkdir -p "$BIN_DIR"
ln -sfn "$CLI" "$BIN_DIR/omarchy-webwall"
chmod +x "$CLI" 2>/dev/null || true

# --- 2. restore the live wallpaper on login -------------------------------
if [[ -f "$AUTOSTART" ]] && ! grep -q 'omarchy-webwall launch' "$AUTOSTART"; then
  printf '\n-- Restore any live omarwalls wallpaper on login (hapatapa.omarwalls)\no.launch_on_start("omarchy-webwall launch")\n' >> "$AUTOSTART"
fi

# --- 3. menu > Style row --------------------------------------------------
MENU_ENTRY='  "style.hapatapa-omarwalls": {"icon":"󰨇","label":"Omarwalls","keywords":"live wallpaper animated shader glsl web html video scene tunnel omarwalls hapatapa","when":"test -x \"$(command -v omarchy-webwall)\"","action":"omarchy-webwall menu"},'

mkdir -p "$(dirname "$MENU_EXT")"
[[ -s "$MENU_EXT" ]] || printf '{\n}\n' > "$MENU_EXT"

# Drop the previous block and the old one-line id, then insert a fresh block.
sed -i "/omarchy-webwall-menu-start/,/omarchy-webwall-menu-end/d" "$MENU_EXT"
sed -i '/"style\.hapatapa-webwall"[[:space:]]*:/d' "$MENU_EXT"

if MENU_ENTRY="$MENU_ENTRY" MARK_START="$MARK_START" MARK_END="$MARK_END" awk '
     /^}/ && !done {
       print ENVIRON["MARK_START"];
       print ENVIRON["MENU_ENTRY"];
       print ENVIRON["MARK_END"];
       done = 1
     }
     { print }
   ' "$MENU_EXT" > "$MENU_EXT.tmp"; then
  mv "$MENU_EXT.tmp" "$MENU_EXT"
else
  rm -f "$MENU_EXT.tmp"
  echo "omarwalls: could not add the menu row; add it manually to $MENU_EXT:" >&2
  printf '%s\n' "$MENU_ENTRY" >&2
  exit 1
fi

echo "omarwalls: CLI linked, autostart and menu row ready"
