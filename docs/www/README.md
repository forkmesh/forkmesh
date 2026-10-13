# Website (`www/`)

The `forkmesh-www` Worker serves `forkmesh.com` and `www.forkmesh.com`: the
landing page, marketing pages, the blog and RSS, the hosted documentation
(including the Markdown library rendered from `www/docs/`), the installer
scripts, and shared site chrome. It is a small JavaScript Worker in front of
Cloudflare Static Assets, deployed from `www/wrangler.production.toml`, and
the only Cloudflare Worker still serving traffic. The App
(`app.forkmesh.com`) and World run on the three core nodes; the Worker has no
service binding and reaches the App over the public internet at `APP_ORIGIN`.
Everything dynamic is proxied or redirected to the App (see
[docs/app](../app/README.md)).

The old `forkmesh-next-www` Worker and `next.forkmesh.com` are gone: retired
2026-09-27, and deleted again 2026-09-29 after a deploy from
`www/wrangler.toml` re-created it. `www/wrangler.toml` now has no `name` and
no route, so a deploy from it fails instead of re-creating the Worker, and
`app/deploy.sh www` deploys `wrangler.production.toml`. Settings of both
deleted copies are in
`~/projects/forkmesh/config/fleet/retired-workers/{2026-09-27,2026-09-29}/`
([CUTOVER.md](../CUTOVER.md), "Workers retired" and "forkmesh-next-www deleted
again").

## Code map

| Path | Responsibility |
| --- | --- |
| `www/worker.js` | Router: redirects, App proxy (`proxyToApp`, `appFetch`), RSS aliases, certification badges, `/api/home-availability`, attribution capture, the signed edge-client header |
| `www/wrangler.production.toml` | Worker `forkmesh-www`, custom domains `forkmesh.com` and `www.forkmesh.com`, assets, `run_worker_first`, vars `APP_ORIGIN`, `WORLD_ORIGIN`, `ATTRIBUTION_*`, `APP_FALLBACK_ORIGINS` |
| `www/wrangler.toml` | Assets, `run_worker_first`, vars `APP_ORIGIN`, `WORLD_ORIGIN`; no name or route, so it cannot be deployed. Tests and local builds still read its assets and `run_worker_first` |
| `www/public/` | Static site: `index.html`, marketing pages, `blog/`, `docs/` (hand-written pages), `assets/`, `_redirects`, `_headers`. Every HTML page except `docs/git-compatibility/` is rendered from `www/templates/` |
| `www/templates/`, `app/tools/site_templates.py` | Page templates with inheritance: `base.html`, `layouts/blog-post.html`, `partials/`, one child per page under `pages/`, and their renderer ([templates](templates.md)) |
| `www/public/genie-spider.js` | The Genie spider on every website page and most App pages ([genie-spider](genie-spider.md)) |
| `tools/site_crawler.py`, `.forkmesh/teammates/site-crawler.json` | Crawler, the special agent that crawls the live site read-only and reports broken links, SEO, accessibility and header issues ([site-crawler](site-crawler.md)) |
| `www/public/site-header.js`, `home-header-auth.js`, `api-client.js` | Login state and the shared browser API client ([sessions](../app/auth-sessions.md)) |
| `www/public/analytics.js`, `analytics-sdk.js` | Consent-gated first-party analytics ([analytics](../app/analytics.md)); the SDK is a generated copy |
| `www/public/homepage-downloads.js` | The downloads card, fed by the signed release manifest |
| `www/public/install.sh`, `uninstall.sh` | Desktop installer and uninstaller served at `forkmesh.com/install.sh` (default `FORKMESH_HOST` `https://forkmesh.com`) |
| `www/public/auth-mesh.js`, `auth-mesh.css`, `assets/land-mask.png` | The dotted world map behind the login and signup pages, drawn from a generated Natural Earth land mask ([auth backdrop](auth-mesh.md)) |
| `www/public/auth-page-theme.css` | Light and dark tokens shared by the auth pages ([auth theme](auth-theme.md)) |
| `www/public/ecosystem.html`, `#ecosystem` in `www/public/index.html` | The ForkMesh Ecosystem directory at `/ecosystem` and `/ecosystem/<slug>`, and its landing-page section, both read from `GET /api/ecosystem` ([docs/app/ecosystem.md](../app/ecosystem.md)) |
| `www/public/features.html` | The `/features` catalog. Self-contained: its styles and an SVG icon sprite are inline and it runs no script of its own; see Traps |
| `#everywhere` in `www/public/index.html`, `assets/everywhere/` | "Your mesh, wherever you work": the connection line, the three device mockups and their screenshots; see Recent changes 2026-09-28 |
| `www/public/mobile.html`, `mobile.css`, `assets/mobile/` | The `/mobile` page for the iPhone app in the landing page's style, built from real app screenshots ([mobile page](mobile-page.md)) |
| `www/docs/` | Public documentation library (Markdown and JSON schemas); see the note at the top of [www/docs/README.md](../../www/docs/README.md) |
| `app/tools/build_site_assets.py www` | Stages `www/dist` (the upload), rendering the docs library into it |
| `app/tools/docs_site.py`, `docs_markdown.py`, `docs_history.py` | Render `www/docs/**/*.md` to `/docs/<path>/`, `/docs/library/`, `/docs/history/`, `/docs/documents/` |
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
6. `/ecosystem/<slug>` serves `ecosystem.html`; other `/<owner>/<repo>` paths
   redirect to the App.
7. Everything else is a static asset, with first-party attribution captured
   on page navigations (`fm_attr` cookie; `docs/attribution.md`).

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
  (`deploy/core/edge_client.md`); the host verifies it before the App runs.
  Without the secret no identity is forwarded.
- **Downloads card.** `homepage-downloads.js` reads
  `.forkmesh/releases/<channel>/release.json`, its `.sig` and
  `desktop/CMakeLists.txt` from app.forkmesh.com, verifies the signature and
  shows one card per platform. An asset missing from `latest` that is only
  held back (Windows, macOS) does not drop straight to "Coming soon":
  `earlierAsset` checks the signed
  `candidate-<major>.<minor>.<patch-1…patch-6>` manifests, newest first, and
  links the first verified copy, labelled with that manifest's tag.
  Two assets of one platform that share a `blob_sha256` (the universal
  macOS dmg is published as both `forkmesh-macos-arm64.dmg` and
  `forkmesh-macos-x86_64.dmg`) render as one "Universal" card. The status
  names a newer `candidate-<version>` only when that candidate carries a
  verified asset the card offers; a candidate holding just the mirror-node
  binary leaves `latest` as the named version.
  Each card also shows how many times its blob was downloaded, and the status
  line the total across every release, from
  `/api/repo/forkmesh/forkmesh/releases/downloads`; when that call fails the
  counts are simply left out.

## Protocols and interfaces

- `/docs/library/` and every `/docs/<path>/` page are generated from
  `www/docs/` by `app/tools/docs_site.py`. Relative links, heading anchors and
  schema references are resolved; Git history dates each page. Code and tests
  reference many `www/docs/**` paths directly, so do not rename or move them.
- `/api/home-availability`: JSON for the homepage availability strip.
- `x-forkmesh-edge-client: v1,<ip>,<country>,<ts>,<mac>`
  (`deploy/core/edge_client.md`).

## Configuration

- `[vars]` in `www/wrangler.production.toml`: `APP_ORIGIN`
  (`https://app.forkmesh.com`), `WORLD_ORIGIN`, `ATTRIBUTION_SKIP_COUNTRIES`,
  `ATTRIBUTION_COOKIE_DOMAIN`, `APP_FALLBACK_ORIGINS` (the three `nN.next`
  node origins) and `APP_UPSTREAM = "origin"`, which `worker.js` on `main`
  does not read. Optional: `APP_FETCH_TIMEOUT_MS`. `DEPLOY_FINGERPRINT` is
  passed with `--var` at deploy time.
- Compatibility date `2026-09-22`. Observability on at 5% sampling with
  `invocation_logs = false`; traces on with `head_sampling_rate = 1`.
- Secret: `WWW_EDGE_SECRET`, the same value as in the operator's
  `secrets.env` (`tools/cutover_identity.py put-www-secret`). It carries
  across deploys.

## Build, run, test

```sh
python3 app/tools/site_templates.py            # render www/templates into www/public
python3 app/tools/build_site_assets.py www     # stage www/dist
python3 -m pytest -q www/tests
node --test www/tests/worker_routing.test.mjs
node --test www/tests/genie_spider.test.mjs
```

## Logo files

`logo.svg` at the repository root is the one source. `python3
tools/render_logo.py` (needs pycairo and Pillow) rewrites every other logo file
from it, so after changing the SVG run the script, then `python3
app/tools/embed_og_logo.py` to re-bake the og-card copy into
`app/src/og_card.py`:

- SVG copies: `www/public/assets/logo.svg`, `www/public/favicon/favicon.svg`,
  `office/public/logo.svg`, `desktop/resources/icons/forkmesh.svg` (the
  scalable Linux launcher icon `desktop/install.sh` installs) and the iOS
  `Logo.imageset/logo.svg`;
- `www/public/assets/forkmesh-bimi.svg`: the mark on `#090909`, flattened to
  `rect`/`path` with the transforms on each shape, because BIMI's SVG Tiny PS
  profile has no `g`, `polygon` or `circle` and Tiny 1.2 paths have no arcs
  (circles become four cubic Béziers);
- transparent rasters: the root `logo.png`, `/assets/logo.png` (895x1024, the
  press PNG, og-card source and JSON-LD logo), `logo-96.png` (84x96, e-mail
  only: mail clients do not show SVG), `logo-mark.webp`, the favicon PNGs and
  `favicon.ico` (16/32/48), the desktop `forkmesh.png`/`forkmesh-256.png`
  (window icon, merge certificate, AppImage uses `android-chrome-512x512.png`),
  the Android `fm_logo.png` (padded) and the browser extension icons (Chrome
  manifests take PNG only);
- opaque tiles (mark at 78% on the charcoal gradient): `apple-touch-icon.png`,
  `fediverse-avatar.png`, the Android `ic_launcher` mipmaps and every iOS and
  watchOS `AppIcon` file.

Pages show the SVG: every `<img>` brand mark, `admin_shell.py`, the World
loader and the hackathon pages use `/assets/logo.svg`, and every page head
lists `/favicon/favicon.svg` (`type="image/svg+xml"`) after `favicon.ico`
(`app/tests/test_favicon_metadata.py`). Rasters stay where SVG is not
accepted: e-mail, og/JSON-LD images, app icons, the extension manifest,
Android drawables (VectorDrawable has no skew transform) and the Qt window
icon.

iOS asks for `/apple-touch-icon.png` and `/apple-touch-icon-precomposed.png` at
the site root whatever the page head says, so `_redirects` serves both from
`/favicon/apple-touch-icon.png` with a 200, the way `/favicon.ico` is served.
Without them each phone visit answered 404 and tripped the 404 alert.

## Operations

The node auto-deploy agent keeps forkmesh.com current: after each deploy
check it compares the website fingerprint with the live
`x-forkmesh-deploy-fingerprint` and, when they differ, runs `wrangler deploy
--config wrangler.production.toml` from the pinned worktree
([auto-deploy](../deploy/auto-deploy.md), "The website Worker"). By hand
([CUTOVER.md](../CUTOVER.md), "F. forkmesh.com (www)"):

```sh
cd www && npx --yes wrangler@4.136.2 deploy --config wrangler.production.toml \
  --var DEPLOY_FINGERPRINT:$(python3 ../app/tools/deploy_targets.py fingerprint www)
```

Wrangler 4.136 needs Node 22 or newer. To roll back: `npx wrangler rollback
--name forkmesh-www`. See also `www/docs/operations/worker-split.md`.

## Security

- The edge-client MAC binds method and path, is single use per core within
  300 s, and the header is always stripped before the App code
  (`app/host/forkmesh_host/request_guard.py`).
- `script-src 'self'` and `style-src 'self'` on proxied App documents: App
  subresources a proxied page needs (`/dashboard.js`,
  `/dashboard/tailwind.css`, `/qr.js`) are proxied same-origin
  (`APP_SUBRESOURCES`).
- `www/public/install.sh` and `uninstall.sh` refuse user-data wipes from an
  AI agent shell or outside `HOME` (`74f3694a0`); see `AGENTS.md`.
- Product-action analytics are allowlisted; the build fails when a CTA is
  instrumented but not allowlisted (`05e2848e1`).
- The downloads card fetches the public release files with
  `credentials: "omit"`. `api-client.js` replaces `window.fetch` and turns
  every API request's credentials into `include`, and the raw-file route
  answers public reads with `Access-Control-Allow-Origin` but no
  `Access-Control-Allow-Credentials`, so a credentialed read is dropped by
  the browser. The wrapper leaves an explicit `omit` alone (test:
  `test_release_metadata_is_fetched_without_credentials`).

Crawlers and scrapers:

- `robots.txt` has one `User-agent: *` group: everything is allowed except
  `/api/`, `/mcp` and the personal hackathon pages
  (`/hackathon/join/`, `/hackathon/squad/`), and it names the sitemap. It
  never names a blocked crawler: blocking is enforced, not requested, and a
  list in `robots.txt` would only tell those crawlers which name to change.
  `www/tests/test_robots_and_sitemap.py` fails if a second `User-agent` group
  appears. Cloudflare's managed robots.txt prepends its content-signal
  comments to this file on forkmesh.com.
- `sitemap.xml` is written by `build_www_sitemap` in
  `app/tools/build_site_assets.py` on every www build: each staged `.html`
  page at its canonical URL (`<link rel="canonical">`, else the path the asset
  router serves it at), without `404.html`, `ios-install.html`, pages marked
  `noindex`, canonicals off forkmesh.com, and paths `_redirects` sends
  elsewhere. Blog posts carry `<lastmod>` from their date on the blog index.
- Every staged page has a `<link rel="canonical">` on forkmesh.com:
  `stamp_canonical_links` adds one, at the path the asset router serves, to
  each page that has none, except `404.html`, `ios-install.html` and
  `noindex` pages. The same Worker answers `www.forkmesh.com`, and the static
  asset router cannot redirect by host, so the canonical tag is what keeps
  crawlers from indexing that copy as a second site.
- Every crawlable document is a Worker asset. The public data pages
  `/status`, `/network` and `/leaderboards` are written in `app/public/` and
  copied into `www/dist` by `WWW_APP_PAGES` in `build_site_assets.py`; their
  data still comes from `/api/*` (including the `/api/status/ws` socket)
  through the Worker's App proxy. On `app.forkmesh.com` and node hosts the App
  308s them to forkmesh.com (`WWW_SITE_ROUTES` in `app/src/static_routes.py`)
  and a self-hosted App serves its own copy. App pages that need a session
  (`/ledger`, `/login`, the dashboard) still redirect to the App, and only
  per-record documents stay App-rendered behind the Worker: `/@name`,
  `/hackathon/{challenge,projects}/<id>` and pitch decks. Repository pages
  (`/<owner>/<repo>`) live on `app.forkmesh.com`.
- `llms.txt` (`www/public/llms.txt`, the llmstxt.org format) is a
  hand-written map of the site for language models: a summary of what
  ForkMesh is, then sections of links to the docs, API, protocol and product
  pages, with the less essential ones under `## Optional`. It is a static
  asset, not in the sitemap. Docs links use the URLs the docs build renders
  (`/docs/<slug>/`, see `document_url` in `app/tools/docs_site.py`).
  `www/tests/test_llms_txt.py` builds the docs and fails when a forkmesh.com
  link no longer resolves to a staged page or points at a redirect, so
  renaming or removing a linked page means editing `llms.txt` too.
- Blocked crawlers are the enabled rows of the `request_block_rules` table
  (rqlite). For forkmesh.com, `app/sync_request_block_rules.sh` reads them
  from rqlite over the fleet SSH path and publishes them as the zone's
  "ForkMesh database request policy" WAF rule; run it after changing the
  table. Keyword rules never match a User-Agent naming ChatGPT
  (`ChatGPT-User`, `OAI-SearchBot`), and verified Google crawlers pass too. The shortcut `tools/forkmesh-sync-block-rules.sh` does the same from
  anywhere; add `--dry-run` to print the expression without publishing. Both
  read the Cloudflare token from the fleet `cloudflare.env`
  (`~/projects/forkmesh/config/fleet/`) when `app/.env.production`, which the
  script used to require, is absent. `app/sync_request_rate_limit.sh` keeps
  the zone rate limit (120 requests per 10 s per address and colo). The cores
  enforce the same table and limit for app.forkmesh.com and
  world.forkmesh.com themselves ([host/README.md](../host/README.md),
  "Blocked requests and rate limit").

## Traps

- HTML pages in `www/public/` are rendered output: edit
  `www/templates/pages/<same path>`, or `base.html` and the partials for
  anything shared, then run `python3 app/tools/site_templates.py`. A hand
  edit to a rendered page makes `build_site_assets.py www` refuse to stage and
  fails `www/tests/test_site_templates.py` ([templates](templates.md)).
  A post card or search entry on `blog.html` copies the post's description,
  so after editing a post template run the renderer again before committing.
- A blog post whose hero `.webp` is not committed breaks the website deploy:
  `build_www_feeds` stats every post image for the RSS enclosure and raises
  `FileNotFoundError`. Do not merge a post before its art exists.
- Until 2026-09-29 `www/deploy.sh` (`app/deploy.sh www`) ran `wrangler
  deploy --config wrangler.toml`, which re-created `forkmesh-next-www` on
  `next.forkmesh.com`. It now deploys `wrangler.production.toml`, and
  `www/wrangler.toml` has no name or route; do not add them back.
- The `www.` redirect for `/@name` in `worker.js` checks the host
  `www.next.forkmesh.com`, so on `www.forkmesh.com` a profile is proxied like
  on the apex. The fallback and probe origins are still the `nN.next` names.
- Many `www/docs/` pages still describe the Cloudflare deployment (Workers,
  D1, Durable Objects). Check the code before trusting a page.
- Files named in `SHARED_FILES` in `app/tools/build_site_assets.py`
  (`api-client.js`, `site-header.js`, `analytics.js`, `styles.css`, ...) are
  staged into the App upload as well (`shared_assets`); a change to one
  reaches App pages only when the App is deployed too.
- `www/public/analytics-sdk.js` is generated by
  `marketplace/packages/analytics/tools/sync_assets.py`; edit the package.
- Removed pages must also leave `_redirects`, `app/src/static_routes.py`,
  `app/tools/deploy_targets.py` and the dependency map
  (`www/docs/external-dependency-map.md`); `719089c51` is a worked example.
- `features.html` draws its filled icons as symbols in the sprite at the top
  of `<body>`, and each knockout (the white detail inside a glyph) is an SVG
  mask. The knockout strokes get their black stroke from the page rule
  `.fx-sprite mask > g`, so keep that rule with the sprite. The sprite is
  hidden by giving it zero size, not with `display: none` or `hidden`, because
  masks inside an undisplayed SVG do not render in every browser. A card
  reuses a glyph with `<use href="#fx-name">`.
- The `features.html` grid has six columns and each card spans two, so the
  `nth-child` rules inside the `min-width` media queries can centre a last row
  of one or two cards. Those rules are scoped to their breakpoint on purpose:
  left to cascade into the one-column phone layout, a two-column span creates
  implicit grid tracks and the page scrolls sideways.
- The textured button recipe is copied as tokens into each stylesheet that
  owns buttons: `index.html` and `hackathon.css` (`--btn-*`), `styles.css`
  (`--button-*`), `site-header.css` (`--fm-btn-*`), `mesh-page-theme.css`
  (`--mesh-btn-*`) and `blog-social.css` (`--bs-*`), with literal values in
  `blog.html` and `security-report.html`. `www/tests/test_button_texture.py`
  (the shared stylesheets and those two pages), `test_landing_page.py`
  (`index.html`), `test_blog_post_styles.py` (`blog-social.css`) and
  `app/tests/test_shared_header_frontend.py` (`site-header.css`) all check the
  same values, so a copy that drifts fails its own test. `styles.css` and
  `site-header.css` are in `SHARED_FILES`, so App pages that load them (the
  header's Sign Up, `network.html`) pick the texture up at the next App
  deploy. `auth-page-theme.css` is left alone so login and signup keep their
  own buttons; the security report scopes its texture to `#sr-btn` instead.
- The pricing plan cards (`#pricing .pt-head`) stick under the floating pill
  nav at `--pt-sticky-top`. The strip around the pill is transparent, so
  `.pt-head::before` paints the card surface above the sticky row, and
  everything before that row in the card (`.pt-intro`, and the cards grid in
  the compact layout) sits at a higher `z-index` so the mask only covers rows
  scrolling out. No ancestor of the sticky row may get `overflow: hidden` or
  `auto` (`.landing-main` uses `overflow-x: clip`, which is safe); `.pt-body`
  clips its own rows. Below 76rem the full cards stop sticking and the compact
  `.pt-mini-head` of column names sticks instead; below 40rem each row carries
  its own plan labels.
- `styles.css` carries the page kit the redesigned pages share: `--display`
  (ABC Favorit, which replaced Domine for every page title that uses the
  token, App pages included), the `--kit-*` tokens and the `.kit-hero`,
  `.kit-badge`, `.kit-title`, `.kit-lede`, `.kit-btn`, `.kit-card` and
  `.kit-tile` classes. A page opts into the gray Cal.com-style canvas with
  `<body class="kit-page">`. A `.kit-hero` placed in a grid cell needs
  `margin: 0`: its own `margin: 0 auto` otherwise shrinks it to its content
  and centres it in the cell.
- A rule that sets `display` on an element beats the browser's own
  `[hidden] { display: none }`, so a `hidden` attribute stops working on it.
  Elements the scripts hide that way carry an explicit `[hidden]` rule: the
  ledger wallet strip and its "Load older records" `.kit-btn`, the network
  page's folded wallet rows (`.wallet-row[hidden]`), the status page's
  `.status-stats` and `.status-reasons`.
- `homepage-downloads.js` keeps `knownAssets` as `["asset", [label, arch]]`
  entries because the App's status probe reads that list out of the deployed
  script (`_homepage_download_asset_names`); platform names and icons live in
  the separate `platforms` map and `icons` object.

## Recent changes

- 2026-10-13: Blog images under `assets/blog/` re-encoded in place as WebP
  quality 80, method 6, where that saved more than 15% (88 of 98 files, 15.9 MB
  to 10.5 MB); names are unchanged. They were not resized to 1600 px: the 23
  wider covers (1672x941, one 1920x1080) are the posts' `og:image`, whose
  `og:image:width`/`height` tags and `test_blog_frontend.py` pin those sizes,
  and dropping 72 px of width saves little.
- 2026-10-13: `assets/fediverse-banner.png` and the repository's own
  `.forkmesh/banner.png` are the same 1500x500 image, now palette-quantized
  (256 colours, dithered), 0.94 MB to 0.44 MB each. Both stay PNG and stay two
  files: the ActivityPub actor names `/assets/fediverse-banner.png` from the www
  deploy, while `.forkmesh/banner.png` is the one fixed name the repo-banner
  convention reads from git (`COMMITTED_BANNER_NAME`), and neither deploy can
  reach the other's copy.
- 2026-10-13: Unused assets removed to shrink the repo: the three
  `assets/video/3d-*` clips and the night fallback still, `assets/blog/coming-soon.webp`,
  `assets/hackathon/menu-ill.webp` (a byte copy of `ill-lantern.webp`) and
  `assets/desktop-mockup/left-decor.png`. Nothing referenced them by name or
  by a built path.
- 2026-10-13: web fonts are WOFF2 only. Lato (Regular, Semibold, Bold) and
  Domine (Regular, Bold) were converted losslessly from TTF (3.3 MB → 0.72
  MB), and the DejaVuSansMono TTFs and `ABCFavorit-Bold.otf` were dropped in
  favour of their existing, glyph-identical WOFF2 twins. Every browser the
  site supports reads WOFF2, so the TTF/OTF fallbacks in `@font-face` were
  never fetched; no server code (OG cards, PDFs) loads these files. Add new
  web fonts as `.woff2` only.
- 2026-10-10: Plugin-entry gate. Core features became account-synced
  packages that start removed for accounts created on or after 2026-10-11
  ([core-plugins](../marketplace/core-plugins.md) is the home page). Any
  element marked `data-plugin-entry="<package-id>"` is hidden while a
  signed-in account's `/api/plugins/` list lacks that id (since 2026-10-13
  the list leaves out an account package whose places lack `web`, and the
  dashboard Plugins list has a "Show on" Phone / Desktop / Web control per
  installed package in `09-apps.js`, posting `/api/plugins/@account/<id>/places`;
  the Market page says where each account package shows); signed-out visitors
  see everything, and deep links keep working because only entry points are
  hidden. `public/api-client.js` caches the whole list per account in
  localStorage `forkmesh.plugins.v1` (rechecked at most every 5 minutes, on
  `forkmesh:session-restored`, and re-applied on `storage` events for
  another tab) and writes one generated style rule,
  `[data-plugin-entry]:not([data-plugin-entry="<installed id>"])…{display:none!important}`,
  so elements rendered later by script are covered without a DOM walk. The
  API is `ForkMeshAPI.pluginShown(id[, account])`, `rememberPlugins(account, ids)`,
  `rememberPlugin(account, id, installed)` and `refreshPlugins(force)`; the
  Status Page calls (`rememberStatusPage`, `refreshStatusPage`,
  `statusPageShown`, `data-status-entry`, `forkmesh.statusPage.v1`) still
  work and now ride on the same fetch. Tagged here: the site header menu's
  World, Chat, Network and Financial ledger links plus its World and chat
  buttons, the footer's Network, Financial ledger, Community chat and chat
  icon and the World band, and the World item in the home, mobile and
  hackathon menus (`templates/pages/*`, rendered by
  `app/tools/site_templates.py`). The dashboard side is in
  [docs/app/README.md](../app/README.md).
- 2026-10-09: `/mirror` ("Is your repo mirrored?") is the landing page for
  campaign RM1. It shows the **Remote clone URL** path and its limits
  (one-time copy, code only, public sources, 200 MiB and 10 per account, one
  operator), and its links carry no UTM tags so the post's first touch is
  kept. `www/tests/test_mirror_page.py` fails if a limit is dropped or a
  live-sync claim appears
  ([brief](../marketing/campaigns/repo-mirrored-2026-10-09.md)).
- 2026-10-07: Fixes for the Crawler's first-run errors
  ([site-crawler](site-crawler.md), "Fixes after the first run"). The
  `/docs/:slug /docs/:slug/ 308` rule is gone from `public/_redirects`:
  Workers Static Assets apply `_redirects` before matching a file, so it sent
  every top-level `/docs/*.json` to `…json/` and a 404, and
  `html_handling = "auto-trailing-slash"` already adds the slash to docs
  pages (nested docs pages never had the rule).
  `test_docs_json_assets_are_not_redirected` checks that no placeholder rule
  catches a docs JSON file. The docs "Chat" links point at
  `https://app.forkmesh.com/chat`, the docs index's install card at
  `/desktop`, and the docs history page links each commit to the App's
  compare page `compare/<parent>...<sha>`, since the App has no `/commit/`
  route (`docs_history` now reads `%P`; a root commit gets no link). The
  public profile's `dashboard-composer.css`/`.js`,
  `dashboard-plugin-host.js` and `user-status.js` are proxied to the App like
  `dashboard.js` (`APP_SUBRESOURCES` and `run_worker_first`, which now also
  lists `/qr.js`), and
  `test_every_profile_shell_asset_resolves_on_the_marketing_origin` covers
  every script and stylesheet the dashboard shell loads.
- 2026-10-07: Crawler, a special agent, crawls forkmesh.com weekly with
  `tools/site_crawler.py`, files issues for confirmed errors and reports
  ranked improvements. Its first run found 20 docs JSON links that the
  `/docs/:slug` redirect turns into 404s and dead `/#roomchat`,
  `/#membership` and `/#install-banner` anchors ([site-crawler](site-crawler.md)).
- 2026-10-07: "Start your own ForkMesh" (`blog/start-your-own-forkmesh/`)
  announces the white-label start page, `app.forkmesh.com/start`
  ([../INSTANCES.md](../INSTANCES.md)). It is the newest Product updates card,
  an Announcement like the coaster post, so the next changelog run still
  starts its window from "Runs that listen, branches that let go". Its hero is
  a screenshot of the live page (1672x941 lossy WebP), rendered from disk
  because the box's headless browser has no network. Both of its links to the
  page carry `utm_campaign=start_instance`; the campaign brief is
  [../marketing/campaigns/start-instance-2026-10-07.md](../marketing/campaigns/start-instance-2026-10-07.md).
- 2026-10-07: "Ride the ForkMesh coaster" (`blog/forkmesh-coaster/`) is the
  newest Product updates card, an Announcement like the Open Collective post,
  so the next changelog run still starts its window from "Runs that listen,
  branches that let go". Its hero is the operator's screenshot of the World
  ride cropped to 16:9 (1672x941 lossy WebP), restyled with Codex image
  generation (sunset sky, glowing rails, speed streaks, HUD and toolbar
  removed) with `assets/logo.png` composited in the top-left corner. Channel
  drafts are in
  [../marketing/campaigns/world-coaster-2026-10-07.md](../marketing/campaigns/world-coaster-2026-10-07.md).
- 2026-10-07 (issue 1733): forkmesh.com stopped updating after bb2081c122.
  That commit changed the "Native terminal" description without re-rendering
  `blog.html`, and it published "Runs that listen, branches that let go"
  without its hero image. Eleven website deploys in a row failed (`wrangler
  deploy exited 1`), first at the template check in `build_site_assets.py www`,
  then in `build_www_feeds`. `blog.html` is re-rendered, and the hero
  (`assets/blog/features/runs-that-listen-branches-that-let-go.webp`, 1672x941
  lossy WebP) is generated from the post's alt text with the official logo
  and its reflection composited in.
- 2026-10-07: the Actions copy on `features.html`, `about.html` and
  `changelog.html` says what ForkMesh Actions runs: shell-step workflows on
  the owner's own desktop, without `uses:` steps or hosted runners (see
  `server/actions_workflow.go` and `docs/server/actions-runner.md`). The
  "Native terminal" card no longer says workflow output renders in the
  terminal: run logs show in a plain-text log view (`QPlainTextEdit#actionLog`),
  and only agent sessions and the workspace use `TerminalWidget`.
