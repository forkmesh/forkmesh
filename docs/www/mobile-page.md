# Mobile page (`/mobile`)

Part of [the website docs](README.md). Code: `www/public/mobile.html`,
`www/public/mobile.css`, screenshots in `www/public/assets/mobile/`. Tests:
`www/tests/test_mobile_page.py` (page, assets, Worker configs) and
`app/tests/test_mobile_routes.py` (App routing).

The page presents the iPhone app (with iPad and the Android APK alongside).
It follows the landing page's design: pill navigation, schematic hero, dark
slabs, card language and textured buttons.

## Structure

- Alternating feature rows with two or three phone screenshots each, a light
  section for on-device AI, a slab of small HTML components for platform
  integrations that have no screenshots, a grid of smaller feature cards, and
  an index of everything else. The page is static and needs no script.
- **Homepage chrome is copied, not shared.** `mobile.css` repeats the tokens,
  hero, button recipe and slab rules it needs under the same names, and the
  page links the shared `home-nav.css`. Homepage-only anchors point at
  `/#...`.
- **Download buttons.** The App Store button opens the app listing, and the
  page carries the `apple-itunes-app` smart banner meta tag with the same id.
  The Android button goes to the verified downloads on the landing page
  (`/#downloads`).

## Screenshots

Simulator captures of the native app exported to 640-pixel-wide WebP at
quality 82. Each shows its own status bar, so the phone frame draws none.
Screenshots must not show personal data. `og.png` (1200 × 630) is the social
card.

## Routing

`/mobile` is a static asset served from `mobile.html`. It is listed in
`run_worker_first` in `www/wrangler.toml`, `www/wrangler.production.toml`,
`app/wrangler.toml`, `world/wrangler.toml` and the route list in
`tools/cloudflare_bootstrap.py`, so attribution observes it. On the core
nodes, `app/src/static_routes.py` sends `/mobile` to the marketing site
(`WWW_SITE_ROUTES`), reserves the prefix (`RESERVED_ROUTE_PREFIXES`) and
hides `/mobile.html` (`BLOCKED_STATIC_HTML_PATHS`). The site footer links it,
and the Product menu (landing page, hackathon page, this page, and the shared
`site-header.js` menu) and the phone-size navigation drawers list "Mobile
app" after "Desktop app".

## Traps

- The live store listing can lag the screenshots; check it before relying on
  a feature in the copy. Availability lines promise only platforms that ship
  (`test_availability_lines_promise_only_what_ships`).
- Push notifications reach iOS only.
- Security copy must match what the phone does, and `test_mobile_page.py`
  checks it. Reviews are signed by the phone's device key. Desktop merges and
  agent approvals are operations on the end-to-end encrypted control channel
  (LAN or relayed), which the desktop checks and confirms; the phone does not
  sign them ([desktop agents](../native/DESKTOP_AGENTS.md)). Face ID is asked
  for only on specific actions (approving an agent's request, confirming a
  payment, deleting a desktop secret, and some others); do not say that
  everything destructive asks for Face ID.
