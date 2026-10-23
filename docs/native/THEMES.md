# Phone themes

The iOS and Android apps let the user pick and edit a colour theme, the way
the start page's theme picker does for a new instance
([../INSTANCES.md](../INSTANCES.md)). The choice is the phone's own: it is
stored in the app's preferences, never in a project scope, and is not synced.

## What a theme is

A theme is five choices, resolved separately for light and dark mode:

| Part | Stored in | Default |
|---|---|---|
| Surface family | `appearance/surface` (name) | `forkmesh` (no key) |
| Accent | `appearance/accent` (preset name, or `custom`) | `blue` |
| Custom accent | `appearance/accentCustom` (`#rrggbb`, only with `custom`) | none |
| Keep colours readable | `appearance/readableColors` (bool) | on (no key) |
| Edited colours | `appearance/themeColors` (JSON) | none (no key) |

Light/dark/system stays in `appearance/themeMode`. Picking a preset accent
removes `appearance/accentCustom`; a build that does not know `custom` falls
back to blue.

`appearance/themeColors` is `{"dark": {...}, "light": {...}}` mapping token
names to `#rrggbb`. Unknown modes, unknown tokens and bad values are dropped
on read. The 13 tokens, in their editor groups:

| Group | Tokens |
|---|---|
| Surfaces | `bgBase` Background, `bgRaised` Cards and bars, `bgOverlay` Sheets and fields, `bgElevated` Raised controls, `border` Borders |
| Text | `textPrimary` Text, `textSecondary` Secondary text, `textTertiary` Hints and captions |
| Accent | `accent` Accent, `onAccent` Text on accent |
| Status | `success`, `danger`, `warning` |

Unedited, `textSecondary` and `textTertiary` are `textPrimary` at 72% and 60%
(light) or 70% and 50% (dark), and `textDisabled` at 25% or 30%, as before.
An edited one is opaque. Choosing a surface family drops the surface and text
edits; choosing an accent drops the accent edits; Shuffle and Reset drop all
of them.

## Surface families

`bgBase`, `bgRaised`, `bgOverlay`, `bgElevated`, `border`, `textPrimary`:

| Family | Light | Dark |
|---|---|---|
| ForkMesh | `#f7f7f7 #ffffff #f0f0f0 #e8e8e8 #f0f0f0 #0a0a0a` | `#0b0b0c #141414 #202022 #26262a #252529 #f4f4f5` |
| Slate | `#f6f8fa #ffffff #eff2f5 #e6eaef #e1e4e8 #1f2328` | `#010409 #0d1117 #161b22 #21262d #30363d #f0f6fc` |
| Zinc | `#fafafa #ffffff #f4f4f5 #e4e4e7 #e4e4e7 #18181b` | `#09090b #18181b #27272a #3f3f46 #27272a #f4f4f5` |
| Stone | `#fafaf9 #ffffff #f5f5f4 #e7e5e4 #e7e5e4 #1c1917` | `#0c0a09 #1c1917 #292524 #44403c #292524 #f5f5f4` |
| Pure | `#ffffff #ffffff #f2f2f2 #e8e8e8 #e5e5e5 #000000` | `#000000 #0a0a0a #161616 #1f1f1f #1f1f1f #ffffff` |

ForkMesh is the palette the apps had before themes. Slate, Zinc and Stone
follow the start page's families; Pure is true black for OLED screens.

## Accents and contrast

Until October 2026 the default blue was `#007AFF` in light mode, which is
3.5:1 on `bgOverlay` and 3.8:1 on `bgBase`, under WCAG AA (4.5:1) for the
small accent text the apps use (links, "Change image", counts). Teal, red,
pink and purple were also under 4.5:1 on a light surface, and dark purple
`#6C63FF` was 3.8:1 on the dark overlay. The presets are now:

| Accent | Light | Dark |
|---|---|---|
| Blue | `#0066cc` | `#409cff` |
| Cyan | `#0e7490` | `#22d3ee` |
| Teal | `#0f766e` | `#2dd4bf` |
| Green | `#147c3b` | `#4ade80` |
| Lime | `#4b780f` | `#a3e635` |
| Amber | `#af5109` | `#fbbf24` |
| Orange | `#c0400c` | `#fb923c` |
| Red | `#d22222` | `#f87171` |
| Pink | `#cb226d` | `#f472b6` |
| Violet | `#9333ea` | `#c084fc` |
| Purple | `#5856d6` | `#8b85ff` |
| Graphite | `#52525b` | `#a1a1aa` |