- 2026-10-06 (issue 1490): the website pages are built from templates.
  `www/templates/base.html` holds the shared head, chrome and spider;
  `layouts/blog-post.html` holds 93 of the 94 blog posts; each page is a small
  child template rendered into `www/public/` by `app/tools/site_templates.py`
  ([templates](templates.md)). The Genie spider from the phone and desktop now
  appears on every website page, the docs library, the dashboard (except
  chat) and the App's standalone pages, with tips for new visitors and a
  briefing for signed-in users ([genie-spider](genie-spider.md)).
- 2026-10-04: every logo copy now shows the white-ringed hexagon. The old
  white-cube rasters (`/assets/logo.png`, `logo-96.png`, `logo-mark.webp`,
  favicons, `apple-touch-icon`, `fediverse-avatar.png`, the desktop icons, the
  Android launcher and `fm_logo`, the iOS and watch app icons, the browser
  extension icons, the BIMI SVG and the og-card logo) are re-rendered by the
  new `tools/render_logo.py`, which replaced `native/ios/tools/app_icon.py`.
  Pages that used a raster brand mark use `/assets/logo.svg`, every page
  adds the SVG favicon, the iOS `Logo` asset is the SVG, and `desktop/install.sh`
  installs `hicolor/scalable/apps/forkmesh.svg` ("Logo files"). The World
  grass imprint and the illustrated artwork (`forkmesh-pixel.png`, the World
  table inlay, the Patreon cards) are unchanged.
