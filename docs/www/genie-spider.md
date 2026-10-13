# The Genie spider on the web

Part of [the website docs](README.md). Added 2026-10-06 (issue 1490): after
the phone ([../native/GENIE.md](../native/GENIE.md)) and the desktop
([../desktop/genie.md](../desktop/genie.md)), the user asked for the spider on
every web page, helpful to new visitors and informative for signed-in users.

The web spider is a guide, not a pet or a voice assistant: it has no health,
speech or model. It looks like the other two and throws the same way. It says
nothing until it is clicked (see "Quiet until clicked").

## Where the code is

| Part | File | Tests |
| --- | --- | --- |
| Painter, behaviour, bubble, tips, interactions, keep-clear | `www/public/genie-spider.js` (one file, its styles injected) | `www/tests/genie_spider.test.mjs` (tips, `clearX`, element kinds, the interaction bag), `www/tests/test_site_templates.py` (every page loads it) |
| Website pages | `www/templates/partials/genie-spider.html`, included by `base.html` ([templates.md](templates.md)) | `test_every_site_page_loads_the_spider` |
| Docs library pages | `PAGE_TEMPLATE` in `app/tools/docs_site.py` | |
| Git compatibility page | the HTML template in `tools/git_conformance.py` | |
| Dashboard | the `geniescript` page token in `dashboard/shell.html`, filled by `compose_page_from_reader` in `app/src/dashboard_shell.py` | `test_dashboard_chat_leaves_the_spider_out` |
| Hide for good, Show again | `offFrom`, `offCookie`, `isOff`, `hidden`, `hide`, `show`, `shown` on `window.ForkMeshGenie` in `genie-spider.js` | `genie_spider.test.mjs` (cookie over flag, cookie domain, nothing built when off, what hide and show write) |
| Settings switch | "Show the Genie spider" (`[data-genie-visible]`) under Appearance in `dashboard/partials/views/settings.html`; `renderGenieSetting`, `saveGenieSetting`, `watchGenieSetting` in `dashboard/js/02-helpers.js`, the click in `08-repo-detail-network.js` | |
| Footer way back | `mountGenieReturn` in `www/public/site-footer.js`, `.site-footer-genie` in `site-footer.css` | |
| Other App pages | a script tag in `login`, `signup`, `forgot-password`, `reset-password`, `verify-pending`, `start`, `ledger`, `leaderboards`, `network`, `status`, `mirror-payouts`, `market-reviews`, `notes/view`, and the unrouted `dashboard/{calendar,notes,tasks}` copies | `test_app_pages_load_the_spider` |

`genie-spider.js` is in `SHARED_FILES` (staged into the App upload) and in
`CACHE_BUSTED_ASSETS`, so it gets a content-hash `?v=`. An App page picks up a
change only when the App is deployed too.

Left out on purpose: the full-screen chat (`chat.html` and
`/dashboard/chat`), whose composer is part of the layout rather than fixed,
so the spider would cover the send button; the OAuth consent screen
(`oauth-authorize.html`), where nothing should distract from the decision;
and the pitch deck. A page opts out with an empty `{% block genie %}` or
`data-genie="off"` on `<html>`.

## On a white-label instance (2026-10-10)

