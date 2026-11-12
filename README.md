# Omarwalls

Live wallpapers for [Omarchy](https://omarchy.org) — animated GLSL shaders, HTML
pages and looping videos on the `wlr-layer-shell` Background layer, theme-aware
straight from Omarchy's native `colors.toml` (no theme hooks, no separate
theme state).

| Kind | Renderer | Notes |
|---|---|---|
| `shader` | `shader-host` (pure Vulkan, no Qt) | Shadertoy-style GLSL; theme arrives as a `u_theme[24]` uniform array |
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
```

The service self-installs the rest on first load: it links the CLI into
`~/.local/bin`, adds the login autostart, and writes the
**menu > Style > Omarwalls** row. Then install the bundled samples:

```bash
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
omarchy-webwall scale [<factor>]   Render scale 0.1–1.0 (or `off`); lowers GPU load
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

### Render scale

Heavy fragment shaders (raymarchers such as `tunnel-theme`) can render below
the screen resolution and let the compositor upscale the result, which cuts
GPU work roughly with the square of the factor:

```json
"renderScale": 0.75
```

`renderScale` (0.1–1.0) is a per-wallpaper default. The global override in
`~/.local/state/omarchy-webwall/render-scale` wins when that file exists, and
is what the CLI writes:

```bash
omarchy-webwall scale 0.5     # render at half resolution
omarchy-webwall scale off     # back to the wallpaper's own value
omarchy-webwall scale         # show the effective value
```

The host restarts to apply it. `iResolution` reports the render size (that is
what shaders key their work off), while the image still fills the screen —
`wp_viewporter` stretches the smaller buffer to the layer surface. Only shader
wallpapers use this; web/video already draw at native size.

### Shader wallpapers

`entry` is a GLSL fragment shader. Shadertoy conventions are supported —
provide `void mainImage(out vec4 fragColor, in vec2 fragCoord)` and use
`iResolution`, `iTime`, `iFrame`, `iMouse`, `iDate` — and so are
self-contained shaders that define `void main()` and write `fragColor`
directly. The active Omarchy theme is available as `uniform vec4 u_theme[24]`:

| Slot | Meaning |
|---|---|
| `u_theme[0..5]` | seeded with `background`, `foreground`, `accent`, `muted`, `cursor`, `selection`, then overridden by `color0..color5` |
| `u_theme[6..15]` | `color6..color15` of the terminal palette |
| `u_theme[16]` | real `background` |
| `u_theme[17]` | real `foreground` |
| `u_theme[18]` | real `accent` |
| `u_theme[19]` | real `muted` |
| `u_theme[20]` | real `cursor` (falls back to `accent`) |
| `u_theme[21]` | real `selection` |
| `u_theme[22..23]` | reserved (zero) |

Since most Omarchy themes define the full `color0..color15` palette, slots 0–15
are the terminal palette at runtime. Slots 16–21 are never touched by that
override and always hold the real UI colors, also exposed as the aliases
`u_background`, `u_foreground`, `u_accent`, `u_muted`, `u_cursor` and
`u_selection` — use those when a wallpaper should follow the UI accent rather
than the terminal palette.

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
store: `aurora`, `plasma`, `tunnel`, `tunnel-theme` (shaders), `web-demo`
(theme web demo), `drift-video` (video).

`tunnel` is a port of the raymarched “I like tunnels” scene from the author's
Wallpaper Engine workshop item; `tunnel-theme` runs the same scene with its
two light streams taken from the theme's real `accent` and `cursor` colors
(falling back to the accent), and its background tint from the real
`background` — not from the `color0..15` terminal palette.

## License

[MIT](LICENSE)