- 2026-10-04: the logo's top face and its F and M are plain white
  (`#ffffff`) instead of white-to-grey gradients (the M face ran down to
  `#8a8a95`), so the letters keep their contrast against the near-black body
  at small sizes and from a distance. The SVG lost its `<defs>`; all three
  copies (`logo.svg`, `www/public/assets/logo.svg`, `office/public/logo.svg`)
  changed and the root `logo.png` was re-rendered (975x1116, transparent,
  headless Chromium). The press kit hero and a new Logo block at the top of
  `/design` show the one SVG side by side on white and on black. The older
  white-cube rasters (`/assets/logo.png`, `logo-96.png`, `logo-mark.webp`, the
  favicons and the native app logos) were already white and are unchanged,
  as is the grey World grass imprint.
- 2026-10-04: `/design` (forkmesh.com/design) is not in `www/public`: the
  `design-system` Market package contributes it through the `www.page`
  socket, and `stage_www` stages it (page, assets, generated
  `design/inventory.json`, the desktop icons and the Material Icons font,
  about 6 MB) before the canonical links and the sitemap, so both include it.
  A catalog without the package builds a site without `/design`. Package and
  socket: `docs/marketplace/README.md`.
- 2026-10-02: the hexagon logo has a white ring around its near-black body,
  a white copy of the hexagon stroked 26 units wide behind the dark one, so
  8 units of white show past the dark edge. On white it vanishes; on black
  it outlines the cube instead of letting the body sink into the page. The
  viewBox grew to `-104 -119 208 238` (same proportions) to hold it, in all
  three copies: `logo.svg`, `www/public/assets/logo.svg` and
  `office/public/logo.svg`. The root `logo.png` is re-rendered from the SVG
  (975x1116, transparent). The faint grass imprint in World
  (`paintLogoMark` in `world/public/world/scene/town.js`) keeps the plain
  hexagon, since it is a grey luminance texture rather than a mark on black.
