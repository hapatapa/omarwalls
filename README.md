# Omarwalls

Live wallpapers for [Omarchy](https://omarchy.org) — animated GLSL shaders, HTML
pages and looping videos on the `wlr-layer-shell` Background layer, theme-aware
straight from Omarchy's native `colors.toml` (no theme hooks, no separate
theme state).

| Kind | Renderer | Notes |
|---|---|---|
| `shader` | `shader-host` (pure Vulkan, no Qt) | Shadertoy-style GLSL; theme arrives as a `u_theme[16]` uniform array |
| `web` | `webwall-host` (QtWebEngine) | Full-page HTML5/canvas; theme is injected as `window.webwallTheme` |
| `video` | `webwall-host` (QtMultimedia) | Loops muted, aspect-cropped to the screen |

The wallpaper runs in its own process (a QtWebEngine inside the shell would
FATAL it), supervised by a small shell service that reacts to state changes.
A picker overlay in the Omarchy menu style — type-to-filter, arrow keys,
Enter — is summoned from the shell or the CLI.

## Install

```bash
omarchy plugin add https://github.com/hapatapa/omarwalls.git --enable --yes
# the static built-in background would fight the live wallpaper for the layer:
omarchy plugin disable omarchy.background
# optional: put the CLI on PATH (~/.local/bin is already in it on Omarchy)
ln -s ~/.config/omarchy/plugins/hapatapa.omarwalls/bin/omarchy-webwall ~/.local/bin/omarchy-webwall
# install the bundled sample wallpapers into your store
omarchy-webwall install-samples
```

The renderer hosts compile themselves on first launch (see
[Build](#build)). Log out and back in — or `omarchy restart shell` — so the
service starts; it restores the saved wallpaper whenever the shell starts.

The picker is an overlay plugin: bind it like any other, e.g. in
`~/.config/hypr/bindings.lua`:

```lua
o.bind("SUPER + PERIOD", nil, "omarchy-shell shell toggle hapatapa.omarwalls")
```

or just run `omarchy-webwall menu`.

## Usage

```
omarchy-webwall launch            (Re)launch the saved wallpaper — autostart
omarchy-webwall stop               Stop the live wallpaper
omarchy-webwall menu               Open the thumbnail picker overlay
omarchy-webwall set <id>           Set the wallpaper by store id and relaunch
omarchy-webwall current            Show the active wallpaper (id + kind + title)
omarchy-webwall list               List wallpapers in the store
omarchy-webwall install-samples    Install bundled sample wallpapers
omarchy-webwall import <dir>       Import a wallpaper directory into the store
```

## Wallpaper store

Wallpapers live in `~/.local/share/omarchy-webwall/wallpapers/<id>/` as a
directory with a `wallpaper.json` manifest plus its assets. The active
wallpaper is recorded in `~/.local/state/omarchy-webwall/current` (the
manifest path; empty = stopped).

```json
{
  "id": "aurora",
  "title": "Aurora",
  "kind": "shader",
  "entry": "aurora.frag",
  "preview": "preview.png",
  "author": "hapatapa",
  "description": "Animated northern-lights curtain"
}
```

`kind` + `entry` are what the hosts read; `preview` shows in the picker.
`omarchy-webwall import <dir>` copies a prepared directory into the store.

### Shader wallpapers

`entry` is a GLSL fragment shader with Shadertoy conventions — provide
`void mainImage(out vec4 fragColor, in vec2 fragCoord)` and use
`iResolution`, `iTime`, `iFrame`, `iMouse`, `iDate`. The Omarchy theme is
available as a `uniform vec4 u_theme[16]`:

| Slot | Meaning | Fallback source |
|---|---|---|
| `u_theme[0]` | background | `color0` |
| `u_theme[1]` | foreground | `color1` |
| `u_theme[2]` | accent | `color2` |
| `u_theme[3]` | muted | `muted` |
| `u_theme[4]` | cursor | `cursor` |
| `u_theme[5]` | selection | `selection` |
| `u_theme[0..15]` | terminal palette | `color0..color15` (override the slots above) |

i.e. the semantic slots are filled first, then `color0..color15` override
slots 0–15, so the full terminal palette is exposed to shaders.

### Web wallpapers

`entry` is an HTML page. At document creation the host injects:

```js
window.webwallTheme = {
  bg: "#302270", fg: "#86f3f5", accent: "#898efa",
  muted: "#aa8acf", cursor: "#…", selection: "#6a55d6",
  ansi0: "#…", /* … ansi15 */ _theme: true
};
```

mapped from the same `colors.toml`. `window.innerWidth/innerHeight` are
reported multiplied by 10 so pages can render supersampled (the bundled
demo scales its canvas back down). The page is reloaded automatically when
the theme changes.

### Video wallpapers

`entry` is any QtMultimedia-playable file (mp4/webm/…); it loops, muted,
cropped to fill.

## Theme

Both renderers read `~/.local/state/omarchy/current/theme/colors.toml`
(Omarchy's native theme source, so `omarchy-theme-set` just works) and
live-reload on change — no hooks, no duplicated theme state.

## Build

`renderer/build.sh` builds both hosts; the CLI runs it automatically when the
binaries are missing. Dependencies (Arch):

```bash
sudo pacman -S --needed base-devel qt6-base qt6-declarative qt6-webengine \
  qt6-multimedia wayland shaderc vulkan-icd-loader
```

(Vulkan headers are vendored in `renderer/vulkan/`.) Only the shader host is
needed for shader-only setups; Qt is needed for web/video. Pass `rebuild` to
`build.sh` to force a rebuild.

## Samples

`omarchy-webwall install-samples` copies the bundled wallpapers into your
store: `aurora`, `plasma`, `tunnel` (shaders), `web-demo` (theme web demo),
`drift-video` (video).

## License

[MIT](LICENSE)
