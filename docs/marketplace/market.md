# The Market: listings, installs, share links and package pages

Part of [Marketplace](README.md). This page covers how packages reach users:
packs and guides, sorting and installing on every Market surface, the public
package pages and share links, the menu on every plugin page, and the preview
images. Ratings and reviews have their own page, [ratings.md](ratings.md);
the package system behind it is [package-system.md](package-system.md).

All packages are currently free (`price.amount: 0`).

## Packs, guides and Market lists

- **Packs** (`marketplace/packs.json`): `id`, `name`, `tagline`, `summary` and
  `packages` (catalog ids, or `"*"` for every installable top-level package
  except agents, themes, catalog-only, World-only and nested packages).
- **Guides** (`marketplace/guides.json`): moving a project from another host,
  with 2-12 `steps`, a `pack` and/or `packages`, and up to six https `links`.
  ForkMesh hosts the repository and runs builds; the guides keep the current
  host serving the site. Both files are validated by
  `marketplace_lib.load_onboarding`, embedded in `marketplace.qrc`, and
  rendered on `/market` between the `market:onboarding` markers.
- **Sorting and stats.** Featured, Name, Top rated, Most installed, Most
  downloaded, Most used. Ratings and usage come from
  `GET /api/market/reviews/summary`; installs and downloads from
  `GET /api/market/installs`, whose rows also carry `accounts` and
  `organizations` counts (`PACKAGE_MEMBER_INSTALL_SQL`). `GET
  /api/market/packages` rows carry `publisher`, `verified` and `free`.
- **Installing.** One click installs a package and its dependencies; a pack or
  guide installs in one batch (`MainWindow::installPackageSet`).
- **Install all.** Every Market surface has an **Install all** button that adds
  everything the device or account can run, with the rules of the
  `whole-cutlery-drawer` pack: top-level packages only (nested ones come with
  their parent), no agents, themes or local AI models (multi-gigabyte
  downloads), and nothing already installed. The label counts what is left and
  reads **All installed** when nothing is. The desktop calls
  `PackageRegistry::installAllPackages` and installs in one batch. The web
  dashboard (Apps) installs the account-synced packages one at a time after a
  confirmation naming the count. The phones do the same for account-synced and
  phone-switched packages, and count organization-scope and desktop-only
  packages as skipped (they need a choice or the desktop). A failure is
  reported and does not stop the rest. forkmesh.com/market links to
  `app.forkmesh.com/dashboard/apps?installAll=1`, which opens that
  confirmation; the other apps are reached from the Market screen.
  `?install=<id>` (from a package page) asks to install that one
  account-synced package, or says it can't be installed there.
- **Category shortcuts.** The desktop Market filters the All list with
  Virtual goods (`virtual.*`), Agents (`agent`), Services (`service.*`) and
  Themes (`theme`) buttons. Clicking the selected button again clears it;
  search terms apply with the selected category.

## Share links and package pages

Every catalog package has one public link, `https://forkmesh.com/market/<id>`
(only the official `forkmesh` market has public pages), and that link is the
package's own page. `build_market_catalog.stage_package_pages`
writes `market/<id>.html` into the staged site at build time (`stage_www` and
`stage_cutover` in `app/tools/build_site_assets.py`), from the manifest, the
package README and `www/templates/layouts/market-package.html`; nothing is
committed, so a manifest or README change reaches the page on the next deploy.
The pages are in the sitemap with that link as canonical.

A page shows the type, shelf, category and price, the live rating and usage
(`www/public/market-package.js`, from the same `/api/market/installs` and
`/api/market/reviews/summary` as the list), an install box chosen by
`install_place` (account-synced packages link to
`app.forkmesh.com/dashboard/apps?install=<id>`; organization, desktop, phone,
editor and World packages say where to install them), Share and Copy link,
screenshots, the README (links resolved to the public repository, headings
shifted one level down), the facts and permissions, the parent, nested,
required and requiring packages, the packs it is in, and up to six others in
its category. Every card on `/market` has the anchor `#package-<id>` and links
to its page.

On forkmesh.com the static-site Worker (`MARKET_PACKAGE` in `www/worker.js`) serves the page asset for
that path, and the `/market` document when no page exists (an id that is not in
the catalog). The App (`static_routes.is_market_package_path`,
`site_document_asset_path`) does the same on a self-hosted instance. The
`/market` script still outlines a card when the document is served for a
package path. `/market/previews/*` stays a static asset path: `previews` is
never read as a package id, and every `run_worker_first` list pairs
`/market/*` with `!/market/previews/*`.

- **forkmesh.com/market:** each card gets **Share** and **Copy link** buttons,
  written by `build_market_catalog.render_share`. Share opens the system
  share sheet through `navigator.share`. Where that isn't available, Share
  copies the link instead.
- **Web dashboard:** the plugin page's options menu (`09-plugin-menu.js`) has
  **Share…** and **Copy link**. Share… shows only when the browser has
  `navigator.share`.
- **Desktop:** the plugin options menu and the rail menu have **Copy link to
  <name>** (`forkmesh::market::packageShareUrl`). Qt has no cross-platform
  share sheet, so the desktop only copies.