- 2026-10-08: the homepage downloads card lists download counts. The App
  had stopped recording downloads when `entry.py` was split into
  `app_runtime.py` (`record_release_download` lost its caller), so
  `/releases/downloads` returned no counts; the release-blob route now records
  each `GET` that the mirror proxy answers with 200.
- 2026-10-02: crawled pages all come from the Worker. `/status`, `/network`
  and `/leaderboards` were App pages that forkmesh.com redirected to the core
  nodes. They are now Worker assets, and the App 308s them back to
  forkmesh.com. 18 hand-written pages (about, terms, privacy, several docs
  pages, one blog post) had no canonical tag and were indexable twice through
  `www.forkmesh.com`; the build now stamps one. The App's production
  `WWW_ORIGIN` (`deploy/fleet.toml`) is `https://forkmesh.com`, so its
  redirects for `/blog`, `/docs` and the marketing pages land on the canonical
  host in one hop instead of on `www.forkmesh.com`.
- 2026-10-01: the homepage downloads card showed macOS twice (the arm64 and
  Intel entries are the same universal dmg, which left a lone card on a second
  row) and headlined "Latest version v1.2.6" while `candidate-1.2.6` held only
  the mirror-node binary, so no v1.2.6 build was downloadable. Same-blob
  assets of one platform now merge into a "Universal" card, and a newer
  candidate is named only once it carries a build the card offers.
