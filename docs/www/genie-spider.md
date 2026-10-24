# The Genie spider on the web

Part of [the website docs](README.md). The same Genie spider as on the phone
([../native/GENIE.md](../native/GENIE.md)) and the desktop
([../desktop/genie.md](../desktop/genie.md)) appears on web pages as a guide.
It has no health, speech or model, and says nothing until it is clicked.

## Where the code is

| Part | File |
| --- | --- |
| Painter, behaviour, bubble, tips, interactions, keep-clear | `www/public/genie-spider.js` (one file, styles injected); tests `www/tests/genie_spider.test.mjs` |
| Website pages | `www/templates/partials/genie-spider.html`, included by `base.html` ([templates.md](templates.md)); `test_every_site_page_loads_the_spider` |
| Docs library pages | `PAGE_TEMPLATE` in `app/tools/docs_site.py` |
| Git compatibility page | the HTML template in `tools/git_conformance.py` |
| Dashboard | the `geniescript` page token in `dashboard/shell.html`, filled by `compose_page_from_reader` in `app/src/dashboard_shell.py`; `test_dashboard_chat_leaves_the_spider_out` |
| Hide and show | `offFrom`, `offCookie`, `isOff`, `hidden`, `hide`, `show`, `shown` on `window.ForkMeshGenie` |
| Settings switch | "Show the Genie spider" (`[data-genie-visible]`) under Appearance; `renderGenieSetting`, `saveGenieSetting`, `watchGenieSetting` in `dashboard/js/02-helpers.js` |
| Footer way back | `mountGenieReturn` in `www/public/site-footer.js`, `.site-footer-genie` in `site-footer.css` |
| Other App pages | a script tag on the auth, start, ledger, leaderboards, network, status, payouts, reviews and note pages |

`genie-spider.js` is in `SHARED_FILES` and `CACHE_BUSTED_ASSETS`, so it gets
a content-hash `?v=`; an App page picks up a change only when the App is
deployed too.

Left out on purpose: the full-screen chat (its composer is part of the
layout), the OAuth consent screen and the pitch deck. A page opts out with
an empty `{% block genie %}` or `data-genie="off"` on `<html>`.

**White-label instances.** The spider is the `genie` instance plugin and is
off on a new instance ([../INSTANCES.md](../INSTANCES.md)). When the page
carries `script[data-instance-brand]`, `instanceAllows()` reads
`/api/instance` once and builds the spider only if the plugin is enabled.

## The spider

- **Painter.** A canvas port of the desktop painter: same colours, legs,
  turban, six eyes, blinking and reactions (wave, cheer, nod, eureka, giggle,
  surprise, dizzy). 78 px canvas (62 px under 520 px wide) at the device
  pixel ratio, capped at 3. All eight feet stand on the floor line.
- **Where it stands.** On the bottom of the viewport or on top of any fixed or
  sticky element anchored there, found with `document.elementsFromPoint`
  along the bottom edge (every 1.5 s idle, 0.3 s moving).
- **Moving.** Every 5 to 13 s it either plays with something on the page or
  walks up to 260 px from home at 70 px/s. Where a throw comes to rest becomes
  home (`forkmesh.genie.spot` in `localStorage`). Its eyes follow the pointer
  within 480 px.
- **Throwing.** Stepped at 120 Hz with gravity and quadratic air resistance,
  capped at 4,200 px/s; bounces off walls and the floor and skids to rest.
- **Start.** The spider is built in idle time after the page's `load` event
  (at most 2 s later), never during the page's first render
  ([../PERF_PAGES.md](../PERF_PAGES.md)).
- **Frames.** `requestAnimationFrame` while it moves, reacts, plays or shows a
  bubble; 12 fps when idle; none while the tab is hidden. The frame step is
  clamped to 0–0.1 s. With `prefers-reduced-motion` it never wanders and
  stops drawing when nothing changes.

## Playing with the page

On half of its idle turns it samples an 8 × 12 grid of viewport points with
`elementsFromPoint`, keeps up to 32 visible candidates, and classifies them
(`ForkMeshGenie.elementKinds`): `heading` (and `short`), `button`, `link` (and
`nav`), `image`, `logo`, `input`, `code` and `card`.
`ForkMeshGenie.drawInteraction` takes the next interaction from a shuffled
bag that fits a kind on the page, walks to the element and plays it.