The spider is the `genie` instance plugin and is off on a new instance
([../INSTANCES.md](../INSTANCES.md), "Blog card, Genie spider and status
page"). When the page carries the instance brand script
(`script[data-instance-brand]`), `boot()` waits for the DOM, then
`instanceAllows()` reads `/api/instance` once and builds the spider only if
the `genie` plugin is enabled; while it asks, and if the read fails, nothing
is built. On forkmesh.com there is no brand script and nothing changes. The
Settings switch row carries `data-instance-plugin="genie"`, so it is hidden
with the plugin.

## The spider

- The painter is a canvas port of `GenieSpiderPainter.cpp` (itself a port of
  the phone's `GeniePainter`): same colours, legs, turban, six eyes, blinking,
  reactions (wave, cheer, nod, eureka, giggle, surprise, dizzy) and squash on
  landing. It draws on a 78 px canvas (62 px under 520 px wide) at the
  device pixel ratio, capped at 3.
- Feet: as on the desktop, all eight feet stand on the floor line (`GROUND`,
  96.2 of the 100-unit box) and are mapped into the body's frame (`toBody`),
  so the breathing bob and sway move the knees while the feet stay put;
  walking lifts them in turn and in the air they splay. The back knees sit
  further out (`KNEES`), the claw's curve is capped at 5.5 units, and a soft
  contact shadow sits under it while it stands. `place()` moves the canvas
  down by the 3.8% under the floor line, so the feet touch whatever it
  stands on.
- It stands on the bottom of the viewport, or on top of any fixed or sticky
  element anchored there (the dashboard's composer pill, a consent banner).
  These are found with `document.elementsFromPoint` along the bottom edge,
  every 1.5 s when idle and every 0.3 s while moving. It climbs when one
  appears under it and falls when it walks or is thrown off one. A throw lands
  on such an element only when it comes down from above it, as on the
  desktop.
- Its home spot starts at the bottom right (94% of the width). Every 5 to 13 s
  it either plays with something on the page (half the time, see below) or
  walks up to 260 px from home at 70 px/s. Where a throw comes to rest becomes
  its home (`forkmesh.genie.spot` in `localStorage`, a fraction of the width).
  Every one of these spots is moved off buttons and logos first (see
  "Keeping clear of buttons and logos").
- Its eyes follow the pointer within 480 px.
- Throwing is stepped at 120 Hz. It applies 2,200 px/s² of gravity and
  quadratic air resistance, so a fall approaches about 2,000 px/s instead of
  gaining speed without limit; any throw remains capped at 4,200 px/s. The
  floor keeps 30% of a hard vertical impact and 78% along it, while walls and
  the top keep 42%. Impacts below 240 px/s settle immediately, then a skid
  slows by 2,400 px/s² and bounces off the sides. The release position joins
  the final 80 ms velocity sample, so a quick flick is measured through the
  moment it leaves the pointer.
- Frames: `requestAnimationFrame` while it moves, reacts, plays, is near the
  pointer or shows a bubble (drawn at up to 30 fps, every frame in flight);
  12 fps through `setTimeout` when idle; none while the tab is hidden. The
  frame step is clamped to 0 to 0.1 s: the first `requestAnimationFrame`
  timestamp after a wake can be earlier than the `performance.now()` taken
  when waking, and a negative step ran interaction clocks backwards (found in
  issue 1750). An interaction that throws ends itself rather than stopping
  the frame loop. With
  `prefers-reduced-motion` it never wanders, its idle motion stops, and it
  stops drawing when nothing changes.

## Playing with the page

Added 2026-10-07 (issue 1750): the user asked for the spider to interact with
things on the page, on the web as on the desktop and the phone, and never to
cover logos or buttons.

When an idle turn comes up (every 5 to 13 s, standing, not parked, held,
thrown or showing the bubble or menu, tab visible), half the time it plays
instead of wandering. It samples an 8 × 12 grid of points over the viewport
with `document.elementsFromPoint`, takes each hit's nearest heading, button,
link, image, svg, input, code block, logo or card, and for a hit that is a
container no taller than half the viewport also up to 6 of those inside it.
At most 32 candidates are kept: visible, at least 10 × 8 px, under half the
viewport, outside its own UI, and (when it may walk) within
max(420 px, half the width) of it. `ForkMeshGenie.elementKinds` sorts each
into kinds: `heading` (h1 to h3; `short` too when 8 words or fewer), `button`,
`link` (`nav` too when at least 3 links share its row), `image`, `logo`
(class, id, alt or aria-label mentions logo), `input`, `code` (`pre`) and
`card` (`article` or a class with "card").

`ForkMeshGenie.drawInteraction` takes the next interaction from a shuffled
bag that fits a kind on the page; the bag refills only when nothing left in it
fits, so every interaction is used before one repeats wherever the page has
something it fits. Then it picks a fitting element, walks along its floor to
it (beside it when the element is down at the floor, never under it; under its
near end when it is higher up) and plays:

| Interaction | Fits | What it does |
| --- | --- | --- |
| `look` | anything | eyes on it (the only one with reduced motion or Stay put) |
| `read` | heading, code | eyes sweep along it twice, then a nod |
| `wave` | button, link, logo, image, heading, card | the wave reaction at it |
| `point` | button, link, logo, image, input | a front leg reaches out toward it (new `point` pose in the painter) |
| `thread` | heading, image, logo, card, code, button | a silk thread shoots to its nearest edge, two tugs (the spider squashes), then reels back |
| `sparkle` | logo, image, heading, card | seven sparkles twinkle around its edges, eureka |
| `outline` | button, input, card, code, link, logo | a soft glowing outline pulses around it |
| `hop` | button, link, logo, image, card, heading | two little hops at it, then a nod |
| `cheer` | button | the cheer reaction |
| `giggle` | link | the giggle reaction |
| `surprise` | image, logo | the surprise reaction |
| `nod` | a row of nav links | eyes and a dot move along up to 6 links in the row, one nod each |
| `count` | a short heading | a glowing dot appears over each word in turn, then eureka |
| `dizzy` | code | eyes race along the lines, then the dizzy reaction |
| `type` | text input | the thinking mood |

No interaction shows text. Threads, sparkles, outlines and dots are drawn in an SVG overlay
(`.fm-genie-fx`, fixed, `pointer-events:none`, under the spider), created when
the interaction starts and removed when it ends. The element itself is never
touched: no style, attribute or DOM change, no click, no focus. Its rect is
re-read each frame; the interaction ends early when the element leaves the
viewport or is removed, on a press, a drag, the bubble, Tuck away, a resize or
the tab hiding. Frames run through `requestAnimationFrame` only while it walks
or plays, as for any other movement. With `prefers-reduced-motion` (and with
Stay put) it never travels and only `look` is drawn.

`ForkMeshGenie.play(name?)` starts one now (or returns `""`) and
`ForkMeshGenie.playing()` says which one runs and its phase; both are used by
the browser check.

## Genie effects

Issue 1833 (2026-10-09): the user asked for the spider to do more genie-like
effects, 30 of them. The same 30 names are on the desktop
([../desktop/genie.md](../desktop/genie.md)) and the phone
([../native/GENIE.md](../native/GENIE.md)).

They are interactions like the ones above: `GENIE_EFFECTS` in
`genie-spider.js` is merged into `INTERACTIONS` with `genie: true`, so they go
into the same shuffled bag, are picked only when something on the page fits,
and walk to their element first. That makes 45 interactions, 30 of them
magic, so most idle plays are now a genie effect. None is `still`, so with
`prefers-reduced-motion` or Stay put none runs. `ForkMeshGenie.genieEffects`
lists the names.

What they draw goes into the same `.fm-genie-fx` SVG overlay, which now
starts with a few gradients (`FX_DEFS`: smoke, orb, mote, bubble, crystal
ball, gold, the shimmer band, a clip rect); the overlay is removed when the
effect ends, so nothing is left behind. Shared pieces: `part` and
`stepParts` (a small particle list: position, velocity, gravity, sway, size,
fade, anchored to the page, the element or the spider), `puff` (a smoke
cloud), `sparks` (a burst of four-point stars) and `smokeTrail` (smoke that
drifts from one point to another). The element is still never touched.

Effects that change the spider itself use `spell` (`scale`, `spin` as a
turn about the vertical axis, `turn` in the plane, `fade`), which `place()`
appends to the button's transform (origin at its feet, 50% 96%) and opacity,
and `s.magic`, which the painter adds as a lilac glow around the body and a
brighter turban jewel. `endPlay` resets both, so an interrupted effect never
leaves the spider small, faded or turned.

| Effect | Fits | What it does |
| --- | --- | --- |
| `poof` | anything | vanishes in a puff of violet smoke and stars, reappears in a second puff at the play spot (it does not walk: `teleport`), then cheers |
| `levitate` | anything | floats about 30 px up, bobbing, on a curling smoke tail, legs splayed, then settles |
| `lamp` | anything | a golden oil lamp grows on the floor beside it, facing the element; smoke curls from the spout to the element; eureka |
| `wishes` | anything | three glowing orbs arc from the turban jewel to the element one after another, each bursts into sparkles |
| `smokeRings` | anything | blows three violet rings toward the element (a squash at each puff); they widen and fade |
| `shimmer` | boxes | a slanted band of golden light sweeps across the element twice, clipped to it, then a glint at its corner |
| `stardust` | heading, button, image, logo, card, code, input | 36 grains arc from the turban and settle twinkling along its top edge |
| `carpet` | anything | a rippling violet carpet with a gold border and fringe slides under it, lifts it about 26 px, floats, lands and slides away |
| `vortex` | logo, image, button, card, heading | 16 sparks spiral inward around it, then a golden flash; eureka |
| `crystal` | anything | a crystal ball on a gold stand appears beside it with teal mist turning inside; its aura glows while it stares |
| `runes` | card, code, image, logo, input, button, heading | six glowing rune shapes (strokes, not letters) circle it |
| `halo` | logo, image, heading, button, card | a gold ring rises from its centre to hover above it, with a glint |
| `fireflies` | anything | ten flickering gold motes drift from the turban and hover over it |
| `whirl` | anything | spins round twice about its vertical axis inside a ring of smoke, then giggles |
| `grow` | anything | swells to 1.5 times in a cloud of smoke, glowing, cheers, and shrinks back in another puff |
| `bottle` | anything | a glass bottle appears at its feet; it swirls, shrinks and fades into it; the bottle glows and wobbles, the cork pops off in a puff and it springs back out |
| `rainbow` | anything | a six-colour arc draws itself from the jewel to the element; eureka |
| `fireworks` | heading, button, logo, image, card | three rockets rise from its top and burst into falling sparks; cheer |
| `bubbles` | card, image, code, input, heading, button | iridescent bubbles rise from it, swaying, and pop |
| `glitter` | heading, image, logo, card, button | thirty little stars drift down over it, spinning |
| `jewelBeam` | anything | the jewel brightens and shines a teal beam onto its nearest edge, which glows |
| `ripple` | button, link, nav, logo, image, input | four rings of light spread out from its centre |
| `moons` | anything | three small moons circle above the turban, then fly to it one by one and sparkle |
| `constellation` | image, card, code, heading, logo | seven stars appear around it one by one, joined by faint lines as they come |
| `crescent` | heading, image, logo, card | a gold crescent moon rises above it, rocking, with twinkling stars |
| `mirage` | image, card, logo, button, code, input | three wavy ghosts of its outline drift up and fade |
| `coins` | button, card, image, logo, heading | nine gold coins pop up, tumble (flipping) to just below it and glint; cheer |
| `flourish` | anything | a looping golden line writes itself from the jewel to it behind a bright star tip; nod |
| `incense` | anything | four wisps of lilac smoke curl upward around it while its aura turns violet; it looks up and nods |
| `whirlwind` | anything | a funnel of smoke spins up around it, lifting it a little, then scatters outward; dizzy |

"Boxes" is every kind except `short` and `nav`, which are a heading or a
link anyway. Spots for the lamp and the crystal ball are on the floor beside
the spider, on the element's side; they are part of the overlay, so they
never take clicks.

## Keeping clear of buttons and logos

The spider's body is the painted spider, not the canvas padding:
`ForkMeshGenie.body` (5% to 95% of the width, 18% to 97% of the height), plus
4 px. It must not overlap any visible `a`, `button`, `input`, `select`,
`textarea`, `summary`, `label[for]`, `[role=button|link|tab|menuitem|checkbox|switch]`,
focusable `[tabindex]`, `[onclick]`, `img`, `svg`, `picture`, `video`,
`canvas`, or anything whose class, id or alt mentions logo.

- Finding them: `elementsFromPoint` every 20 px along the width at four
  heights inside where its body would be at that x (on the floor or a fixed
  bar), each hit and its ancestors checked against that list, and for a hit no
  taller than half the viewport up to 40 matching descendants (so a 21 px
  button in a banner is found even between sample rows). Each element's
  `getClientRects` are used, so a link wrapping over two lines is two boxes.
  Elements larger than a quarter of the viewport are ignored: a hero
  background or a full-width picture cannot be avoided anywhere along the
  floor and would otherwise pin it. The result is cached for 1 s.
- Choosing a spot: `ForkMeshGenie.clearX` (pure) takes the floor span, the
  body, the boxes, the floor height at each x and the preferred x, tries the
  preferred x, both ends and the two sides of every box (and the edges of
  fixed bars, where the floor height changes), and returns the nearest x that
  covers nothing. If none is clear it returns the least-covered one by
  overlapped area. A full-width row of links at a 390 px phone width is such
  a case.
- When: the home spot on load, every wander target, every interaction spot,
  where a throw or skid comes to rest (that spot becomes home), after Tuck
  away (it peeks from the bottom-left corner instead when the right one would
  cover something), 200 ms after any scroll ends (capture listener, so inner
  scroll panes count), and after a resize. While it stands idle it checks its
  own footprint at most once a second (about 30 point tests) and walks to a
  clear spot when something has moved under it; when no spot is clear it
  rechecks only every 5 s.

## Quiet until clicked

Issue 1819 (2026-10-08): the user asked for the spiders to say nothing unless
they are clicked or tapped on purpose, not dragged, and then to say one
helpful line of 5 to 10 words.

- It never opens the bubble by itself: the first-visit greeting
  (`forkmesh.genie.met`) and the signed-in briefing that opened 4 s into a
  session (`forkmesh.genie.briefed`) are gone, and no interaction draws words
  or numbers.
- A click (or Enter/Space, since it is a `<button>`) waves and shows the next
  line in a small bubble above it: one sentence, no title, link or buttons.
  The bubble closes after 5 s, or 4 s after the pointer leaves it; Escape
  closes it. A press that moves more than 6 px is a drag and says nothing;
  one held for 0.55 s opens the menu instead.
- Right-click or press-and-hold opens the menu ("What would you like?"):
  Next tip, Stay put / Wander again (`forkmesh.genie.still`), Tuck away (it
  walks into the bottom-right corner and only peeks out;
  `forkmesh.genie.parked`; a click brings it back), Hide for this visit
  (`forkmesh.genie.away` in `sessionStorage`, so it is gone until the tab
  closes) and Hide for good (see "Hiding it for good").

## Hiding it for good

Issue 1831 (2026-10-09): the user asked to make the spider disappear
completely from its long-press menu and from Settings.

- Hide for good, the last item of the menu, removes the spider at once and
  keeps it away on every later page load: `genie-spider.js` still loads (it
  is one cached file) but builds nothing, so there is no canvas, style,
  listener, timer or notifications fetch. Hide for this visit stays: it is
  the lighter choice for someone who only wants it gone now, and Tuck away
  the lighter one still.
- Where it is stored: `forkmesh.genie.off` = `1` in `localStorage`, and a
  cookie `forkmesh_genie=off` (or `on`), one year, `SameSite=Lax`, on
  `Domain=.forkmesh.com` when the page is on forkmesh.com or a subdomain
  (host-only elsewhere). This is the pattern of the theme cookie
  (`forkmesh_theme`, `saveDashboardTheme`). `localStorage` belongs to one
  origin, so a flag alone would hide it on forkmesh.com and leave it on
  app.forkmesh.com, and a switch in Settings on app.forkmesh.com could not
  bring it back on forkmesh.com. The cookie is read first: `off` hides it,
  `on` shows it whatever the origin's own flag says, and only without the
  cookie (a `file:` page, a browser that refuses it) does the flag decide
  (`ForkMeshGenie.offFrom`). Showing it again therefore writes `on` rather
  than deleting the cookie, so a stale flag on the other origin cannot keep
  it hidden. `forkmesh.genie.off` is kept when signing out, with the theme
  keys (`clearForkMeshBrowserState`, `clearSignedInBrowserState`).
- Bringing it back: Settings → Appearance has a "Show the Genie spider"
  switch. It reads `ForkMeshGenie.isOff()` (so it is disabled on a page that
  did not load the spider) and calls `ForkMeshGenie.hide()` or `show()`,
  which take effect on the current page without a reload; `show()` also
  clears Hide for this visit. Settings needs an account, so for visitors the
  website footer shows a "Show the Genie spider" button next to System status
  while the spider is hidden (`ForkMeshGenie.hidden()`), and not on pages
  that leave the spider out.
- `hide()`, `show()` and Hide for this visit send a `forkmesh:genie` event on
  `window` (`detail.off`, `detail.hidden`); the Settings switch and the footer
  button follow it. Taking the spider down (`teardown`, returned by `start`)
  aborts its document and window listeners through one `AbortController`,
  clears its interval and timeouts, ends any interaction and removes the
  button, bubble and style; `show()` builds it again from scratch.

## Lines

`ForkMeshGenie.tips(context)` builds the list fresh on every click and the
clicks step through it. It is pure and exported so the Node test can call it;
the test checks every line on every page is 5 to 10 words
(`ForkMeshGenie.wordCount`).

1. **Unread news** for signed-in users (a `forkmesh.session` in
   `localStorage`), from `briefing(context)`: "You have n unread
   notifications." (`GET /api/notifications?node=…&limit=1`, which returns
   `unread`; cached 5 minutes in `forkmesh.genie.inbox`), or else "n new chat
   messages since you last looked." (the `forkmesh.chatActivity` counters
   `site-header.js` caches, against `forkmesh.chat.activitySeen`). Nothing
   when there is no news.
2. **The page's line** (`PAGE_LINES`), matching a path or its sub-paths, for
   `guest`, `member` or `any`, and one for the home page.
3. **General lines**: for visitors (`GUEST_LINES`) cloning without an account,
   the desktop node, a free account, Getting started and how to move the
   spider; for members (`MEMBER_LINES`) agents, adding a repository, the
   desktop node, referrals, Settings and how to move the spider.

## Verified

Issue 1833 (2026-10-09), headless Chromium through Playwright against a
harness page (logo, a row of nav links, cards with headings, buttons, an
input, an SVG picture, a code block and an article) loading
`genie-spider.js` from disk: each of the 30 genie effects was forced with
`ForkMeshGenie.play`, screenshotted 0.5, 1.3 and 2.1 s into its act (whole
page and around the spider) and looked at; each ran to the end, removed
`.fm-genie-fx`, and left the spider's opacity and transform as before (no
rotate or scale). The first run found `shimmer` reading the overlay before
creating it, which threw in the frame loop; it now creates its group first,
and an effect's `begin` runs inside the same `try` as `tick`, so a failing
one ends itself. The rainbow's bands were offset vertically and collapsed
into one line when the element was straight above; they are now offset
across the arc. No page errors after the fixes. Not tried: Safari, a phone,
the deployed pages, `prefers-reduced-motion` in a browser (covered by the
`still` test).

