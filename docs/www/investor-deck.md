# Investor deck

The deck a CRM contact opens (`/pitch-deck/<token>`, served by
`app/src/pitch_deck_runtime.py`) uses the homepage's look, because it goes out
with every investor email. Part of [the website docs](README.md); its head, favicons and CSP
manifest rule: [templates](templates.md#logos-and-favicons); what it reports
to analytics: [analytics](../app/analytics.md#investor-deck).

## Look and slides

- **Theme.** Homepage tokens: white page, ink `#101012`, blue accent, ABC
  Favorit headlines. Slides 3, 5, 7, 9 and 13 sit in the homepage's dark
  rounded panel. Light by default; a choice saved under `forkmesh.theme` or
  `forkmesh.dashboard.theme` wins, and the moon/sun button writes both keys.
- **Assets.** The cover and closing slide draw the hero schematic (copied from
  `www/public/index.html`, with moving pulses). Images and the font load from
  `/assets/...` on the deck's own origin: `PAGE_CSP` keeps `img-src 'self'`
  and `font-src 'self'`, so the deck must be opened on forkmesh.com (the CRM's
  `DECK_SITE_ORIGIN`). On app.forkmesh.com the screenshots and font are missing.
- **Product visuals.** The cover shows the desktop app on a laptop and the
  mobile app on a phone. On slide 4 a phone beside the six workflow cards
  cycles through the matching mobile screens; the cards stay still (user
  request). Slide 5 fans three phone screens. Numbers count up, use-of-funds
  bars fill and the ARR ladder steps in when their slide scrolls into view;
  `prefers-reduced-motion` and print show the final state.
- **Beyond the desk (slide 7).** The watch app and AR/XR glasses: the
  workspace (desktop app and a Claude Code session opening a pull request)
  appears only inside the lenses of aviator AR glasses, with a blurred glow
  behind and a slight pointer parallax; the watch copies the watchOS agent
  list. Everything is inline SVG and CSS, because `PAGE_CSP` allows no
  external scripts (no three.js). The glasses sit on a fixed 600 by 240 stage
  that a small script scales (`--fit` sets the width share). Labels: watch
  "In build", glasses "R&D"; the footer names the pictured hardware. Watch
  classes are `.strap` and `.wrow.on` because `.band` and `.live` are taken.
- **Page numbers.** Slides 8 to 14 are Customer & go-to-market through
  Sources; `DECK_PAGE_TITLES` in the CRM worker follows. Page views are stored
  by number, so time recorded before slide 7 was inserted (20 Oct 2026) shows
  one title too early in the CRM's per-page table.
- **Page tracking.** The current page is the slide crossing the middle of the
  viewport, because a phone slide is taller than the screen and a
  "55% visible" rule never changed page.