Every preset clears 4.5:1 on the ForkMesh `bgBase`, `bgRaised` and
`bgOverlay` as it is, so with the default surfaces nothing is adjusted.

**Keep colours readable** (on by default) makes that a rule for every theme:
`accent`, `textPrimary`, `success`, `danger` and `warning` are moved to at
least 4.5:1 against `bgBase`, `bgRaised` and `bgOverlay` of the resolved
theme. The colour keeps its hue and saturation; only its HSL lightness moves,
darker when `bgBase` is light (relative luminance above 0.18) and lighter
when it is dark, by a 24-step binary search for the closest lightness that
passes. `onAccent` is the near-black `bgBase` in dark mode and white in light
mode (or the edited value) while that clears 4.5:1 on the accent; otherwise
whichever of white and `#0a0a0a` contrasts more. With the switch off, custom
and edited colours are drawn exactly as picked.

`accentSubtle` (the pressed-row and selection wash) is the accent at 10% in
light mode; in dark mode it is the preset's deeper shade at 20%, or for a
custom or edited accent the accent mixed 45% toward black at 20%.

The custom accent is one colour for both modes; the rule above derives its
light and dark variants. Shuffle picks a random surface family and a custom
accent at a random hue (HSL 75% saturation, 50% lightness) with the switch on.

## Matching the instance (iOS)

**Match the instance's theme** (`appearance/matchInstanceTheme`, off by
default) makes the app wear the colours the instance's admin picked. While it
is on, the app reads `themeColors` from `GET <instance>/api/instance` (or
`theme.dark`/`theme.light` from a server that predates it) on launch, on
returning to the foreground and on a project switch, maps the dashboard
tokens onto the 13 app tokens and keeps them in
`appearance/instanceThemeColors` (the `appearance/themeColors` shape):
`background`→`bgBase`, `card`→`bgRaised`, `popover` (else `muted`)→`bgOverlay`,
`muted` (else `secondary`)→`bgElevated`, `border`, `foreground`→`textPrimary`,
`muted-foreground` mixed 35% toward `foreground` (else
`secondary-foreground`)→`textSecondary`, `muted-foreground`→`textTertiary`,
`primary`→`accent`, `primary-foreground`→`onAccent`, `success`,
`destructive`→`danger`, `attention`→`warning`. Missing tokens fall back to the
ForkMesh look; the readable switch still applies. An instance with the default
look, or a project without an instance, clears them and the phone's own theme
shows; a failed fetch keeps the last colours. Turning the switch off drops
them. `AppTheme.instance(themeColors:)` and `SettingsService.effectiveTheme`
(`ForkMeshCore/Sources/FMAccount/InstanceTheme.swift`) hold the rule; the
fetch runs from `ThemedAppView` in `App/ForkMeshApp.swift`.

## Where it lives

| | iOS | Android |
|---|---|---|
| Model, contrast, storage | `ForkMeshCore/Sources/FMAccount/AppTheme.swift`, `SettingsService.theme` | `core/.../settings/AppTheme.kt`, `Appearance.kt` |
| Palette | `FmPalette(theme:)` in `Theme/FmColors.swift`, installed by `ForkMeshTheme` | `fmPaletteFor` in `ui/theme/ForkMeshTheme.kt` |
| Settings card | `AppearanceCard` (`Features/Account/SettingsCards.swift`) | `AppearanceCard.kt` |
| Theme editor | `ThemeEditorScreen.swift` | `ThemeEditorScreen.kt` |
| Tests | `FMAccountTests/AppThemeTests.swift` | `core/src/test/.../settings/AppThemeTest.kt` |

The two cores hold the same tables and the same resolution, and their tests
check the same values.

## The screens

Settings, General, Appearance card: the System/Light/Dark track, the twelve
accent swatches plus **Custom** (the system colour picker), the surface
family chips, a **Theme editor** row, and the background image (iOS).

The theme editor, a pushed page:

- **Preview.** A mock screen (bar, list rows, a link, status chips, a switch
  and a filled button) drawn with the theme being edited, for the mode
  chosen in its Light/Dark switch (the app's current mode at first). The
  whole app also changes as soon as anything is picked.
- **Accent** and **Surfaces**, as on the card.
- **Keep colours readable**, **Shuffle**.
- **Every colour**: the 13 tokens grouped as above for the previewed mode,
  each with its hex value and a colour picker; an edited token shows a reset
  button that drops just that edit.
- **Reset theme** puts the ForkMesh surfaces, the blue accent and the
  readable switch back and drops every edit; it does not touch light/dark or
  the background image.