Issue 1831 (2026-10-09), headless Chromium through Playwright, with
`www/public` and `app/public` served under a forkmesh.com subdomain and the
API stubbed: the menu lists Hide for good; choosing it removed the spider and
its bubble, wrote the flag and `forkmesh_genie=off`, and showed the footer
button; after a reload, and on another page with `localStorage` cleared (the
other origin's case), no spider and no style were built; the footer button
brought it back and wrote `on`. On `/dashboard/settings` with a stored
session the switch started on, turning it off removed the spider live, a
reload kept it off with no spider notifications fetch, and turning it on
brought it back live. No page errors. Not tried: two real origins, Safari, a
real phone's press-and-hold.

Issue 1750 (2026-10-07), headless Chromium through Playwright against a
harness page (header logo and nav, cards with an input and a code block, a
fixed round Help button 16 px above the bottom, a fixed logo, a footer of
links, logo and button), a page with a cookie banner fixed 12 px above the
bottom, and `www/public` served locally (home, features, about): with home
seeded at 97% it starts beside the Help button, not on it; after scrolling to
the footer, after dropping it on the Help button, and after each of the 15
interactions forced through `ForkMeshGenie.play`, its body overlapped no
button, link, input or logo (the only overlaps were the home hero's background
schematic and, at 390 px, the full-width desktop mockup, both over the
quarter-viewport limit, and at 390 px the harness footer where no clear spot
exists). Every interaction walked, played and left no `.fm-genie-fx` behind;
two started by themselves in 45 s; with reduced motion `thread` was refused
and `look` played without moving; no page errors. Not tried: Safari, a real
phone, the dashboard composer pill on the deployed App.

Issue 1490: the painter, standing on a fixed bar, a throw (bounce off the left wall and
the top, landing on the bar, skid), click, right-click menu, the guest
greeting and the signed-in briefing (plan and unread count from mocked API
answers) were checked in headless Chromium through Playwright, against a
harness page and against `www/public` served locally (about, home, a blog
post, features). Not tried yet: touch press-and-hold on a phone, Safari, and
the dashboard composer pill on the deployed App.
