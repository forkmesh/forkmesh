# How Market packages work

Part of [Marketplace](README.md). This is the developer reference for the
package system: where packages live, the host sockets they plug into, the
desktop page contract, the server module runtime, the manifest fields hosts
read, and how to build, test and release a package. The user-facing contract
(package layout, lifecycle, publishing) is in
[`marketplace/README.md`](../../marketplace/README.md); which plugins a build
contains is in [plugins.md](plugins.md); the Market surfaces are in
[market.md](market.md).

Every package lives in `marketplace/packages/<id>/` with its own README and is
listed in `marketplace/catalog.json`. The same package can contribute a native
page to the desktop app, a page and a server module (the manifest's `worker`
block, Python run by the App on the core nodes) to the web app (`app/`), a
module to World (`world/`), and a screen to the phone apps. Hosts only know
generic sockets; no core file names a package.

## Code map

| Path | Responsibility |
| --- | --- |
| `marketplace/catalog.json` | The catalog (`schemaVersion: 2`): id, version and manifest path of every package. |
| `marketplace/packs.json`, `marketplace/guides.json` | Packs (several packages installed together) and onboarding guides (moving a site from another host). Validated by `marketplace_lib.load_onboarding`; see [Packs and guides](market.md#packs-guides-and-market-lists). |
| `marketplace/catalog.schema.json`, `marketplace/manifest.schema.json`, `marketplace/agent.schema.json` | Schemas for the catalog, package manifests and agent descriptors (`gemma-4`). |
| `marketplace/tools/marketplace_lib.py` | Loads and validates catalogs and manifests; shared by every generator. |
| `marketplace/tools/marketctl.py` | CLI: `validate`, `pack`, `sync`, `install`, `uninstall`. |
| `marketplace/tools/plugin_sockets.py` | Discovers `contributions` per socket. |
| `marketplace/plugins.json` | The plugins a build adds to the core; see [plugins.md](plugins.md). |
| `marketplace/tools/generate_desktop_packages.py` | Desktop: turns the listed plugins' `content/desktop/build.json` descriptors into `marketplace.qrc`, one `plugins/<id>.qrc` per module plugin, `packages.cmake` and the build's own `catalog.json`, `packs.json` and `guides.json` under the build dir. Run by `desktop/CMakeLists.txt` at configure time with `FORKMESH_PLUGINS`. |
| `marketplace/tools/generate_world_packages.py` | World: writes `world/public/world/world-marketplace-packages.js`. |
| `app/tools/build_app_plugins.py` | App: stages the listed plugins' `dashboard.page` contributions into `app/dist` and their server modules into `app/src_build/plugins` (both ignored). Called from `app/tools/build_site_assets.py` and `app/tools/build_worker_python.py`. |
| `app/tools/build_market_catalog.py` | Rewrites the tracked `www/public/market.html` landing page from the catalog, and stages one page per package at `/market/<id>` at build time ([Share links and package pages](market.md#share-links-and-package-pages)). |
| `www/templates/layouts/market-package.html`, `www/public/market-package.{css,js}` | The package page layout, its styles, and its rating, usage and share script. |
| `app/src/plugin_data.py`, `app/src/plugin_registry.py` | Organization install/uninstall engine, account install state for `install.sync: "account"` packages (`/api/plugins/@account`, dependencies, declared settings), reminder and account-purge hooks. |
| `app/src/entry.py` (`_PluginApiRuntime`) | The least-privilege runtime handed to a package's server module. |
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

## Desktop page contract

Declared in `desktop/src/NativePackageExtension.h`. `NativePackageHost`
gives every package manifest metadata, a signed `request` limited to the
package's API prefix, the window's shared `QNetworkAccessManager` (so the
desktop firewall sees every destination), `searchIndexChanged`,
`navigationBadgeChanged`, `promptTargetChanged`, `userDirectory` (cached
account names), `accountName`, `openDirectMessage(name)`,
`openUserProfile(name)`, administrator-only `userRecords(done)` and
`askAgent(prompt, done)` (one read-only, low-effort run of the default agent
CLI, never listed in Agents).

Every package also gets two calls with no permission: `openWebPage(path)`
opens a same-origin path on the active host in the browser (credentials
dropped, another origin or a `..` segment refused; `nativePackageWebPageUrl`,
used by the [Runners](runners.md) page), and `publicPageUrl(path)` returns the
same path as a URL to share: `https://forkmesh.com` for the official hosts,
the active host otherwise (`nativePackagePublicPageUrl`, used by
[Feed](instance-feed.md)).

`request` waits out the network manager's host-wide cooldown (after a 5xx or
429 on any `/api/` route) and resends GETs up to four times before reporting
"the ForkMesh relay is restarting or unreachable".

Calls a package gets only when its manifest declares the permission:

| Permission | Host calls | What for |
| --- | --- | --- |
| `desktop.agents.run` | `specialAgents()`, `specialAgentAction(...)` | Special agents |
| `repository.read` | `repositories()`, `currentRepository()`, `openRepositoryFile(repository, path, line)` | Code analysis ([codebase-analysis.md](codebase-analysis.md#host-contract)) |
| `desktop.models.usage.read` | `agentUsagePage(parent)` | The window's own Usage page with this desktop's Claude Code and Codex accounts and their controls, which the [Usage](usage.md) package embeds ([../desktop/agent-usage.md](../desktop/agent-usage.md)) |
| `desktop.logins.use` | `loginStoreName()`, `saveLogin(login, asker, done)`, `findLogins(url, asker, done)` | See [Login store](#login-store) |
| `social.mastodon.signin` | `mastodonSignIn()` | The Mastodon package's active account (`instance`, `acct`, `token`), empty when that package is not installed here or has no sign-in; the [CRM](../../marketplace/packages/crm/README.md#mastodon-plugin-sign-in) follows contacts with it |
| `desktop.workflows.run` | `repositoryWorkflows()`, `repositoryWorkflowAction(repo, id, action, payload, done)` | The workflows in the desktop's repositories (special agents among them), run, edited and pinned to a rail on this desktop ([../app/workflows.md](../app/workflows.md#desktop-workflows-and-special-agents)); the repository key `@account/workflows` pins the account's own workflows (`pin`, `unpin`, `sync`) |

A `NativePackageWidget` implements `refresh()` (load on open and on Refresh,
no timers), `navigationBadge()` / `prepareNavigationBadge()`,
`setHostSearchQuery()`, `searchHits()` / `activateSearchHit()` /
`prepareSearch()` (answered from memory on the GUI thread), `exportData()`
(offered on removal), `hostPromptTarget()` / `submitHostPrompt()` (the page
takes over the footer prompt box while visible), `userDirectoryChanged()`,
`repositoryChanged()` (the user opened another repository) and
`repositoryWorkflowsChanged()` (a desktop workflow was saved, ran or was
pinned).

### Reading another package's API

`request` only reaches the package's own
`apiPrefix`. A page that needs one more API lists its prefix in
`accountPaths` and its request proof in `resourceProofs`, then signs that
request itself, as Calendar does and as the CRM contact card does for
[Invoicing](invoices.md#crm-links). There is no host call for one package to
open another package's desktop page; link to the web page instead.

### Login store

A package can hand a password it was given to the user's
password manager and ask for it back, without the host knowing which package
that is. The store is the first installed page whose `storesLogins()` is true
and whose manifest declares `desktop.logins.store` (today the Password Vault);
only packages declaring `desktop.logins.use` (today the Router) get
`saveLogin` and `findLogins`, and `loginStoreName()` is empty when there is no
store. The store answers with a `NativePackageLoginReply` (`ok`, `cancelled`,
a readable `error`, `logins`); it may show its own unlock prompt over the
`asker` widget, so the master password never passes through the host or the
asking package. Logins are matched by origin (scheme, host and port), and a
save reply carries no password. The Vault side is described in
[the Vault README](../../marketplace/packages/vault/README.md#on-the-desktop),
the Router side in [openwrt.md](openwrt.md#safety).

### Own window

"Open in new window" on a rail button (or dragging it out)
moves the page widget into a top-level window
(`MainWindow::openNativePackageWindow`); a placeholder keeps its stack index,
and closing the window docks it back (`dockNativePackageWindow`). Geometry is
kept under `market/windows/<id>/geometry`.

## Server module runtime

A package module receives `_PluginApiRuntime`
(`app/src/entry.py`): `session`, `json_body`, `organization`,
`plugin_installed`, `notify`, `new_secret`, `seal_with` / `open_with`,
`outbound_text` / `outbound_json` (limited to `outboundOrigins`; returns the
`Link` header as `link`), `request_origin`, `ai_json` (bounded Workers AI) and
mail sending. Desktop callers sign with a per-package `REQUEST_PROOF` string.
`notify(recipient, "plugin_activity", title, body, href, dedupe, meta, push)`
files a ping; `push=False` updates the inbox row without a phone banner, which
the CRM and Marketing activity pings use to fold a burst into one ring.

### Organization roles

`organization(name, account_bi)` returns the
caller's role only for organization insiders (owner, admin, maintainer,
member, auditor); a guest gets the same empty role as an outsider, so every
package that checks `if not role` fails closed. `organization_member_emails`
answers insiders only. The CRM, Marketing, Analytics and Competitor packages
also refuse guests themselves and answer auditors' writes with 403
`read_only_role` (Analytics still lets them run read-only `query` requests).
Guests are outside collaborators added for a few repositories and must not
read an organization's customer, marketing or analytics records.

### Data lifecycle

`dataLifecycle.scope` is `organization` (installs go
through `app/src/plugin_data.py`; the module provides `DATA_SCHEMA_UP`,
`DATA_SCHEMA_DOWN`, `org_has_data`, `export_org_data`, `purge_org_data`),
`account` (each account installs for itself; account deletion calls
`purge_account_data`) or `none`. `localData: true` keeps data on the device
and offers a JSON bundle on removal. Packages may export
`deliver_reminders(runtime)` for the App's maintenance phase, and
organization-data packages may export `record_lead(runtime, org, lead)` to
receive leads from `plugin_data.deliver_lead` (the CRM does, for the
affiliate program).

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
  `:/forkmesh-market/packages/<id>/repository/<folder>/...` (inside the
  plugin's module when it is one); the generator
  also emits `FORKMESH_MARKETPLACE_RESOURCE_GLOBS` so a new file in such a
  folder re-runs package discovery. The Docs package uses it for `docs`,
  because a copy of the library inside the package would drift from the web.
  `coreLinked: true` marks a plugin the window still calls directly; it is
  always built into the core ([plugins.md](plugins.md#plugins-the-core-still-links)).
  A desktop extension registers itself with
  `DesktopPackageExtensionRegistration(<desktop entrypoint>, factory)`.
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
The top-level marketplace JSON files, every `manifest.json`, every
`content/desktop/build.json` and the generator's two scripts are
`CMAKE_CONFIGURE_DEPENDS`, so editing one makes the next Ninja build configure
again. A `CONFIGURE_DEPENDS` glob alone notices only added or removed files:
before, a version bump kept the old `generated/marketplace/catalog.json`, while
the app embeds `manifest.json` straight from the source, and the registry drops
a package whose catalog and manifest versions differ ("Catalog identity does
not match"), so CRM 1.49.0 vanished from the desktop's left rail.

## Operations

- Releasing a package version: bump `manifest.json` `version`, the matching row
  in `catalog.json`, then run `app/tools/build_market_catalog.py`. A manifest
  bumped without its catalog row passes pytest but fails every desktop
  configure.
- Package server modules deploy with the App to the core nodes
  ([auto-deploy](../operations/auto-deploy.md)); package web pages
  with the App's static assets.

## Security

- Desktop package network calls must use `NativePackageHost::network` so the
  firewall sees them.
- Server modules get only `_PluginApiRuntime`; outbound fetches are limited to
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

