# Mobile page (`/mobile`)

Part of [the website docs](README.md). Code: `www/public/mobile.html`,
`www/public/mobile.css`, screenshots in `www/public/assets/mobile/`. Tests:
`www/tests/test_mobile_page.py` (page, assets, Worker configs) and
`app/tests/test_mobile_routes.py` (App routing).

The page sells the iPhone app (with iPad and the Android APK mentioned
alongside). It follows the landing page, which is the design source
of truth for www: the same pill navigation, circuit-schematic hero, notched
dark slabs, card language and textured buttons.

## Why it is shaped this way

- **Layout "A" of two prototypes.** The owner picked the showcase layout
  (alternating feature rows with two or three phones each, then a grid of
  smaller features) over a sticky scroll-driven tour. It reads like the
  landing page and stays static, so it needs no page script.
- **Big features get a row, small ones get a card or a line.** Agents, Code &
  Git, Pull requests and Chat & Inbox are rows because each has several real
  screenshots. On-device AI (Gemma 4 through MLX) gets its own light section
  because it is the most surprising feature. iPhone and Apple Watch
  integrations (Live Activities, widgets, Control Center, Siri, Spotlight,
  share sheet, AutoFill, iPad) have no screenshots, so they are drawn as small
  HTML components in a second slab. "And that's not all." shows eight smaller
  features as cards with one phone each: the command palette, the prompt
  composer and the avatar hub, then Tasks, Calendar, the calorie counter,
  Organizations and themes (the last two as a wide pair). Everything else from
  the feature list is in the "Everything else in the app" index, which leaves
  out what already has a card.
- **Homepage chrome is copied, not shared.** The landing page keeps its CSS
  inline, so `mobile.css` repeats the tokens, hero, button recipe and slab
  rules it needs under the same names, and the page links the shared
  `home-nav.css` (a verbatim lift of the homepage navigation). The nav markup
  and the hero schematic are copies of the homepage's; homepage-only anchors
  (`#pricing`, `#membership`, `#faq`) point at `/#...` instead.
- **No green kickers.** The landing page dropped its green section kickers, so
  rows are labelled with a neutral icon tile and text instead.
- **The download buttons are real.** The App Store button opens the listing
  `https://apps.apple.com/app/forkmesh/id6803060735`, and the page carries the
  `apple-itunes-app` smart banner meta tag with the same id. The Android button
  goes to the verified downloads on the landing page (`/#downloads`).

## Screenshots

- Source: iPhone 17 Pro simulator captures of the native app (1206 × 2622,
  build 1.4.0+17), exported to 640-pixel-wide WebP at quality 82 (about 40 KiB
  each). The phone frame clips the corners, and each screenshot already shows
  its own status bar and Dynamic Island, so the frame draws neither.
- `chat.webp`: two private channels in the "Office" group were named after
  people; they read `#design` and `#launch` in the published image. The
  replacement text was rendered with SF Pro at the capture's size and colour.
- `history.webp` shows a toolbar that the floating tab bar overlaps in the
  bottom right corner. It is used only as the left-hand phone of the Code row,
  where the middle phone covers that corner.
- `tasks.webp`, `calendar.webp`, `calories.webp`, `organizations.webp` and
  `themes.webp` came from a debug build, which draws the telemetry strip and a
  latency pill under the top bar. Each image is spliced: the status bar and top
  bar of `01-code-repos.png` (9:41, full signal) above the page content from the
  strip's divider down, with the pill's overhang painted over. Tasks, calories
  and themes are the owner's window captures of the simulator; calendar and
  organizations are native simulator captures. The debug build's bug-report
  button is still in each image, about 70% of the way down, where the card's
  crop hides it. `themes.webp` has only the bottom half of the settings screen,
  so its card is bottom-aligned; the space above "General" is plain background.
- `og.png` (1200 × 630) is the social card: the hero headline and three
  phones on the dark schematic.

## Routing

`/mobile` is a static asset served from `mobile.html`. Like the other
marketing pages it is listed in `run_worker_first` in `www/wrangler.toml`,
`www/wrangler.production.toml`, `app/wrangler.toml`, `world/wrangler.toml` and
the route list in `tools/cloudflare_bootstrap.py`, so attribution observes it.
On the core nodes, `app/src/static_routes.py` sends `/mobile` to the marketing
site (`WWW_SITE_ROUTES`), reserves the prefix (`RESERVED_ROUTE_PREFIXES`) and
hides the implementation path `/mobile.html` (`BLOCKED_STATIC_HTML_PATHS`).
The site footer links it under Explore.

## Traps

- The live App Store listing can lag the screenshots. When the page shipped,
  the store served 1.1.0 while the screenshots and the feature list describe
  1.4.0; check the store before relying on a feature in the copy.
- Push notifications reach iOS only; Android has no push transport.
- The homepage (`index.html`), the simple header (`site-header.js`) and the
  hackathon page carry their own copies of the Product menu. They do not link
  `/mobile` yet.
- Security copy must match what the phone actually does, and
  `test_mobile_page.py` checks it. Reviews are signed by the phone's device
  key. Desktop merges and agent approvals are operations on the end-to-end
  encrypted control channel (LAN or relayed), which the desktop checks and
  confirms; the phone does not sign them
  ([desktop agents](../native/DESKTOP_AGENTS.md)). Face ID is asked for only
  on specific actions: approving an agent's request, confirming a payment,
  deleting a desktop secret, and some others. Deleting an agent from the phone
  asks for an explicit confirmation, not Face ID. Do not say that everything
  destructive asks for Face ID.

## Recent changes

- 2026-09-30: the availability lines (hero note, closing call to action, meta
  description and the structured data's `operatingSystem`) no longer promise
  Apple Watch. The App Store still serves the Flutter 1.1.0 build (iPhone and
  iPad), 1.4.0 (17) shipped phone-only with no Watch app, and the v1.2.5 APK
  on `/#downloads` is the Flutter build.
  `test_availability_lines_promise_only_what_ships` keeps watchOS out of them
  until a Watch build is live. The
  Apple Watch feature card and "Agent Watch" line still describe the native
  app and should be revisited with the store listing.
- 2026-09-28: the "Its own signing key" card no longer says merges and
  approvals are signed on the phone. The "Inside" intro no longer says there is
  "no host in the middle", because desktop control can be relayed. The Face ID
  card now names the actions that ask for Face ID instead of promising it for
  anything destructive.