- **iPhone and Android:** the package screen's menu has **Share…** (the
  system share sheet) and **Copy link**. The universal link or app link opens
  the package screen, stacked under Market. `/market/previews/...` and
  `forkmesh://market/<filter>` keep their meaning.

## Plugin page menu

Every installed plugin page has a ⋯ button at the top
right on the web, the desktop and both phones, with the same three actions so
a user can manage a plugin without going back to the Market or Apps page:

- **Rate** opens the package's reviews ([ratings.md](ratings.md)).
- **Send feedback or report a bug** posts to `/api/bug-reports` with
  `package: <id>`. The private report and any attachments are visible only to
  platform administrators in the Support inbox and the phones' Bug sheet
  Reported tab (`GET /api/bug-reports/inbox`), labelled `plugin`. A
  signed-in reporter sees their own reports and their status in the Bug
  sheet's My reports tab (`GET /api/bug-reports/mine`) and can confirm one is
  fixed. It works without the `bug-reports` package.
- **Uninstall** uses the surface's normal removal flow, including its
  confirmation and dependents.

On the web, `build_app_plugins._mark_plugin_body` stamps
`data-plugin-package` and `data-plugin-name` on each staged plugin page's
`<body>`, and `dashboard/js/09-plugin-menu.js` shows the header's
`[data-plugin-menu]` only when that stamp is present. Rate links to
`/market-reviews?market=forkmesh&package=<id>`. Feedback reuses the
dashboard's feedback dialog in its per-plugin mode. Uninstall calls
`POST /api/plugins/@account/<id>/uninstall`, then returns to Apps. It is
disabled, with the reason shown, when the account cannot remove the plugin
itself: it is built in, installed by an organization, not installed, or the
user is signed out.

On the desktop the ⋯ (`packageActionsButton`) sits in the window's top bar,
just left of the view switcher, and shows only while a native package page
(or the placeholder of one open in its own window) is the current section
(`MainWindow::updatePackageActionsButton`): package pages draw their own
headers with buttons at the right, which a floating button would cover. A page
in its own window has no top bar, so `openNativePackageWindow` adds a slim
`nativePackageWindowHeader` row for the button. The menu comes from `buildPackageActionsMenu`:
Rate opens `/market-reviews?market=<marketplaceId>&package=<id>` on the
active host in the browser (the desktop usually has no session to post a
review with). Feedback opens `PackageFeedbackDialog`
(`desktop/src/PackageFeedback.{h,cpp}`), which posts anonymously, or with the
session bearer when there is one, with `source: "desktop"`; the payload and
the reply wording are pure helpers tested in `forkmesh-native-package-tests`.
Uninstall is `requestPackageRemoval(id, true)`, so it always asks first, and
it is disabled for `managedExternally` packages as on the rail. The rail's
right-click menu offers the same Rate and feedback items
(`addPackageFeedbackActions`), covering built-in packages with no native
page. The rail menu, the ⋯ menu and a plugin's icon on Home share one
**Uninstall <name>…** item (`addPackageUninstallAction`).

## Preview images

- **Website.** `app/tools/build_market_catalog.py` points each card's
  screenshot at `/market/previews/packages/<package>/preview/<file>`, and the
  www build (`stage_market_previews` in `app/tools/build_site_assets.py`)
  copies every `marketplace/packages/*/preview/*.{png,svg,webp,jpg,jpeg}`
  there, served with its image type. The raw repository API serves SVG as an
  attachment with `nosniff` and must not be used for images
  (`app/tests/test_market_previews_static.py`).
- **Desktop.** `MainWindow::marketPreviewPixmap` reads each preview once with
  `QImageReader::setScaledSize` at the 240×135 card size and caches it in
  `m_marketPreviewCache` (keyed by path, box and file time), so opening the
  Market never re-renders SVGs on the GUI thread. The first read runs on a
  worker (`QtConcurrent`): the card shows an empty frame, and
  `applyMarketPreview` caches the image and queues it; `flushMarketPreviews`
  fills every card and details pane whose `previewKey` matches, for all the
  previews that landed in the last 50 ms, in one walk of the window. A
  card's details pane is built the first time the card opens
  ([diagnostics.md](../desktop/diagnostics.md), report #18) and copies the
  card's preview then (decoding inline cost ~4 s on the first Market open
  under memory pressure, report #15). WebP needs Qt's optional
  qtimageformats plugin, which many Linux installs lack, so when Qt cannot
  read a preview whose bytes are WebP the worker decodes it with the libwebp
  the app already links (`diffDecodeWebp`, `FORKMESH_HAVE_WEBP`), reading at
  most 16 MB; seven packages (App Store Status, RSS Reader, Code Map, Size
  Map, Strata, Ember, Synapse) ship only `.webp` previews. PNG and SVG need no
  plugin.
- **Phones.** `GET /api/market/packages` rows carry `screenshots: [{url,
  alt}]`, up to eight absolute `https://forkmesh.com/market/previews/...`
  URLs; a listed `.svg` is sent as its `.webp` sibling. Every SVG preview has a
  committed WebP made by `python3 app/tools/rasterize_market_previews.py`
  (headless Chromium, `FORKMESH_CHROME`; `--force` re-renders);
  `test_every_svg_preview_has_a_webp_for_phones` enforces it. Phones keep only
  `https` URLs; Android uses `MarketImages` with a 32 MB LRU cache.

