# Marketplace packages

The Marketplace is how optional features ship in ForkMesh. Every package lives
in `marketplace/packages/<id>/` and is listed in `marketplace/catalog.json`.
The same package can contribute a native page to the desktop app, a page and a
Worker API to the web app (`app/`), a module to World (`world/`), and a screen
to the mobile app. Hosts only know generic sockets; no core file names a
package. The user-facing contract (package layout, lifecycle, publishing) is in
[`marketplace/README.md`](../../marketplace/README.md); this page is the
developer home for how the pieces fit.

## Code map

| Path | Responsibility |
| --- | --- |
| `marketplace/catalog.json` | The catalog (`schemaVersion: 2`): id, version and manifest path of every package. |
| `marketplace/packs.json`, `marketplace/guides.json` | Packs (several packages installed together) and onboarding guides (moving a site from another host). Validated by `marketplace_lib.load_onboarding`; see [Packs and guides](#packs-guides-and-market-lists). |
| `marketplace/catalog.schema.json`, `marketplace/manifest.schema.json`, `marketplace/agent.schema.json` | Schemas for the catalog, package manifests and agent descriptors (`gemma-4`). |
| `marketplace/tools/marketplace_lib.py` | Loads and validates catalogs and manifests; shared by every generator. |
| `marketplace/tools/marketctl.py` | CLI: `validate`, `pack`, `sync`, `install`, `uninstall`. |
| `marketplace/tools/plugin_sockets.py` | Discovers `contributions` per socket. |
| `marketplace/tools/generate_desktop_packages.py` | Desktop: turns `content/desktop/build.json` descriptors into `marketplace.qrc`, `packages.cmake` and `DesktopPackageFactories.cpp` under the build dir. Run by `desktop/CMakeLists.txt` at configure time. |
| `marketplace/tools/generate_world_packages.py` | World: writes `world/public/world/world-marketplace-packages.js`. |
| `app/tools/build_app_plugins.py` | App: stages `dashboard.page` contributions into `app/dist` and package Worker modules into `app/src_build` (both ignored). Called from `app/tools/build_site_assets.py` and `app/tools/build_worker_python.py`. |
| `app/tools/build_market_catalog.py` | Rewrites the tracked `www/public/market.html` landing page from the catalog. |
| `app/src/plugin_data.py`, `app/src/plugin_registry.py` | Organization install/uninstall engine, account install state for `install.sync: "account"` packages (`/api/plugins/@account`, dependencies, declared settings), reminder and account-purge hooks. |
| `app/src/entry.py` (`_PluginApiRuntime`) | The least-privilege runtime handed to a package's Worker module. |
| `app/src/marketplace_runtime.py`, `app/src/marketplace_governance.py` | `/market` publication review and the public catalog route. |
| `desktop/src/NativePackageExtension.h` | The desktop socket: `NativePackageHost`, `NativePackageWidget`, `NativePackageRegistration`. |
| `desktop/src/ForkMeshPackageRegistry.{h,cpp}`, `desktop/src/MainWindowMarket.cpp` | Desktop install state, Market page, rail buttons, package data export/import. |
| `desktop/src/MainWindowSearch.cpp` | Top search box: asks every installed package for hits. |
| `marketplace/tests/test_marketplace_lifecycle.py`, `marketplace/packages/*/tests/` | Lifecycle tests and per-package suites. |

## Architecture

**Discovery.** `catalog.json` is the only list of packages; every generator
reads it through `marketplace_lib.load_catalog`, which checks each row against
its manifest's id and version.

**Sockets.** A manifest's `contributions` name a host socket, an entrypoint and
socket config:

- `dashboard.page` (App): an HTML view plus optional script calling
  `window.ForkMeshDashboard.registerPage(pageId, initializer)`. Config
  `devices: ["camera", "microphone"]` (either or both) lets that route alone
  use them: `build_app_plugins.py` writes `PLUGIN_DEVICE_PAGES` into the
  staged `plugin_registry`, and `_apply_browser_security_headers`
  (`app/src/entry.py`) answers those routes with `camera=(self)` when asked
  and `mediastream:` in `media-src`. Every other page keeps `camera=()`.
- `www.page` (website): a public one-segment route on forkmesh.com.
  `build_app_plugins.stage_www_pages` copies the page and assets, runs an
  optional `builder`, content-hashes `?v=` references, refuses routes already
  served or reserved, and exports `PLUGIN_WWW_SITE_ROUTES` so the App host
  308s the path to the website.
- `worker.http` (App), or more commonly a top-level `worker` block (`module`,
  `api`, `outboundOrigins`) mounted at its `api` prefix.
- Desktop native pages: entrypoint `builtin:<id>`; the package's C++ registers
  a `NativePackageContribution` with a static `NativePackageRegistration`. A
  contribution with only `browserPath` is a rail link that opens that path on
  the active host (`MainWindow::openNativePackageLink`).
- World: `virtual.world` and `virtual.object` packages export an
  `install(host)` module.

Only `install.bundled: true` packages are activated by the App host.
`install.default` decides whether a desktop installs a package on first run
and whether the web lists it in the Plugins column; it never grants server
code anything.

**Desktop native page contract** (`desktop/src/NativePackageExtension.h`).
`NativePackageHost` provides manifest metadata, a signed `request` limited to
the package's API prefix, the window's shared `QNetworkAccessManager` (so the
desktop firewall sees every destination), `searchIndexChanged`,
`navigationBadgeChanged`, `promptTargetChanged`, `userDirectory` (cached
account names), `accountName`, `openDirectMessage(name)`,
`openUserProfile(name)`, administrator-only `userRecords(done)`,
`askAgent(prompt, done)` (one read-only, low-effort run of the default agent
CLI, never listed in Agents) and, for packages declaring `desktop.agents.run`,
`specialAgents()` / `specialAgentAction(...)`, and for packages declaring
`repository.read`, `repositories()`, `currentRepository()` and
`openRepositoryFile(repository, path, line)`
([codebase-analysis.md](codebase-analysis.md#host-contract)). `request` waits out the
network manager's host-wide cooldown (after a 5xx or 429 on any `/api/`
route) and resends GETs up to four times before reporting "the ForkMesh relay
is restarting or unreachable".

A `NativePackageWidget` implements `refresh()` (load on open and on Refresh,
no timers), `navigationBadge()` / `prepareNavigationBadge()`,
`setHostSearchQuery()`, `searchHits()` / `activateSearchHit()` /
`prepareSearch()` (answered from memory on the GUI thread), `exportData()`
(offered on removal), `hostPromptTarget()` / `submitHostPrompt()` (the page
takes over the footer prompt box while visible), `userDirectoryChanged()`
and `repositoryChanged()` (the user opened another repository).

**Own window.** "Open in new window" on a rail button (or dragging it out)
moves the page widget into a top-level window
(`MainWindow::openNativePackageWindow`); a placeholder keeps its stack index,
and closing the window docks it back (`dockNativePackageWindow`). Geometry is
kept under `market/windows/<id>/geometry`.

**Plugin page menu.** Every installed plugin page has a ⋯ button at the top
right on the web, the desktop and both phones. Its menu always holds the same
three actions, so a user never has to go back to the Market or Apps page to
manage the plugin they are looking at:

- **Rate** opens the package's reviews (`docs/MARKETPLACE_RATINGS.md`).
- **Send feedback or report a bug** posts to `/api/bug-reports` with
  `package: <id>`. The private report and any attachments are visible only to
  platform administrators in the Support inbox and in the Bug sheet's
  Reported tab on the phones (`GET /api/bug-reports/inbox`), and are
  labelled `plugin`.
  It does not depend on the `bug-reports` package being
  installed.
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
(`MainWindow::updatePackageActionsButton`). Package pages draw their own
headers with their own buttons at the right, so a button floated over the page
would cover them, and wrapping every page in a host header would change what
the section stack holds. A page in its own window has no top bar, so
`openNativePackageWindow` adds a slim `nativePackageWindowHeader` row above
the page for the same button. The menu comes from `buildPackageActionsMenu`:
Rate opens `/market-reviews?market=<marketplaceId>&package=<id>` on the
active host in the browser, because the desktop usually has no account session
to post a review with. Feedback opens `PackageFeedbackDialog`
(`desktop/src/PackageFeedback.{h,cpp}`), which posts anonymously, or with the
session bearer when there is one, with `source: "desktop"`; the payload and
the reply wording are pure helpers tested in `forkmesh-native-package-tests`.
Uninstall is `requestPackageRemoval(id, true)`, so it always asks first, and
it is disabled for `managedExternally` packages as on the rail. The rail's
right-click menu offers the same Rate and feedback items
(`addPackageFeedbackActions`), which also covers built-in packages that have
no native page. The rail menu, the ⋯ menu and a plugin's icon on Home all add
the same **Uninstall <name>…** item (`addPackageUninstallAction`), so the
wording and the confirmation match wherever it is removed from.

**Worker runtime.** A package module receives `_PluginApiRuntime`
(`app/src/entry.py`): `session`, `json_body`, `organization`,
`plugin_installed`, `notify`, `new_secret`, `seal_with` / `open_with`,
`outbound_text` / `outbound_json` (limited to `outboundOrigins`; returns the
`Link` header as `link`), `request_origin`, `ai_json` (bounded Workers AI) and
mail sending. Desktop callers sign with a per-package `REQUEST_PROOF` string.
`notify(recipient, "plugin_activity", title, body, href, dedupe, meta, push)`
files a ping; `push=False` updates the inbox row without a phone banner, which
the CRM and Marketing activity pings use to fold a burst into one ring.

**Data lifecycle.** `dataLifecycle.scope` is `organization` (installs go
through `app/src/plugin_data.py`; the module provides `DATA_SCHEMA_UP`,
`DATA_SCHEMA_DOWN`, `org_has_data`, `export_org_data`, `purge_org_data`),
`account` (each account installs for itself; account deletion calls
`purge_account_data`) or `none`. `localData: true` keeps data on the device
and offers a JSON bundle on removal. Packages may export
`deliver_reminders(runtime)` for the App's maintenance phase, and
organization-data packages may export `record_lead(runtime, org, lead)` to
receive leads from `plugin_data.deliver_lead` (the CRM does, for the
affiliate program).

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
- **Category shortcuts.** The desktop Market filters the All list with
  Virtual goods (`virtual.*`), Agents (`agent`), Services (`service.*`) and
  Themes (`theme`) buttons. Clicking the selected button again clears it;
  search terms apply with the selected category.

## Share links

Every catalog package has one public link, `https://forkmesh.com/market/<id>`
(only the official `forkmesh` market has public pages). There is no separate
page per package. The www Worker (`MARKET_PACKAGE` in `www/worker.js`) and the
App (`static_routes.is_market_package_path`) serve the `/market` document for
that path. The page script then outlines the package's card, scrolls to it and
puts its name in the title. `/market/previews/*` stays a static asset path:
`previews` is never read as a package id, and every `run_worker_first` list
pairs `/market/*` with `!/market/previews/*`.

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
  `forkmesh://market/<filter>` keep their old meaning.

## Packages

Every package lives in `marketplace/packages/<id>/` with its own README. Groups:

- **Core features as plugins** (account-synced on/off switches for built-in
  features): see [core-plugins.md](core-plugins.md).
- **Productivity and data:** `notes`, `tasks`, `calendar`, `timers`, `crm`,
  `competitor`, `ledger`, `vault` (end-to-end encrypted, suite `fmv1`),
  `secure-drive` (an end-to-end encrypted shared drive kept as one file on a
  router's USB drive or any network share, suite `fmsd1`, on the desktop,
  iPhone and Android; [secure-drive.md](secure-drive.md)),
  `rss-reader` (a local-only feed reader on iPhone, Android and the desktop,
  [rss-reader.md](rss-reader.md)),
  `docs` (the `www/docs` library on desktop and phones), `workflows` with its
  nested `workflows-*` packs ([../app/workflows.md](../app/workflows.md)).
- **Code analysis:** `code-map`, `size-map` (built-in features as
  switches), `strata` (architecture and complexity), `ember` (file-level
  hotspots) and `synapse` (the import graph as a 3D brain), sharing the
  engine in `desktop/src/RepoInsight`; the last three also run on iPhone and
  Android through ports of that engine (`FMCodeInsight`, core `codeinsight`);
  see [codebase-analysis.md](codebase-analysis.md).
- **Health:** `calories`, `steps`, `water`, `sleep`, `sugar-break`,
  `stretch`, `food-guide` (account-scoped, sealed per account).
- **Operations:** `analytics`, `marketing`, `status-page` and its
  `status-check-*` packages ([status-page.md](status-page.md)), `sentry`,
  `app-store-status` ([app-store-status.md](app-store-status.md)),
  `backups`, `cleanup` ([cleanup.md](cleanup.md)), `computer-security`
  ([computer-security.md](computer-security.md)), `phone-security`, its
  iPhone and Android counterpart ([phone-security.md](phone-security.md)).
- **Home network:** `openwrt` (manage an OpenWrt router and its USB storage on
  the user's LAN from the desktop, iPhone and Android over its ubus JSON-RPC
  endpoint; [openwrt.md](openwrt.md)), and `files` (a file explorer for each
  device's files and SMB network shares, such as the router's shared disk, on
  the desktop, iPhone and Android; [files.md](files.md)).
- **Desktop and phone extras:** `capture` (on the iPhone, long-press a blank
  area to select, mark up and copy part of the screen;
  [../native/CAPTURE_MARKUP.md](../native/CAPTURE_MARKUP.md)), `discord`,
  `browser-extension`, `design-system`, `sign-in`, `speech`, `glasses-alert`,
  `eye-control` (iPhone, experimental: gaze cursor and blink to tap;
  [eye-control.md](eye-control.md)),
  `tour`, `people` ([people.md](people.md)), `aquarium`
  ([aquarium.md](aquarium.md)), `spider-room` (the Genie spider in the room
  in augmented reality on the iPhone; [spider-room.md](spider-room.md)),
  `winamp` (theme), `gemma-4` (agent model).
- **Face and voice:** `face-changer` (web page, a desktop rail link, and a
  native Face Changer screen in the phone apps whose face, background and voice
  choices video calls use) and
  `voice-changer` (desktop virtual microphone plus a web page). Both web pages
  run the call engine from `app/public` and share `app/public/call-studio.js`
  ([../app/chat-direct-calls.md](../app/chat-direct-calls.md#plug-ins)).
- **Video mixing:** `video-mix-dj-studio` (two YouTube decks with local cue,
  loop, tempo and crossfader controls on responsive web, mobile and desktop
  surfaces; [details](video-mix-dj-studio.md), iOS performance checks in
  [video-mix-dj-studio-performance.md](video-mix-dj-studio-performance.md)).
- **World:** `forkmesh-sky-office`, `reddit-banner-board`, `x-banner-board`.
- **Catalog only:** `forkmesh-ide` (VS Code extension source).

All packages are currently free (`price.amount: 0`).

## Protocols and interfaces

- Package API routes: `/api/<package>` (from the manifest `worker.api`), or
  `/api/plugins/<package-id>` for `worker.http` contributions.
- Organization lifecycle: `/api/plugins/...` handled by `app/src/plugin_data.py`;
  desktop paths come from `nativePackageLifecyclePath()`.
- Public catalog: `/api/market/catalog.json`; landing page `/market`
  (`www/public/market.html`).
- First-party package list for the phone apps: `GET /api/market/packages`
  (`app/src/market_packages.py`). The build bakes the rows into the staged
  `plugin_registry.MARKET_PACKAGES`, because the core nodes ship
  `app/src_build` and not `marketplace/`.
- Archive format: `.fmpkg` (ZIP), see `marketplace/README.md`.
- Repository content: a package can ship files for a repository's
  `.forkmesh` folder with the `content.action`, `content.shortcut`,
  `content.skill` and `content.teammate` sockets. Each contribution names its
  entrypoint file and `config: {path, name}`; the path must be one the
  Workflows file editor accepts for that kind (`workflow_files.kind_for_path`).
  `build_app_plugins.content_files` stages the text into
  `plugin_registry.PLUGINS[id]["files"]` (at most 32 files, 256 KB each), and
  `GET /api/workflows/package-files` lists the ones from packages installed
  for the account. Workflows' Repository view adds a file as one commit after
  the user reviews it; nothing is written to a repository by installing a
  package.

## Configuration

- Manifest fields that matter to hosts: `entrypoints`, `contributions`,
  `worker`, `permissions`, `install.bundled`, `install.default`,
  `install.sync`, `install.installedBefore`, `dependencies`, `settings`,
  `dataLifecycle`, `lifecycle.migrations`. `install.installedBefore`
  (`YYYY-MM-DD`, needs `install.default: false`) installs the package for
  projects and accounts created before that date
  ([core-plugins.md](core-plugins.md)). The last three before `dataLifecycle` are described
  in [`marketplace/README.md`](../../marketplace/README.md#dependencies-account-installs-and-settings)
  and [status-page.md](status-page.md).
- Desktop build descriptor: `content/desktop/build.json` (`schemaVersion: 1`,
  `sources`, `mocHeaders`, `testSources`, `testLibraries`,
  `testHostSources`). Allowed test libraries are listed in `TEST_LIBRARIES`
  in `generate_desktop_packages.py`. `testHostSources` names bare files in
  `desktop/src` that the package test links (for example `AgentSonifier.cpp`),
  because `testSources` may not leave the package's own directory; the
  package's app sources reach core headers through the app's include path.
  `repositoryResources` names repository folders (relative to the repository
  root, never outside it) whose files the build embeds as
  `:/forkmesh-market/packages/<id>/repository/<folder>/...`; the generator
  also emits `FORKMESH_MARKETPLACE_RESOURCE_GLOBS` so a new file in such a
  folder re-runs package discovery. The Docs package uses it for `www/docs`,
  because a copy of the library inside the package would drift from the web.
- Marketplace sources for `marketctl sync`: see
  `marketplace/sources.example.json`.

## Build, run, test

```sh
python3 marketplace/tools/marketctl.py validate marketplace/catalog.json
python3 marketplace/tools/marketctl.py validate marketplace/packages/<id>
python3 -m pytest marketplace/tests marketplace/packages/<id>/tests
python3 -m pytest app/tests/test_plugin_sockets.py app/tests/test_market_landing.py
python3 app/tools/build_market_catalog.py        # after any catalog/manifest change
```

A desktop configure (`cmake`) reruns `generate_desktop_packages.py`; a broken
catalog fails the configure with "Marketplace package discovery failed".

## Operations

- Releasing a package version: bump `manifest.json` `version`, the matching row
  in `catalog.json`, then run `app/tools/build_market_catalog.py`. A manifest
  bumped without its catalog row passes pytest but fails every desktop
  configure.
- Package Worker modules deploy with the App (`app/deploy.sh`); package web
  pages with the App's static assets.

## Security

- Desktop package network calls must use `NativePackageHost::network` so the
  firewall sees them.
- Worker modules get only `_PluginApiRuntime`; outbound fetches are limited to
  the manifest's `outboundOrigins`. The fetch uses `redirect: "manual"`; a GET
  follows up to 3 redirects (301/302/303/307/308) itself, and only while every
  `Location` stays `https` on an allowed origin. A redirect anywhere else, a
  fourth hop, or any redirect on a non-GET answers `outbound_redirect_rejected`.
  `authorization` is dropped when a redirect changes host.
- Account-scoped data is sealed per account (`seal_with`/`open_with`, key
  wrapped under the host `DATA_KEY`); the vault server never holds a key.
- Permissions are declared in the manifest and shown at install.

## Traps

- `manifest.summary` must be 1 to 240 characters and permission labels at most
  120 (`marketplace/tools/marketplace_lib.py`). A violation surfaces as a pytest
  collection error, not a failed assertion.
- Catalog/manifest version drift breaks the desktop configure, not pytest.
- `www/public/market.html` is tracked and generated; `app/tests/test_market_landing.py`
  fails when it disagrees with the catalog.
- A Q_OBJECT header in a package must be listed in `mocHeaders` of its
  `build.json`, or it links without its meta-object code.
- Pages must not poll. Load on open and on Refresh.

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
  Market never re-renders SVGs on the GUI thread. A package whose manifest
  lists `preview/market.png` keeps PNG, because WebP needs Qt's optional
  qtimageformats plugin.
- **Phones.** `GET /api/market/packages` rows carry `screenshots: [{url,
  alt}]`, up to eight absolute `https://forkmesh.com/market/previews/...`
  URLs; a listed `.svg` is sent as its `.webp` sibling. Every SVG preview has a
  committed WebP made by `python3 app/tools/rasterize_market_previews.py`
  (headless Chromium, `FORKMESH_CHROME`; `--force` re-renders);
  `test_every_svg_preview_has_a_webp_for_phones` enforces it. Phones keep only
  `https` URLs; Android uses `MarketImages` with a 32 MB LRU cache.

## Related docs

- [`marketplace/README.md`](../../marketplace/README.md): package format, lifecycle, publishing.
- [`www/docs/plugins/README.md`](../../www/docs/plugins/README.md): planned plugins.
- [core-plugins.md](core-plugins.md), [status-page.md](status-page.md), [app-store-status.md](app-store-status.md), [people.md](people.md), [aquarium.md](aquarium.md), [spider-room.md](spider-room.md), [cleanup.md](cleanup.md), [computer-security.md](computer-security.md), [phone-security.md](phone-security.md), [eye-control.md](eye-control.md), [openwrt.md](openwrt.md), [files.md](files.md).
- [`docs/desktop/README.md`](../desktop/README.md), [`docs/app/README.md`](../app/README.md), [`docs/world/README.md`](../world/README.md).
