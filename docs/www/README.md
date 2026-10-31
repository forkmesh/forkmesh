# Website (`www/`)

The `forkmesh-www` Worker serves `forkmesh.com` and `www.forkmesh.com`: the
landing page, marketing pages, the blog and RSS, the hosted documentation
(including the Markdown library rendered from `docs/`), the installer
scripts, and shared site chrome. It is a small JavaScript Worker in front of
Cloudflare Static Assets, deployed from `www/wrangler.production.toml`. The
App (`app.forkmesh.com`) and World run on the core nodes; the Worker has no
service binding and reaches the App over the public internet at
`APP_ORIGIN`. Everything dynamic is proxied or redirected to the App (see
[docs/app](../app/README.md)). `www/wrangler.toml` has no `name` and no
route, so a deploy from it fails instead of creating a second Worker.

The site is the one part of the served stack still on Cloudflare; the cores
behind app and world are reached by DNS round robin, not through it
([architecture](../architecture.md),
[censorship resilience](../censorship-resilience.md)). Index:
[docs/README.md](../README.md).

## Code map

| Path | Responsibility |
| --- | --- |
| `www/worker.js` | Router: redirects, App proxy (`proxyToApp`, `appFetch`), RSS aliases, certification badges, `/api/home-availability`, attribution capture, the signed edge-client header |
| `www/wrangler.production.toml` | Worker `forkmesh-www`, custom domains `forkmesh.com` and `www.forkmesh.com`, assets, `run_worker_first`, vars `APP_ORIGIN`, `WORLD_ORIGIN`, `ATTRIBUTION_*`, `APP_FALLBACK_ORIGINS` |
| `www/wrangler.toml` | Assets, `run_worker_first`, vars `APP_ORIGIN`, `WORLD_ORIGIN`; no name or route, so it cannot be deployed. Tests and local builds still read its assets and `run_worker_first` |
| `www/public/` | Static site: `index.html`, marketing pages, `blog/`, `docs/` (hand-written pages), `assets/`, `_redirects`, `_headers`. Every HTML page except `docs/git-compatibility/` is rendered from `www/templates/` |
| `www/templates/`, `app/tools/site_templates.py` | Page templates with inheritance: `base.html`, `layouts/blog-post.html`, `partials/`, one child per page under `pages/`, and their renderer ([templates](templates.md)) |
| `www/public/genie-spider.js` | The Genie spider on every website page and most App pages ([genie-spider](genie-spider.md)) |
| `tools/site_crawler.py`, `.forkmesh/workflows/site-crawler.json` | Crawler, the special agent that crawls the live site read-only and reports broken links, SEO, accessibility and header issues ([site-crawler](site-crawler.md)) |
| `www/public/site-header.js`, `home-header-auth.js`, `api-client.js` | Login state and the shared browser API client ([sessions](../app/auth-sessions.md)) |
| `www/public/analytics.js`, `analytics-sdk.js` | Consent-gated first-party analytics ([analytics](../app/analytics.md)); the SDK is a generated copy. Conversion points (`data-forkmesh-product-action`) and markup experiments (`data-fm-experiment`) are listed under [conversion tracking](../app/analytics.md#conversion-tracking) |
| `www/public/homepage-downloads.js` | The downloads card, fed by the signed release manifest |
| `www/public/install.sh`, `uninstall.sh` | Desktop installer and uninstaller served at `forkmesh.com/install.sh` (default `FORKMESH_HOST` `https://forkmesh.com`) |
| `www/public/auth-mesh.js`, `auth-mesh.css`, `assets/land-mask.png` | The dotted world map behind the login and signup pages, drawn from a generated Natural Earth land mask ([auth backdrop](auth-mesh.md)) |
| `www/public/auth-page-theme.css` | Light and dark tokens shared by the auth pages ([auth theme](auth-theme.md)) |
| `www/public/ecosystem.html`, `#ecosystem` in `www/public/index.html` | The ForkMesh Ecosystem directory at `/ecosystem` and `/ecosystem/<slug>`, and its landing-page section, both read from `GET /api/ecosystem` ([docs/app/ecosystem.md](../app/ecosystem.md)) |
| `www/public/features.html` | The `/features` catalog. Self-contained: its styles and an SVG icon sprite are inline and it runs no script of its own; see Traps |
| `#everywhere` in `www/public/index.html`, `assets/everywhere/` | "Your mesh, wherever you work": the connection line, the three device mockups and their screenshots |
| `www/public/mobile.html`, `mobile.css`, `assets/mobile/` | The `/mobile` page for the iPhone app in the landing page's style, built from real app screenshots ([mobile page](mobile-page.md)) |
| `#mobile-app` in `www/public/index.html` | "Your mesh, now in your pocket": the landing page's call to action for the phone app ([mobile page](mobile-page.md#landing-page-section)) |
| `docs/` | Public documentation library (Markdown and JSON schemas); see the note at the top of [docs/README.md](../README.md) |
| `app/tools/build_site_assets.py www` | Stages `www/dist` (the upload), rendering the docs library into it |
| `app/tools/docs_site.py`, `docs_markdown.py`, `docs_history.py` | Render `docs/**/*.md` to `/docs/<path>/`, `/docs/library/`, `/docs/history/`, `/docs/documents/` |
| `www/public/robots.txt`, `sitemap.xml` (built) | Crawler rules and the sitemap they point to; see Security |
| `www/tests/` | Python and Node tests for routing, the docs library, headers and scripts |
| `www/deploy.sh` | Calls `app/deploy.sh www`; see Traps |

## Architecture

Routing in `www/worker.js`, in order:

1. `/@name` is proxied to the App so public profiles keep the
   `forkmesh.com` address.
2. RSS aliases serve `/rss.xml`.
3. Certification badges and `/api/home-availability` (a cached probe of the
   App and the three core nodes, 180 s) are answered by the Worker itself.
4. `/api` redirects to the App developer page; `/api/*`, Git Smart HTTP,
   ActivityPub, `/.well-known/*`, `/nodeinfo/*`, `/health`, `/mcp`, hackathon
   and pitch-deck documents, and every non-GET request are proxied to the App.
5. `/world*` redirects to World; dashboard and other App pages (`APP_PATHS`)
   redirect to the App.
6. `/ecosystem/<slug>` serves `ecosystem.html`; `/market/<id>` serves the
   package page `market/<id>.html`, or `market.html` for an unknown id
   ([package pages](../marketplace/README.md#share-links-and-package-pages));
   other `/<owner>/<repo>` paths redirect to the App.
7. Everything else is a static asset, with first-party attribution captured
   on page navigations (`fm_attr` cookie; [attribution](../app/attribution.md)).

Many redirects are also in `www/public/_redirects` so the asset router
answers them without running the Worker. Static responses carry
`x-forkmesh-worker: www` and `x-forkmesh-deploy-fingerprint`.

- **App proxy without a service binding.** GET and HEAD reads that fail,
  time out (`APP_FETCH_TIMEOUT_MS`, default 15 s) or answer 502/504 are
  retried on each node's own name under `/_app/*` (`APP_FALLBACK_ORIGINS`,
  else `DIRECT_MIRRORS`: `n1.next.forkmesh.com` to `n3.next.forkmesh.com`).
  Writes are sent once.
- **Visitor identity.** Caddy on a core sees only a Cloudflare egress
  address for relayed requests, so the Worker signs the visitor's address
  and country into `x-forkmesh-edge-client` with `WWW_EDGE_SECRET`
  ([edge_client.md](edge-client.md)); the host verifies it before the App runs.
  Without the secret no identity is forwarded.
- **Downloads card.** `homepage-downloads.js` reads
  `.forkmesh/releases/<channel>/release.json`, its `.sig` and
  `desktop/CMakeLists.txt` from app.forkmesh.com, verifies the signature and
  shows one card per platform ([release verification](../mirroring.md)).
  An asset missing from `latest` that is only held back (Windows, macOS) does
  not drop straight to "Coming soon": `earlierAsset` checks the signed
  `candidate-<major>.<minor>.<patch-1…patch-6>` manifests, newest first, and
  links the first verified copy, labelled with that manifest's tag. A
  per-platform rebuild (`build` above 1) shows `Build N` in the details, title
  and label. Two assets of one platform that share a `blob_sha256` render as
  one "Universal" card. A newer `candidate-<version>` is named only when it
  carries a verified asset the card offers. Download counts per card and in
  total come from `/api/repo/forkmesh/forkmesh/releases/downloads` and are
  left out when that call fails.
- **Hero install line.** A Binary / Source switch in the install pill.
  Binary (default) shows `curl -fsSL https://forkmesh.com/install.sh | bash`;
  Source shows `… | FORKMESH_FROM_SOURCE=1 bash` (the variable sits on the
  `bash` side so the installer sees it). Both copies count as the
  `home-install-hero` product action.

## Protocols and interfaces

- `/docs/library/` and every `/docs/<path>/` page are generated from
  `docs/` by `app/tools/docs_site.py`. Relative links, heading anchors and
  schema references are resolved; Git history dates each page. Code and tests
  reference many `docs/**` paths directly, so do not rename or move them.
- `/api/home-availability`: JSON for the homepage availability strip.
- `x-forkmesh-edge-client: v1,<ip>,<country>,<ts>,<mac>`
  ([edge_client.md](edge-client.md)).

## Configuration

- `[vars]` in `www/wrangler.production.toml`: `APP_ORIGIN`
  (`https://app.forkmesh.com`), `WORLD_ORIGIN`, `ATTRIBUTION_SKIP_COUNTRIES`,
  `ATTRIBUTION_COOKIE_DOMAIN`, `APP_FALLBACK_ORIGINS` (the core node
  origins). Optional: `APP_FETCH_TIMEOUT_MS`. `DEPLOY_FINGERPRINT` is passed
  with `--var` at deploy time.
- Compatibility date `2026-09-22`. Observability on at 5% sampling with
  `invocation_logs = false`; traces on with `head_sampling_rate = 1`.
- Secret: `WWW_EDGE_SECRET`, the same value as in the fleet `secrets.env`
  (`tools/cutover_identity.py put-www-secret`). It carries across deploys.

## Build, run, test

```sh
python3 app/tools/site_templates.py            # render www/templates into www/public
python3 app/tools/build_site_assets.py www     # stage www/dist
python3 -m pytest -q www/tests
node --test www/tests/worker_routing.test.mjs
node --test www/tests/genie_spider.test.mjs
```

## Logo files

`logo.svg` at the repository root is the one source: `python3
tools/render_logo.py` (pycairo, Pillow) rewrites every other logo file, then
`python3 app/tools/embed_og_logo.py` re-bakes the og-card copy. Pages use
`/assets/logo.svg` and list `/favicon/favicon.svg` after `favicon.ico`
(`app/tests/test_favicon_metadata.py`); rasters stay where SVG is not accepted.

That test holds every page under `app/public` and `www/public` to the same
head: all four favicons, the manifest, `application-name`,
`apple-mobile-web-app-title`, `theme-color` `#090909`, `color-scheme`
`light dark` and a description. Only the www landing page and the investor
deck (`app/public/pitch-deck/deck.html`) may use `color-scheme: dark`; the
deck advertises `light dark`. The deck's CSP (`PAGE_CSP` in
`app/src/pitch_deck_runtime.py`) allows `manifest-src 'self'` so its manifest
link loads.

## Investor deck

The deck a CRM contact opens (`/pitch-deck/<token>`, served by
`app/src/pitch_deck_runtime.py`) uses the homepage's look, because it goes out
with every investor email.

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

## Operations

The node auto-deploy agent keeps forkmesh.com current: after each deploy
check it compares the website fingerprint with the live
`x-forkmesh-deploy-fingerprint` and, when they differ, runs `wrangler deploy
--config wrangler.production.toml` from the pinned worktree
([auto-deploy](../operations/auto-deploy.md), "The website Worker"). By hand:

```sh
cd www && npx --yes wrangler@4.136.2 deploy --config wrangler.production.toml \
  --var DEPLOY_FINGERPRINT:$(python3 ../app/tools/deploy_targets.py fingerprint www)
```

Wrangler 4.136 needs Node 22 or newer. To roll back: `npx wrangler rollback
--name forkmesh-www`.

## Security

- The edge-client MAC binds method and path, is single use per core within
  300 s, and the header is always stripped before the App code
  (`app/host/forkmesh_host/request_guard.py`).
- `script-src 'self'` and `style-src 'self'` on proxied App documents: App
  subresources a proxied page needs (`/dashboard.js`,
  `/dashboard/tailwind.css`, `/qr.js`) are proxied same-origin
  (`APP_SUBRESOURCES`).
- `www/public/install.sh` and `uninstall.sh` refuse user-data wipes from an
  AI agent shell or outside `HOME`; see [AGENTS.md](../../AGENTS.md).
- Product-action analytics are allowlisted; the build fails when a CTA is
  instrumented but not allowlisted.
- The downloads card fetches the public release files with
  `credentials: "omit"`. `api-client.js` replaces `window.fetch` and turns
  every API request's credentials into `include`, and the raw-file route
  answers public reads with `Access-Control-Allow-Origin` but no
  `Access-Control-Allow-Credentials`, so a credentialed read is dropped by
  the browser. The wrapper leaves an explicit `omit` alone (test:
  `test_release_metadata_is_fetched_without_credentials`).

Crawlers and scrapers:

- `robots.txt` has one `User-agent: *` group: everything is allowed except
  `/api/`, `/mcp` and personal hackathon pages, and it names the sitemap. It
  never names a blocked crawler: blocking is enforced, not requested.
  `www/tests/test_robots_and_sitemap.py` fails if a second `User-agent` group
  appears.
- `sitemap.xml` is written by `build_www_sitemap` in
  `app/tools/build_site_assets.py`: each staged `.html` page at its canonical
  URL, without `404.html`, `ios-install.html`, `noindex` pages, off-site
  canonicals and redirected paths. Blog posts carry `<lastmod>`.
- `stamp_canonical_links` gives every staged page without one a
  `<link rel="canonical">` on forkmesh.com, so the `www.` copy is not indexed
  as a second site.
- Every crawlable document is a Worker asset. `/status`, `/network` and
  `/leaderboards` are written in `app/public/` and copied into `www/dist`
  (`WWW_APP_PAGES`); the App 308s them to forkmesh.com (`WWW_SITE_ROUTES`).
  Only per-record documents (`/@name`, hackathon records, pitch decks) stay
  App-rendered behind the Worker.
- `llms.txt` (llmstxt.org format) is a hand-written site map for language
  models; `www/tests/test_llms_txt.py` fails when a linked page no longer
  resolves.
- Blocked crawlers are the enabled rows of the `request_block_rules` table.
  `app/sync_request_block_rules.sh` (or `tools/forkmesh-sync-block-rules.sh`,
  `--dry-run` to print) publishes them as the zone's WAF rule; automatic probe
  bans ([../app/probe-auto-ban.md](../app/probe-auto-ban.md)) are never
  published. `app/sync_request_rate_limit.sh` keeps the zone rate limit (120
  requests per 10 s per address and colo). The cores enforce the same table
  and limit for app and world ([host/README.md](../host/README.md)).

## Traps

- HTML pages in `www/public/` are rendered output: edit
  `www/templates/pages/<same path>` (or `base.html` and the partials), then
  run `python3 app/tools/site_templates.py` ([templates](templates.md)). Blog
  index cards copy each post's description, so rerun the renderer after
  editing a post.
- A blog post whose hero image is not committed breaks the website build
  (`build_www_feeds` stats every post image for the RSS enclosure).
- `www/deploy.sh` (`app/deploy.sh www`) deploys `wrangler.production.toml`;
  `www/wrangler.toml` must keep no name or route.
- Files in `SHARED_FILES` (`api-client.js`, `site-header.js`,
  `analytics.js`, `styles.css`, ...) are staged into the App upload too; a
  change reaches App pages only when the App is deployed.
- `www/public/analytics-sdk.js` is generated by
  `marketplace/packages/analytics/tools/sync_assets.py`; edit the package.
- Removed pages must also leave `_redirects`, `app/src/static_routes.py`,
  `app/tools/deploy_targets.py` and `docs/external-dependency-map.md`.
- `features.html` draws filled icons from an inline SVG sprite whose
  knockouts are masks; keep the `.fx-sprite mask > g` rule, and hide the
  sprite with zero size, not `display: none`. Its six-column grid's
  `nth-child` centring rules must stay scoped to their breakpoints.
  The counts are written by hand: a card added or removed changes its
  section's `.fx-count` line, its `.fx-toc-count`, the hero's `.fx-stats`
  line and the outro title. A card whose feature is a Market package
  (`marketplace/catalog.json`, including the core switches in
  [core-plugins](../marketplace/core-plugins.md)) carries a `.fx-tag-plugin`
  chip, a link to `/market/<id>` with the package's name, and counts as a
  plugin; built-in features have none. Removed features leave the page.
- The textured button recipe is copied as tokens into each stylesheet that
  owns buttons (`--btn-*`, `--button-*`, `--fm-btn-*`, `--mesh-btn-*`,
  `--bs-*`); `www/tests/test_button_texture.py` and the page tests check the
  copies agree.
- Sticky pricing headers need no `overflow: hidden`/`auto` on any ancestor.
- `styles.css` carries the shared page kit (`--display`, `--kit-*`, `.kit-*`
  classes; `<body class="kit-page">`). A `.kit-hero` in a grid cell needs
  `margin: 0`.
- Checkboxes and radios on landing-style pages use the ink colour (white in
  dark mode, near-black in light), never green or blue: green is for status
  and blue belongs to the dashboard. `styles.css` sets
  `accent-color: var(--foreground)` on both through a `:where()` rule, so a
  new page gets it without its own rule and a page rule still overrides it.
- A rule that sets `display` beats `[hidden] { display: none }`; elements
  hidden by script need an explicit `[hidden]` rule.
- `homepage-downloads.js` keeps `knownAssets` as `["asset", [label, arch]]`
  entries because the App's status probe reads that list from the deployed
  script (`_homepage_download_asset_names`).

## Plugin-entry gate

Core features are account-synced packages
([core-plugins](../marketplace/core-plugins.md)). Any element marked
`data-plugin-entry="<package-id>"` is hidden while a signed-in account's
`/api/plugins/` list lacks that id (or the package's places lack `web`);
signed-out visitors see everything, and deep links keep working because only
entry points are hidden. `public/api-client.js` caches the list per account
in localStorage `forkmesh.plugins.v1` (rechecked at most every 5 minutes, on
`forkmesh:session-restored`, and on `storage` events) and writes one
generated style rule,
`[data-plugin-entry]:not([data-plugin-entry="<installed id>"])…{display:none!important}`,
so elements rendered later are covered without a DOM walk. API:
`ForkMeshAPI.pluginShown(id[, account])`, `rememberPlugins(account, ids)`,
`rememberPlugin(account, id, installed)`, `refreshPlugins(force)`; the Status
Page calls (`rememberStatusPage`, `refreshStatusPage`, `statusPageShown`,
`data-status-entry`, `forkmesh.statusPage.v1`) ride on the same fetch.

## Pages contributed by packages

A Market package can add a page to the website through a `www.page`
contribution (`app/tools/build_app_plugins.py`, `WWW_SOCKET`): a `route` that
must not collide with a core route (`static_routes.CORE_RESERVED_ROUTE_PREFIXES`)
or another package, a title and its assets. Each package page documents
itself; current ones include the Feed's public post page
`forkmesh.com/feed?post=<id>` ([instance-feed.md](../marketplace/instance-feed.md)).
Test: `app/tests/test_www_plugin_pages.py`.

## Related docs

- [docs/app/README.md](../app/README.md), [World](../world/README.md)
- [OPERATIONS.md](../operations/README.md), [auto-deploy](../operations/auto-deploy.md)
- [templates](templates.md), [genie spider](genie-spider.md),
  [site crawler](site-crawler.md), [auth backdrop](auth-mesh.md),
  [auth theme](auth-theme.md), [mobile page](mobile-page.md)
- [docs/README.md](../README.md) (public library index)
  (optional self-host; not how forkmesh.com's app runs)
- [attribution](../app/attribution.md)