- 2026-09-29 (`65df105ee`): `forkmesh-next-www` deleted again; a deploy from
  `www/wrangler.toml` had re-created it on 2026-09-27. `app/deploy.sh www`
  deploys `wrangler.production.toml`, `www/wrangler.toml` lost its name and
  route, and the cutover redirect Worker (`wrangler.redirect.toml`,
  `redirect_worker.js`) was removed ([CUTOVER.md](../CUTOVER.md),
  "forkmesh-next-www deleted again").
- 2026-09-29: generated documentation pages show a unique view count in their
  dateline, fetched by `docs-page.js` from `GET /api/analytics/_/pages`; see
  [../app/analytics.md](../app/analytics.md#docs-view-counter-and-default-install-2026-09-29).
- 2026-09-29: the logo is a vector, `logo.svg` (repo root) and
  `www/public/assets/logo.svg`, drawn to sit on white or black without a
  filter: a near-black (`#131316`) rounded cube body whose light top face
  carries the fork glyph and whose side faces carry the F and M in greys.
  On white the body is the outline; on black it disappears and the mark
  reads like the old white cube. It replaced `/assets/logo.png` wherever
  that PNG sat on a light surface: the homepage, mobile and hackathon nav
  (whose `invert(1) brightness(0.2)` light-mode filter flattened the cube
  into a black blob with grey rims around the fork), the shared footer and
  simple header, and the press kit, which now offers the SVG first and the
  PNG for dark backgrounds. The root `logo.png` (the repository logo that
  repo cards and the desktop read, since they accept only PNG, WebP and
  JPEG) is a transparent render of the SVG instead of a white-on-black
  square; the README shows the SVG. Dark App pages keep `/assets/logo.png`.
- 2026-09-28: forkmesh.com has its own `robots.txt` and a built
  `sitemap.xml` (249 pages, every one checked 200 on the live site), see
  Security. `app/sync_request_block_rules.sh` reads the block table from
  rqlite; it still ran `wrangler d1 execute` against the D1 database deleted
  on 2026-09-27, so a change to the table could not reach the WAF. The live
  WAF rule was checked equal to the table's rows the same day. The docs
  business-model page's canonical URL gained its trailing slash (the bare
  path is a 308).