There are 15 base interactions (look, read, wave, point, thread, sparkle,
outline, hop, cheer, giggle, surprise, nod, count, dizzy, type) and 30 genie
effects (`GENIE_EFFECTS`, merged into `INTERACTIONS` with `genie: true`, listed
by `ForkMeshGenie.genieEffects`; the same names exist on desktop and phone).

Effects draw into an SVG overlay (`.fm-genie-fx`, fixed,
`pointer-events:none`) created when the interaction starts and removed when
it ends. The element itself is never touched: no style, attribute or DOM
change, no click, no focus. Effects that change the spider (`spell`: scale,
spin, turn, fade; `s.magic` glow) are reset by `endPlay`. An interaction ends
early when the element leaves, on any input, a resize or the tab hiding. With
`prefers-reduced-motion` or Stay put only `look` runs.
`ForkMeshGenie.play(name?)` and `ForkMeshGenie.playing()` exist for tests.

## Keeping clear of buttons and logos

The spider's body (`ForkMeshGenie.body`, plus 4 px) must not overlap any
visible link, button, form control, interactive role, focusable or
clickable element, image, svg, video, canvas or logo.

- **Finding them:** `elementsFromPoint` every 20 px along the width at four
  heights, plus matching descendants of small hits; each element's
  `getClientRects`. Elements larger than a quarter of the viewport are
  ignored. Cached for 1 s.
- **Choosing a spot:** `ForkMeshGenie.clearX` (pure) returns the nearest x
  that covers nothing, or the least-covered one.
- **When:** on load, every wander or interaction spot, after a throw, after
  Tuck away, 200 ms after any scroll ends, after a resize, and at most once a
  second while idle.

## Quiet until clicked

- It never opens its bubble by itself.
- A click (or Enter/Space) waves and shows the next line in a small bubble:
  one sentence, closing after 5 s (4 s after the pointer leaves) or on
  Escape. A press that moves more than 6 px is a drag; one held for 0.55 s
  opens the menu.
- The menu: Next tip, Stay put / Wander again (`forkmesh.genie.still`), Tuck
  away (`forkmesh.genie.parked`), Hide for this visit (`forkmesh.genie.away`
  in `sessionStorage`) and Hide for good.

## Hiding it for good

- Hide for good removes the spider and keeps it away on later loads: the
  script loads but builds nothing (no canvas, listener, timer or fetch).
- Stored as `forkmesh.genie.off` = `1` in `localStorage` and a cookie
  `forkmesh_genie=off` (or `on`), one year, `SameSite=Lax`, on
  `Domain=.forkmesh.com` on forkmesh.com and its subdomains (host-only
  elsewhere). The cookie is read first, so the choice holds across origins;
  only without the cookie does the flag decide (`ForkMeshGenie.offFrom`).
  Showing it again writes `on`. The flag survives sign-out.
- Settings → Appearance "Show the Genie spider" calls
  `ForkMeshGenie.hide()`/`show()` live. For visitors the footer shows a
  "Show the Genie spider" button while it is hidden.
- `hide()`, `show()` and Hide for this visit dispatch a `forkmesh:genie`
  event on `window`. Teardown aborts all listeners through one
  `AbortController` and removes every element it created.

## Lines

`ForkMeshGenie.tips(context)` builds the list on every click; every line is 5
to 10 words (`ForkMeshGenie.wordCount`, checked by the Node test): unread
news for signed-in users (notifications from
`GET /api/notifications?node=…&limit=1`, cached 5 minutes in
`forkmesh.genie.inbox`, or new chat messages), then the page's line
(`PAGE_LINES`), then general lines (`GUEST_LINES`, `MEMBER_LINES`).

## Halloween

While `<html>` has `data-season="halloween"` (set by `season.js`, see
[the seasonal theme](../app/seasonal-theme.md)), the painter swaps the teal
glow (`TEAL_GLOW*`) for orange (`EMBER*`) and draws `witchHat()` instead of
`turban()`: a purple-black hat with a bent tip that flops as it walks, an
orange band and a glowing buckle. The check runs every frame, so the Genie
changes as soon as the season does. Effect strokes built with `glowRgb()`, the
focus ring and the speech bubble border follow the same colour.
