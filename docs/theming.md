# Theming oshot

oshot reads its colors and sizes from a TOML theme file, `theme.toml` in the config folder unless the `theme-file`
option in `config.toml` names another one. `oshot --gen-theme` writes a commented starting point there, and
Preferences > Theme edits the same file.

oshot watches the theme file and the files it includes, and applies changes while an overlay is open, so a palette
generator or a text editor can restyle it live. A file that doesn't parse leaves the current theme in place, and the
error goes to the log (Tools > View Logs).

## Layout

```toml
[theme]
base = "dark"                         # or "light", the built-in theme everything else applies on top of
include = ["~/.cache/wal/oshot.toml"] # optional, see below
smooth-animations = true

[theme.style]
window-rounding = 4.0                 # pixels, 0 = sharp corners
frame-rounding  = 4.0
grab-rounding   = 1.5
tab-rounding    = 2.0
window-border   = 1.0                 # border width in pixels, 0 = none
frame-border    = 0.0

[theme.palette]                       # named colors for the tables below
accent = "#5274F0"
bg     = "#0D1015"

[theme.colors]                        # Dear ImGui's colors
WindowBg = "$bg"
Button   = "$accent"
ButtonHovered = "$accent/80"

[theme.oshot-colors]                  # oshot's own colors
selection-border = "$accent"
```

Every key is optional. Anything left out comes from the includes, then from the base theme.

## Color values

- `"#RRGGBB"` or `"#RRGGBBAA"`
- `"$name"`, a color from `[theme.palette]`
- `"$name/50"`, the same color at 50% opacity (0 to 100)

Palette names use letters, digits, `-` and `_`. Palette entries themselves are always hex.

A misspelled color name or a value that isn't a color is logged and skipped, and that color falls back to the next
layer.

## `[theme.colors]`

These are Dear ImGui's style colors, by the names `ImGui::GetStyleColorName()` gives them: `Text`, `WindowBg`,
`Button`, `FrameBgHovered` and so on. Preferences > Theme lists them all with "All ImGui colors".

## `[theme.oshot-colors]`

| Key | Used for |
| --- | --- |
| `selection-border` | The selection rectangle |
| `selection-handle`, `selection-handle-hovered` | Its resize handles |
| `selection-dim` | The screenshot outside the selection |
| `size-label-bg`, `size-label-border`, `size-label-text` | The size label next to the selection |
| `hint-select`, `hint-full`, `hint-select-all` | The three hint lines before a selection |
| `tool-selected` | The selected annotation tool's button |
| `text-muted` | Notes and secondary text, trace logs |
| `info`, `success`, `warning`, `error`, `notice` | Messages, log levels and plugin status icons |
| `log-debug` | Debug lines in the Logs window |
| `confidence-low`, `confidence-mid`, `confidence-high` | The OCR confidence bar |
| `tag`, `tag-neutral`, `tag-success`, `tag-warning`, `tag-error` | Labels such as licenses and plugin states, drawn with white text |
| `danger`, `danger-hovered`, `danger-active` | Buttons that delete something |

The overlay keys draw over the screenshot, so their defaults are the same in both base themes.

## Includes

`include` takes a path or a list of paths to other theme files. Relative paths start from the theme file's folder,
and `~` and `$VARS` are expanded. From an included file oshot reads only `[theme.palette]`, `[theme.colors]` and
`[theme.oshot-colors]`. Later includes override earlier ones, and the theme file's own entries override all of them.

This keeps generated colors apart from the settings you write by hand: a palette generator rewrites its file, and your
`theme.toml`, which Preferences may also rewrite, stays yours. Preferences > Theme shows included colors marked
"(include)", and saving from there never copies them into the theme file.

### pywal

`~/.config/wal/templates/oshot.toml`, which wal fills into `~/.cache/wal/oshot.toml`:

```toml
[theme.palette]
bg     = "{background}"
fg     = "{foreground}"
accent = "{color4}"
alt    = "{color2}"
muted  = "{color8}"

[theme.colors]
WindowBg = "$bg"
ChildBg = "$bg"
PopupBg = "$bg"
MenuBarBg = "$bg"
Text = "$fg"
TextDisabled = "$muted"
Border = "$muted/60"
FrameBg = "$muted/35"
FrameBgHovered = "$muted/55"
FrameBgActive = "$muted/75"
TitleBg = "$bg"
TitleBgActive = "$accent"
Button = "$accent/80"
ButtonHovered = "$accent"
ButtonActive = "$alt"
Header = "$muted/45"
HeaderHovered = "$accent/45"
HeaderActive = "$accent/65"
Tab = "$bg"
TabHovered = "$accent/60"
TabSelected = "$accent"
CheckMark = "$accent"
SliderGrab = "$accent"
SliderGrabActive = "$alt"
PlotHistogram = "$accent"

[theme.oshot-colors]
selection-border = "$accent"
tool-selected = "$alt"
```

Then in `theme.toml`:

```toml
[theme]
include = "~/.cache/wal/oshot.toml"
```

### matugen

The same template works with matugen's placeholders, for example `bg = "{{colors.surface.default.hex}}"`,
`fg = "{{colors.on_surface.default.hex}}"` and `accent = "{{colors.primary.default.hex}}"`, with a
`[templates.oshot]` entry in matugen's config pointing `output_path` at the file `theme.toml` includes. For a light
scheme, also set `base = "light"` in `theme.toml`.