- 2026-09-28: the "Your mesh, wherever you work" devices show real dark-mode
  product screenshots instead of a black screen: the iPhone Inbox
  (`assets/mobile/inbox.webp`), the desktop app (`desktop-mockup/desktop-app.webp`
  with the macOS menu bar cropped off) and the web repository page
  (app.forkmesh.com/forkmesh/forkmesh captured headless with
  `colorScheme: dark`), resized into `assets/everywhere/` at about 3x their
  on-screen size. Screens sit dimmed until hovered. Hover plays a short boot:
  a dark frosted splash, `logo-mark.webp` (transparent, 14 KB, replacing the
  736 KB `logo.png`) scaling in with a halo, glow and light sweep, then the
  splash fades and the screenshot comes up to full brightness. The packet on
  the connection line no longer fades out at the right end, and every node
  flashes and throws a spark (flash, ring and six rays) as the packet head
  reaches it while the screen below glitches for about 0.4 s (red and cyan
  offset copies of its screenshot in shifting slices, plus a scan bar). All
  of it is CSS kept in step by one cycle length, `--ev-cycle: 6.5s`, and the
  hit times `--ev-hit-1..3` (0, 2.54, 5.09 s), which are the node arc
  lengths along the path (0, 564, 1132 user units) over the 1446-unit dash
  travel. Changing the path, the dash pattern or the cycle means recomputing
  those three times. The glitch is off below 760px, where the line is
  hidden, and sparks, glitch and boot are off under reduced motion.
