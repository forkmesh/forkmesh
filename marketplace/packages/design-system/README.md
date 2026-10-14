# Design System

ForkMesh's design specs for every surface, at `/design` on the website
(forkmesh.com/design). Free, bundled and installed by default; remove it from
the Market like any other package.

## What the page shows

Every value is read from the code that draws it when the website is built, so
the page follows the apps without anyone copying numbers by hand.

| Section | Read from |
| --- | --- |
| Web: colour tokens (dark and light), fonts, other tokens, every button class drawn live, Lucide icons | `www/public/styles.css`; `data-lucide` names in `app/public`, `www/public` and package web pages; package navigation icons |
| Desktop: application palette, named colours, sender colours, interface font order, every `QPushButton` style sheet rule in both themes with its states, every icon `icons.qrc` compiles | `desktop/src/Theme.h`, `desktop/src/main.cpp`, `desktop/resources/icons.qrc` |
| iPhone: colours paired light/dark, accent choices, text roles, spacing, radius, motion, button components, Material icons, SF Symbols (names only) | `native/ios/ForkMesh/Theme/`, every `.swift` file for icon and button names |
| Android: the same tokens plus Material shapes, button components, Material icons | `native/android/.../ui/theme/ForkMeshTheme.kt`, every `.kt` file for icon and button names |
| All icons | every set above in one searchable grid; clicking an icon copies its name |

Desktop QSS gradients (`qlineargradient`) are converted to CSS
`linear-gradient`; `palette(...)` and `url(...)` values are left out. Phone
buttons are drawn from the token values, not rendered by the apps. SF Symbols
cannot be drawn outside Apple devices, so they are listed by name.

Clicking a colour swatch or an icon copies its name and says **Copied** inside
that swatch or icon, in the muted colour; a failed copy says so there in the
error colour. The line stays until another swatch or icon is copied; there
are no toasts.

## Where the button lives

- Desktop: a **Design** button on the rail (the `palette` icon). It opens
  `<active host>/design` in the browser; the app host answers `/design` with a
  308 to forkmesh.com. Right-click offers the package settings and removal.
- iPhone and Android: **Design** in the App group of the menu, opening the
  active project's homepage plus `/design`. The Market page of the package has
  an **Installed on this device** switch; turning it off hides the button on
  that phone.
- Web: the page itself. A site built from a catalog without this package has
  no `/design` page and no redirect for it.

## Files

- `content/www/design.html`, `design.css`, `design.js`: the page.
- `content/www/inventory.py`: `build(repository, destination)` writes
  `inventory.json` and copies the desktop icons and the Material Icons font
  next to the page.
- `content/desktop/DesignSystemLink.cpp`: the rail link registration.
- `tests/test_design_system_inventory.py`: the inventory against the real
  sources and the page against the website's page conventions.

See `docs/marketplace/README.md` for the `www.page` socket and desktop rail
links.