- 2026-09-27 (`b4c4a1e1a`): the node auto-deploy agent deploys
  `forkmesh-www` when its fingerprint differs from `www/` of the target;
  `www/worker.js` and `www/wrangler.production.toml` became release files.
  The downloads card links held-back assets from earlier signed candidate
  manifests (`earlierAsset`). 1.2.5 shipped without a macOS `.dmg`
  (Codemagic's free minutes were used up), so macOS kept offering v1.2.4
  until the v1.2.5 `.dmg` files were published on 2026-09-28.
- 2026-09-27 (`69abcf13f`): the homepage's "Get the latest verified build"
  card showed "Release verification unavailable" for every visitor, because
  its release reads were credentialed (see Security). It now fetches them with
  `credentials: "omit"`; the signed v1.2.4 manifest verifies and lists Linux,
  Android, Windows and macOS.
- 2026-09-27: `forkmesh-next-www` deleted and `next.forkmesh.com` no longer
  resolves; its settings are exported in
  `~/projects/forkmesh/config/fleet/retired-workers/2026-09-27/`
  ([CUTOVER.md](../CUTOVER.md), "Workers retired"). Homepage links to
  `next.forkmesh.com` now point at forkmesh.com (`9591b6061`).
- 2026-09-27: the unpublished draft "A shorter path to the work" (branch
  `agent/adhoc-2039-marketing-scheduled-work`, 2db782e15) is pruned, not
  published. "The mirror becomes a front door" already covers the same
  evidence window (e0891f901 to 8a366036d) and the same ground: mirror-served
  repository pages, portable login, event-driven sync, Market packages, the web
  task workspace, desktop and mobile polish, v0.7.95 candidate vs v0.7.29
  latest. The draft also describes Worker bindings and the Cloudflare Tunnel
  fallback, which the Next stack no longer runs. The branch is kept.
- 2026-09-27: `analytics.js` reports an unhandled rejection on the next tick
  and skips it when another handler called `preventDefault()` on it (groups
  #115/#130). Test: `app/tests/test_client_error_reporting.py`.
- 2026-09-27: `/about`, `/careers` and `/changelog` on the page kit. About
  left the dark-only Tailwind and `mesh-page-theme.css` setup for
  `styles.css`, so it follows the visitor's light or dark choice; its story
  sections are two-column (heading, then text), the quote is a card and the
  founders are kit cards, on the plain kit canvas (a faint grid behind the
  hero was tried and removed at the user's request). Careers shows the role
  as a card with its two lists side by side and an apply button in the card
  header. The changelog lists each release as a row with the version and date
  on the left (sticky on wide screens) and the notes on the right, with
  neutral check marks instead of the green timeline dots. The shared
  `styles.css` network and status blocks and `static-page.js`'s wallet table
  changed with the App pages
  ([docs/app/public-pages.md](../app/public-pages.md)).
- 2026-09-27: second UI pass from the user's Cal.com references. The green
  kickers ("Distributed Git", "Direct downloads") are gone from the landing
  page. The downloads section shows one card per platform with a dark icon
  tile (Tux, Android, Windows, Apple), format, architecture and file size, a
  textured Download button, a shield status badge and skeleton cards while
  the signature is checked; the visitor's own platform is detected from the
  user agent and marked "This device" with a blue ring and the primary
  button. `/desktop` was rebuilt on the page kit: a left-aligned badge and
  Favorit title, a single install command bar (one `$` prompt; the page wires
  its own copy button instead of `static-page.js`'s `#install-copy`, whose
  text swap would erase the icon), platform cards with one-line download rows
  and the same device highlight, no hover on cards that are not links, and
  numbered setup cards. `/press` and the App's `/leaderboards` hero moved to
  the kit in the "Choose your subscription" layout. Page titles that used
  Domine (careers, changelog, market, about, privacy, terms, 404, the blog
  index and blog posts) now use ABC Favorit. The membership panel
  (`app/public/membership-prompt.js`) became Cal.com-style plan cards: no
  kicker, no green dots and no tier artwork, a founding-goal card, a big
  price, one line on what each level funds, a "Support for $N a month"
  button, and "Everything in X, plus:" lists with neutral check marks; the
  "Most chosen" badge was dropped because no tier has members yet, and a
  zero count reads "Be the first to join". The App's `start.html` launch
  button and `market-reviews.html` buttons use the texture tokens too.
- 2026-09-27: landing-page UI pass after the user's Cal.com references.
  Pricing is one card: heading, sub and billing toggle, then the four plan
  cards as a header row that stays pinned while the feature rows scroll
  (full-bleed rows, a one-line description per feature taken from
  `business-model.json`, check marks, Pro as the inverted "popular" card). It
  is built from `div`s with table roles so the header can stick and reflow;
  the plan names, prices and contract text the tests pin are unchanged.
  Primary and secondary buttons on the landing page, marketing pages, blog
  index, 404, hackathon pages and security report use the reference's
  texture (primary: `#2c2c2c` to `#222222` fill, a `#161616` edge and a 17%
  white inset top highlight; secondary: white with a `#e1e2e3` border),
  inverted in dark mode, and the landing CTAs gained its muted chevron. The
  header's World shortcut shows a globe instead of the green dot; the site
  menu marks the current page in blue (`--fm-header-active`) rather than the
  green accent, Marketplace is no longer highlighted, and Membership and
  Ecosystem got the icon tiles the other links have (without them they read
  as group headings). Blog posts: the "Posted on" and "Share this post"
  blocks became a share card (Copy link, Reddit, Mastodon, X), icon chips for
  where the post was announced, and a "Prefilled drafts" disclosure holding
  the full-length drafts with Copy and Open buttons; reach stats moved to the
  end, previous/next links became cards, and article bodies gained code
  chips, a note accent and list, quote, table and code-block styles.
- 2026-09-27: `/features` redesigned after the Cal.com embed-features section
  the user supplied as a reference. Each category is a centred badge, headline
  and subtitle over a three-column grid of white, 16px-radius cards, each with
  a 48px icon tile (72 purpose-drawn filled glyphs), an 18px title and 16px
  body. Headings use the bundled ABC Favorit and text uses Geist, standing in
  for the reference's Cal Sans and Cal Sans UI, which are not bundled. Light
  body text is `#6f6f6f` rather than the reference's `#898989` so it passes
  WCAG AA both on the white cards (5.0:1) and on the `#f4f4f4` page (4.6:1).
  The category headlines are new copy; each old section title now sits in its
  badge, and every card, tag, link and subtitle is unchanged. The rewards
  custody exception moved into a note under that section's cards.
- 2026-09-27: the ForkMesh Ecosystem, on the landing page and at
  `/ecosystem`, redesigned from the user's Cal.com references. The landing
  section is centred, drops its green kicker and shows eight screw-plate
  tiles that open on hover to show a project's tagline; the directory and
  project pages take the `/features` card language
  ([docs/app/ecosystem.md](../app/ecosystem.md), "Website").
- 2026-09-26: `/ecosystem` page and landing-page section. The Worker
  serves `ecosystem.html` for `/ecosystem/<slug>` rather than redirecting
  the two-segment path to the App as a repository; the section hides
  itself when the API is unavailable or empty
  ([docs/app/ecosystem.md](../app/ecosystem.md)).
- 2026-09-25: forkmesh.com deploys from `main` with
  `www/wrangler.production.toml` (fetching the cores directly, no
  `forkmesh-app` binding); before, it was deployed by hand from a cutover
  branch, so www work merged into `main` did not go live.
- 2026-09-24: blog post "Three small servers and a long September"
  (b836978, 35fe553); cutover redirect Worker (3e432a2).
- 2026-09-23: www reaches the App over `APP_ORIGIN` with a signed edge-client
  header (2ca9dc4).
- 2026-09-23 `12317aa01`: `posthog.js` replaced by `analytics.js` and the
  first-party SDK.
- 2026-09-23 `719089c51`: `/homev2`, `/homepage-update`, `/new-home` and
  their assets removed.
- 2026-09-22 `1dae2af18`: homepage offers the macOS `.dmg` once the signed
  release carries it.
- 2026-09-22 `74f3694a0`: installer refuses user-data wipes from agent
  shells.
- 2026-09-21 `1bce7c529`: docs history page, sortable documents index,
  datelines.
- 2026-09-21 `8c1ba825e`: installer defaults to `~/projects/forkmesh/src`.
- 2026-09-18 `7b0501202`: homepage downloads label each build's version.
- 2026-09-14 `aebd861ba`: a landing-page session the App already revoked
  expires.

## Related docs

- [docs/app/README.md](../app/README.md), [World](../world/README.md)
- [OPERATIONS.md](../OPERATIONS.md), [CUTOVER.md](../CUTOVER.md),
  [auto-deploy](../deploy/auto-deploy.md)
- [auth backdrop](auth-mesh.md), [auth theme](auth-theme.md),
  [mobile page](mobile-page.md)
- `www/docs/README.md` (public library index)
- `www/docs/operations/worker-split.md`
- `docs/attribution.md`
