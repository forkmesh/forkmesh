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
| `marketplace/packs.json`, `marketplace/guides.json` | Packs (several packages installed together) and onboarding guides (moving a site from another host). Validated by `marketplace_lib.load_onboarding`; see [Packs and guides](#packs-and-guides). |
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

**Discovery.** `catalog.json` is the only list of packages. Each generator reads
it through `marketplace_lib.load_catalog`, which checks that every catalog row
matches its manifest's id and version.

**Sockets.** A manifest's `contributions` name a host socket, an entrypoint and
socket config. Hosts implement:

- `dashboard.page` (App): an HTML view plus optional script; the script calls
  `window.ForkMeshDashboard.registerPage(pageId, initializer)`.
- `www.page` (website): a public page at a one-segment route on forkmesh.com
  (for example `/design`). `build_app_plugins.stage_www_pages`, called by the
  www build before canonical links and the sitemap, copies the HTML entrypoint
  to `<route>.html`, the `assets` entrypoints into `<route>/`, and runs the
  optional `builder` entrypoint (`build(repository, destination)`) into the same
  folder. `/<route>/<file>?v=${version}` references in the page, scripts and
  styles get content hashes, scripts first so a changed data file changes the
  script URL too. A route already served by `www/public` or reserved by
  `static_routes.CORE_RESERVED_ROUTE_PREFIXES` is refused. The generated App
  registry carries the names as `PLUGIN_WWW_SITE_ROUTES`, which `static_routes`
  adds to `WWW_SITE_ROUTES`, so the App host 308s the path to the website.
- `worker.http` (App): a Python module and a route under the package's
  namespace. Most packages instead declare a top-level `worker` block
  (`module`, `api`, `outboundOrigins`) with a `worker` entrypoint; the App
  mounts it at the `api` prefix (for example `/api/voice-changer`).
- Desktop native pages: entrypoint `builtin:<id>` in the manifest. The package's
  C++ registers a `NativePackageContribution` with a static
  `NativePackageRegistration`; the generated CMake input links it in.
  A contribution with `browserPath` (and no factory or API prefix) is a rail
  link instead of a page: a non-checkable rail button that opens the active
  host (`catalogApiUrl()`) with that path in the browser
  (`MainWindow::openNativePackageLink`). It keeps the rail order, caption and
  right-click menu of a page, never pops out, and a top-search pick of the
  package opens the link.
- World: `virtual.world` and `virtual.object` packages export an
  `install(host)` module (see the district contract in `marketplace/README.md`).

Only packages with `install.bundled: true` are activated by the App host.
`install.default` decides whether a desktop installs the package on first run;
it is never permission to run server code. On the web it lists the package in
the dashboard's Plugins column: for every signed-in account when the package
keeps no organization or account data, and for every member of an
organization when it keeps organization data (the package's page then offers
the install; see [../app/analytics.md](../app/analytics.md)).

**Desktop native page contract** (`desktop/src/NativePackageExtension.h`). The
host passes `NativePackageHost`: manifest metadata, a signed `request`
function, the window's shared `QNetworkAccessManager` (so the desktop firewall
decides every outbound destination), `searchIndexChanged`,
`navigationBadgeChanged`, and `userDirectory`: the lowercase ForkMesh account names the desktop has cached
(calling it also refreshes that cache, at most once every five minutes).
Packages read the directory through the host because their own `request` can
reach only their API prefix. `promptTargetChanged` is described with
`hostPromptTarget()` below.

The host's `request` rides out a relay hiccup before it reports one. When the
network manager's host-wide cooldown suppresses the call (opened by a 5xx or
429 on any `/api/` route, such as the 502 burst a core-node restart produces),
or a GET gets a 429/502/503/504, it waits out the cooldown and sends again, up
to four times while the cooldown is at most 90 s. Only a suppressed call is
resent for other methods, since it never left the desktop. After that the
handler gets "the ForkMesh relay is restarting or unreachable; try again in
N s" rather than the backoff reply's own "ForkMesh relay failed with HTTP 502;
backing off". On 2026-09-30 Pedometer showed that raw text although
`/api/steps` itself answered 200 all day: chat and node-event routes had 502ed
while the core nodes restarted.

The page is a `NativePackageWidget` with:

- `refresh()`: load data. Pages load on open and on Refresh; no timers.
- `navigationBadge()` / `prepareNavigationBadge()`: a count drawn on the
  page's rail button (0 hides it). The host reads it when the button is
  created and whenever the page calls `navigationBadgeChanged`, and calls
  `prepareNavigationBadge()` once, right after the button is created, so a page
  with a badge can load what it counts before it is opened (default: nothing).
- `setHostSearchQuery()`: the top search box filters the visible page.
- `searchHits()` / `activateSearchHit()`: rows for the top search dropdown,
  answered from memory on the GUI thread. `prepareSearch()` warms a page the
  user has not opened yet (default: `refresh()`).
- `exportData()`: a copy of server-side records, offered when the package is
  removed from the rail.
- `hostPromptTarget()` / `submitHostPrompt()`: a page that returns a
  placeholder takes over the footer prompt box while it is the visible
  section. Enter, the "new" button, and voice auto-submit hand it the typed
  text and the attached files instead of starting an agent; the box keeps its
  text when the page refuses it, and clears once it is taken. The placeholder
  and the Enter tooltip come from the target, and the page calls
  `promptTargetChanged` when it starts or stops accepting prompts (Calories
  does so when it learns it is installed). Other pages keep the agent prompt.
- `userDirectoryChanged()`: the host fetched a newer user directory. A page
  showing suggestions calls `userDirectory` again. The CRM uses this for its
  **Add members** completion.

`userRecordsAvailable` and `userRecords` hand a page the administrator view of
the user directory, email addresses included. `userRecordsAvailable()` is true
only while the signed-in account is a ForkMesh administrator; `userRecords(done)`
reads `GET /api/accounts/users` with the administrator proof on every call (no
cache, since that read scans whole tables) and calls `done(users, error)`. The
CRM uses them for **Import users** (`marketplace/packages/crm/README.md`).

`agentName` and `askAgent` let a page have the current agent write text.
"Current" means the **Default agent** in Settings (`agents/defaultProvider`).
`agentName()` returns "Claude Code" for the Claude providers, "Codex" for Codex
and OpenAI, and an empty string for the Cloudflare and local Llama agents,
which have no one-shot run. `askAgent(prompt, done)`
(`MainWindow::askNativePackageAgent`) starts one read-only run of that CLI, the
same way the calendar's **Fill event** does: Claude Code in plan mode or Codex
in Plan mode, low effort, the configured model, the signed-in agent account,
with the API key variables removed. It calls `done(reply, error)` exactly once:
with the final `result` text, with the CLI's stderr when the run fails, or after
five minutes. The run starts in the working folder, so it can read the open
repository, but it never shows in the Agents list. The CRM uses this for
**Write email with …** (`marketplace/packages/crm/README.md`).

`specialAgents` and `specialAgentAction` let a page show and work with the
desktop's special agents. The host fills them only for a package whose manifest
declares `desktop.agents.run`; for any other package both are empty.
`specialAgents()` (`MainWindow::nativePackageSpecialAgents`) returns the
`teammates.list` rows of the special agents of the repository picked on the
Special agents page: `id`, `name`, `avatar`, `specialty`, `focus`, `status`
(`Working`, `Queued`, `Waiting`, `Scheduled`, `Paused` or `Idle`), `enabled`,
`schedule` (with `human` and `next`), `lastActive`, `workflow` and
`marketingChannels`, among others. `specialAgentAction(id, action, text, done)`
(`MainWindow::runNativePackageSpecialAgentAction`) runs on the next event loop
turn, so the button that asked may be rebuilt meanwhile, and calls
`done(ok, message)` once. `open` shows the agent's page, `run` starts a run
like its play button, and `prompt` sends `text` like the prompt box on its
card: it steers a run in progress or starts one with the text as the task. An
agent with a built-in workflow takes no prompt. The Marketing package uses
them on its channel pages (`marketplace/packages/marketing/README.md`).

**Own window.** Right-clicking a native page's rail button offers "Open in new
window" (`MainWindow::openNativePackageWindow`, `MainWindowMarket.cpp`). Dragging
the rail button out of the main window does the same
([desktop](../desktop/README.md#architecture)). The
page widget itself moves into a top-level window parented to the main window,
so the package keeps one instance, one set of listeners and its state. A
placeholder takes its slot in the section stack: the rail button's
`QButtonGroup` id is the stack index, so removing the page outright would shift
every later package's index. Clicking the rail button while the page is out
shows the placeholder and raises the window. Closing the window, or "Return to
main window", puts the widget back at the placeholder's index
(`dockNativePackageWindow`); the window's geometry is kept under
`market/windows/<id>/geometry`, outside the package's own settings so removing
the package does not export or clear it.

**Worker runtime.** A package module receives `_PluginApiRuntime`
(`app/src/entry.py`): `session`, `json_body`, `organization`,
`plugin_installed`, `notify`, `new_secret`, `seal_with` / `open_with`,
`outbound_text` / `outbound_json` (limited to the manifest's
`outboundOrigins`; `outbound_text` also returns a response's `Link` header
as `link`, for paginated APIs), `request_origin`, `ai_json` (bounded
Workers AI), and Mailtrap sending. Packages without a website session (desktop pages) sign
requests with a per-package `REQUEST_PROOF` string.

**Data lifecycle.** `dataLifecycle.scope` is `organization`, `account` or
`none`:

- `organization`: installs go through `app/src/plugin_data.py`. The module
  provides `DATA_SCHEMA_UP`, `DATA_SCHEMA_DOWN`, `org_has_data`,
  `export_org_data`, `purge_org_data`. Uninstall offers an export, purges rows
  and drops the schema when the last organization uninstalls.
- `account`: each account installs for itself through the package API
  (`calories`, `vault`); account deletion calls `purge_account_data`.
- `localData: true`: data lives on the device (for example `timers`). Removing
  it from the rail offers to save a JSON bundle; installing offers to import one.

Packages may also export `deliver_reminders(runtime)`, run by the App's
maintenance phase through `plugin_data.deliver_reminders`.

## Packs and guides

A **pack** installs several packages with one click. `marketplace/packs.json`
(`schemaVersion: 1`) lists each pack's `id`, `name`, `tagline`, `summary` and
`packages`: a list of catalog ids, or `"*"` for every package. A pack id may
not reuse a package id.

- **The Whole Cutlery Drawer** (`whole-cutlery-drawer`, `"*"`): every
  top-level package this device can install. `PackageRegistry::packPackages`
  (and `pack_members` in `app/tools/build_market_catalog.py`) leave out agent
  packages, whose install starts a multi-gigabyte Ollama download, themes,
  which would each switch the skin, catalog-only and World-only packages, and
  nested packages, which come with their parent.
- **Chat & Essentials** (`chat-essentials`): Chat, Inbox, Agents, Notes,
  Tasks, Calendar and Timers.
- **Site Mover** (`site-mover`): Code, Commits, Branches, Pull requests,
  Issues, Actions, Workflows, Releases and Status Page. The guides use it.

A **guide** (`marketplace/guides.json`) walks someone from another host to
ForkMesh: `title`, `from`, `summary`, 2-12 `steps`, a `pack` and/or extra
`packages`, and up to six https `links`. The guides cover Cloudflare Pages
with GitHub, Vercel, Netlify, AWS S3 with CloudFront, GitHub Pages and
Fly.io. ForkMesh hosts the repository and runs the builds, not the website,
so every guide keeps the current host serving the site. It imports the
repository on app.forkmesh.com (Repositories, New), adds the host's token and
`FORKMESH_ACTIONS_ALLOW_NETWORK=1` under Settings, Secrets & Coves, deploys
from a `.forkmesh/deploy.yml` workflow with the host's own CLI, and turns
off the host's GitHub integration. Deploys that need secrets run on the
owner's desktop, because cloud runners get no variables
([../server/runner-actions.md](../server/runner-actions.md)).

The desktop generator adds both files to `marketplace.qrc`, and
`PackageRegistry::loadOnboarding` reads them next to the catalog. An entry
that fails validation becomes a registry warning instead of hiding the
Market. On `/market`, `build_market_catalog.py` renders them between the
`market:onboarding` markers.

### Market lists

- **Sort.** The desktop's `marketSort` menu (saved as `market/sort`) and the
  `/market` page's **Sort by** menu order packages as Featured (catalog order),
  Name A-Z, Top rated, Most installed, Most downloaded or Most used. Ties keep
  catalog order.
- **Stats.** Each card shows its star rating, downloads, installs, how many
  signed-in people opened it, and the total opens. Ratings and usage come
  from one request, `GET /api/market/reviews/summary`, whose `usage` and
  `usageByMarketplace` maps sum `marketplace_package_activity` per package.
  Download and install counts still come from `/api/market/installs`. A card
  with no reported numbers shows no stats line.
- **One click.** A card's **Install** installs straight away. Missing
  dependencies come along, and the toast names them. A package that keeps
  local data asks about importing only when a saved copy exists at its
  default backup path. **Install from a saved copy…** covers a copy saved
  somewhere else. A pack or guide installs its packages in one batch
  (`MainWindow::installPackageSet`): `m_marketBatchInstall` holds back the
  per-package runtime sync, count report and shelf rebuild until the end.

## Packages

| Package | Type | Surfaces | Default | Notes |
| --- | --- | --- | --- | --- |
| [`analytics`](../../marketplace/packages/analytics/README.md) | extension.native | web, Worker | on | First-party product analytics; replaced PostHog on ForkMesh's own pages (2026-09-23). Public write-key route; per-URL unique visitors feed the docs view counter (2026-09-29). |
| [`aquarium`](aquarium.md) | extension.native | desktop | off | The window turns into an aquarium while ForkMesh is in the background: a fish per running agent, 30 tricks with the window's buttons and text, 30 with the Genie spider. Host socket `desktop/src/PetHostFeed.h` (2026-10-07). |
| [`backups`](../../marketplace/packages/backups/README.md) | extension.native | desktop | off | Every backup on this computer on shelves: snapshots, kept, package data, moved aside, fleet database. Host socket `desktop/src/BackupHostFeed.h` (2026-09-27). |
| [`browser-extension`](../../marketplace/packages/browser-extension/README.md) | extension.native | web (zip download), Chrome | off | One Chrome extension for CRM and Vault; own package since 2026-09-18. |
| [`calendar`](../../marketplace/packages/calendar/README.md) | extension.native | web, desktop window | off (on before 2026-10-11) | Page for the host `/api/calendar` service. Account-synced since 2026-10-10 ([core plugins](core-plugins.md)). |
| [`calories`](../../marketplace/packages/calories/README.md) | extension.native | web, desktop, mobile, Worker | off | Account-scoped diary sealed under a per-account key. Account-synced since 2026-10-10; an account that set it up keeps it ([core plugins](core-plugins.md)). |
| [`cleanup`](cleanup.md) | extension.native | desktop | off | Disk Cleanup: what Claude Code, Codex and agent runs left on disk, what each item is attached to, and guarded deletion (git for worktrees, `removeTree` for files). Host socket `desktop/src/CleanupHostFeed.h` (2026-09-30). |
| `capture` ([preview](../../marketplace/packages/capture/preview/README.md)) | extension.native | desktop | on | Region capture and annotation. |
| [`competitor`](../../marketplace/packages/competitor/README.md) | extension.native | web, Worker | off | Organization-private competitor list. |
| [`crm`](../../marketplace/packages/crm/README.md) | extension.native | web, desktop, iOS, Android, Worker | off | Contacts, pipelines, lists, Mastodon and GitHub contributor import, deck links. |
| [`design-system`](../../marketplace/packages/design-system/README.md) | extension.native | website `/design`, desktop rail link, iOS and Android menu | off (on before 2026-10-11) | Colours, type, spacing, buttons and every icon of web, desktop, iPhone and Android, read from the sources at website build time through the `www.page` socket (2026-10-04). |
| [`docs`](../../marketplace/packages/docs/README.md) | extension.native | desktop | on | The `www/docs` library (the pages forkmesh.com/docs renders) on a rail page: areas, full-text search, links, outline, backlinks. Embedded through `repositoryResources` (2026-09-29). |
| `discord` ([preview](../../marketplace/packages/discord/preview/README.md)) | extension.native | desktop | on | Discord bot connector. |
| [`food-guide`](../../marketplace/packages/food-guide/README.md) | extension.native | iOS (`foodGuide`) | off | A Food Guide section with breakfast, lunch and dinner ideas from recipes on the web, made with ALDI groceries, ranked by the calorie budget (Calorie Counter goal), a goal and the nutrient gaps in 14 days of Apple Health nutrition; everything is worked out on the phone (2.0.0, 2026-10-13; 1.0.0 was the Basil agent). |
| `forkmesh-ide` ([VS Code README](../../marketplace/packages/forkmesh-ide/content/vscode/README.md)) | extension.vscode | catalog only | off | Source for the VS Code extension. |
| `forkmesh-sky-office` ([preview](../../marketplace/packages/forkmesh-sky-office/preview/README.md)) | virtual.world | World | off | The Sky Office district. |
| `gemma-4` | agent | desktop | off | Local Ollama agent model (`agent.json`). |
| [`glasses-alert`](../../marketplace/packages/glasses-alert/README.md) | extension.native | Android, iOS | off (on before 2026-10-11) | Glasses Alert: the phone scans Bluetooth adverts for Meta's and Luxottica's company IDs (Ray-Ban Meta, Oakley Meta) and rings a loud alarm with strong vibration, through silent mode, when a new pair comes near, then points an arrow and a live closeness bar at it. A pill floats over the app while watching, and other Bluetooth devices heard show as icons. Android scans in a `connectedDevice` foreground service; iOS scans while the app is open. On the device only (2026-10-07, arrow 2026-10-09). |
| `ledger` ([preview](../../marketplace/packages/ledger/preview/README.md)) | extension.native | desktop | off (on before 2026-10-11) | Rail link to the public Ledger plus local expense records. |
| [`marketing`](../../marketplace/packages/marketing/README.md) | extension.native | web, desktop, Worker | off | Channel catalog, content calendar, ads manager, growth loop, Reddit desk, posted links. |
| [`notes`](../../marketplace/packages/notes/README.md) | extension.native | web, desktop window | off (on before 2026-10-11) | Page for the host `/api/notes` service. |
| `reddit-banner-board`, `x-banner-board` | virtual.object | World | off | Social boards; loaded only once installed (2026-09-23). |
| `sentry` ([preview](../../marketplace/packages/sentry/preview/README.md)) | extension.native | desktop | on | Unresolved Sentry issues in the Log page. |
| [`sign-in`](../../marketplace/packages/sign-in/README.md) | extension.native | iOS, Android | off (on before 2026-10-11) | The phone top bar avatar and Sign in chip and the menu's Sign in / Sign out; per device. Settings always keeps sign-in (2026-10-10). |
| [`sleep`](../../marketplace/packages/sleep/README.md) | extension.native | web, desktop, iOS, Worker | off | Sleep: the iPhone counts the stretch it lies still and unused by your bed as sleep, uses Apple Health sleep when it is there, and syncs one entry per night; account-scoped, sealed per account. |
| [`speech`](../../marketplace/packages/speech/README.md) | extension.native | iOS | off (on before 2026-10-11) | Speech Playground: type text and hear it in the phone's best voice (Personal, Premium, Enhanced), with word highlighting, speed and pitch, and audio export. On the device only; the manifest's `ios: builtin:speech` entrypoint marks it as built into the iPhone app (2026-10-02). |
| [`status-page`](status-page.md) | extension.native | desktop debug bar, iOS and Android footer, web, Worker | off (on before 2026-10-11) | A dot per system from `/api/status`. Off for accounts created from 2026-10-11 (2.1.0). Since 2.0.0 (2026-10-07) installed per account (`install.sync: "account"`), so removing it anywhere removes it everywhere; declared settings; custom monitors at `/api/status-monitors`. |
| `status-check-*` (18, [list](status-page.md#the-checks)) | extension.native | wherever Status Page shows dots | on (Mirrors off) | One package per check, each depending on `status-page` and naming its rows in a `status.monitor` contribution (2026-10-07). |
| [`steps`](../../marketplace/packages/steps/README.md) | extension.native | web, desktop, iOS, Worker | off | Pedometer: the iPhone counts steps through Apple Health and syncs one total per day; account-scoped, sealed per account. |
| [`stretch`](../../marketplace/packages/stretch/README.md) | extension.native | web, desktop, iOS, Worker | off | Stretch Break: a guided, spoken stretch routine for the back and posture (hips, arm swings that rotate the torso, side stretches, twists, neck) and a lymph tapping routine, with an animated example person; iPhone saves each session to Apple Health as a workout; account-scoped, sealed per account (2026-10-08). |
| [`sugar-break`](../../marketplace/packages/sugar-break/README.md) | extension.native | web, desktop, iOS, Worker | off | Sugar Break: start a break from sugar (30 days by default) and tap +1 every time you skip it; the iPhone shows Apple Health dietary sugar beside each day; account-scoped, sealed per account. |
| [`tasks`](../../marketplace/packages/tasks/README.md) | extension.native | web, desktop window | off (on before 2026-10-11) | Page for the host `/api/tasks` service. |
| `timers` ([preview](../../marketplace/packages/timers/preview/README.md)) | extension.native | desktop top bar | off | Timers, countdowns, alarms; device-local data. Install state account-synced since 2026-10-10. |
| `tour` ([preview](../../marketplace/packages/tour/preview/README.md)) | extension.native | desktop | off (on before 2026-10-11) | First-run welcome and the replayable feature tour; removable since 2026-09-27; follows the account since 2026-10-10 (the per-phone `packages/tour/on` is gone). |
| [`vault`](../../marketplace/packages/vault/README.md) | extension.native | web, extension, desktop, mobile, Worker | off | End-to-end encrypted password vault, suite `fmv1`. Account-synced since 2026-10-10; an account that set it up keeps it. |
| [`voice-changer`](../../marketplace/packages/voice-changer/README.md) | extension.native | desktop, Worker (AI design only) | off | Free, desktop-only live voice changer. |
| [`workflows`](../app/workflows.md) | extension.native | web, desktop rail link, iOS and Android Hub and Market, Worker (`builtin:workflows`) | on | Visual automations with 100 integrations, n8n-compatible files and 10 examples, plus visual editing of a repository's actions, shortcuts, skills and special agents. Installed per account (`install.sync: "account"`). Ships a special agent, two shortcuts, a skill and an action through the `content.*` sockets (2026-10-09). |
| `workflows-pack-*` (12), `workflows-examples`, `workflows-repository-files` | extension.native | wherever Workflows runs | on | Nested packages depending on `workflows`: one per integration category (`workflows.integrations`), the examples (`workflows.templates`) and the Repository view (`workflows.repository`) (2026-10-09). |
| [`water`](../../marketplace/packages/water/README.md) | extension.native | web, desktop, iOS, Worker | off | Water Intake: drinks log and daily goal, written to and imported from Apple Health on iPhone. |
| `winamp` | theme | desktop | off | Installable desktop theme. |
| `agents`, `special-agents`, `inbox`, `code`, `chat`, `bug-reports`, `issues`, `commits`, `branches`, `pull-requests`, `releases`, `actions`, `discussions`, `desktops`, `shortcuts`, `hosts`, `network`, `coves`, `on-device-ai`, `activity`, `stats`, `organizations`, `world` ([core plugins](core-plugins.md)) | extension.native | desktop, iOS, Android, web (as each feature exists) | off (on before 2026-10-11) | Account-synced on/off switches for features built into each app; Pull requests depends on Branches, Special Agents on Agents (2026-10-10). |

All packages are currently free (`price.amount: 0`). One-time package purchases
were added with the Voice Changer on 2026-09-22 and removed the same day along
with Polar; Patreon membership is the only paid path (see
[`www/docs/operations/patreon.md`](../../www/docs/operations/patreon.md)).

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
- Pages must not poll. Load on open and on Refresh (client polling was removed
  2026-09-19).

## Recent changes

- 2026-10-13: Market sort, usage stats, packs and onboarding guides. The user
  asked for sorting and search, every usage stat and rating on the Market
  list, one-click installs, packs (one with everything, one with chat and
  essentials) and guides for moving a site from Cloudflare, Vercel, AWS and
  similar hosts. The desktop Market and `/market` now have a sort menu,
  and their cards show ratings, downloads, installs, people and opens. The
  desktop has **Packs** and **Guides** tabs, and installing a package takes
  one click. The guides keep the existing host serving the site, because
  ForkMesh does not host websites. See [Packs and guides](#packs-and-guides).
- 2026-10-13: Plugins choose where they show up; Workflows 1.1.0 and Food
  Guide 2.1.0. The user circled Vault, Calories, Steps, Water, Sleep, Sugar
  Break, Stretch Break, Food Guide and Workflows and asked that they be
  plugins, off in new projects, installable on mobile, desktop and web or
  only some of them. An account install now has places (`install.places` in
  the manifest, `POST /api/plugins/@account/<id>/places`, a "Show on" row in
  every Market); Workflows is off for accounts and projects from 2026-10-14;
  a data package set up without a Market install no longer follows the
  account into new projects
  ([core-plugins.md](core-plugins.md#2026-10-13-where-each-plugin-shows-up)).
- 2026-10-13: Food Guide 2.0.0. The user said the food suggestion should not
  be a new agent but a section to open, showing breakfast, lunch and dinner
  based on the user's dietary needs from Apple Health nutrition history, using
  ALDI groceries and healthy recipes from the internet, inside the day's
  calorie budget and toward their goal (weight loss or another). Basil and the
  Suggest a meal shortcut are gone; the package now declares an `ios`
  entrypoint and routes to `foodGuide` like Glasses Alert. ALDI publishes no
  product API, so the app bundles 48 ALDI US products (names checked on
  aldi.us) with USDA reference nutrition, and recipes whose pages were opened
  and checked (31: ALDI, Cleveland Clinic, Heart Foundation, Diabetes Food Hub
  and others), stored as grams per serving so the phone computes each nutrient. Apple Health is read only and
  never leaves the phone
  ([README](../../marketplace/packages/food-guide/README.md)).
- 2026-10-12: Marketing 1.35.0: a channel can be deleted. The user asked for
  a way to delete a marketing channel. `DELETE /api/marketing/<org>/channels/<id>`
  (owners and admins) takes it off the dashboard and deletes its logged
  numbers. The web Channels page, the web channel settings and the desktop
  channel page have a **Delete** button
  ([Marketing README](../../marketplace/packages/marketing/README.md#deleting-a-channel-1350)).
- 2026-10-12: Uninstall from the phone, Market views and opens, quick stars.
  The user asked that a plugin uninstalled on the phone leave the home menu
  and stay gone until it is installed again from the Market. They also asked
  for a count of how many times each person viewed or opened a plugin, and
  for a quick star rating on each plugin.
  - **Account uninstall is the main path.** Since the core-plugins change
    ([core-plugins.md](core-plugins.md)), almost every phone package is
    account-synced: removing it in the Market uninstalls it on the account,
    and the package gate hides every entry point on every device until it is
    installed again.
  - **Tools.** Sleep, Pedometer, Water Intake, Sugar Break and Stretch Break
    each have their own **Uninstall**. On iOS it now also uninstalls the
    package from the account, so the tool leaves the home menu. Before, a
    Market-installed tool kept its account row and stayed on the menu.
  - **Per-phone uninstall for organization packages.** CRM, Marketing,
    Analytics and Competitors are installed by an organization. A member can
    remove one from their own phone with **Uninstall from this phone**. The
    removed ids are kept in `packages/removed`: the iOS settings, and
    Android's per-project prefs. They are subtracted from the gate's
    installed set (`PhonePackages.installed` on iOS,
    `PackageGateFeature.isInstalled` on Android), so the package leaves
    every surface the gate covers. The organization keeps the install and
    its data; an org **Remove** still deletes the data. On iOS, a link or
    restored path to a removed package's screen shows "uninstalled" with a
    way back to the Market. An org install or removal on iOS now refreshes
    the hub at once.
  - **Views and opens.** Opening a package's Market page reports a view.
    Opening its screen reports an open, which also counts as a use for
    rating. The phone reports each package and kind at most once a minute.
    The page shows the totals, your own counts, and, for maintainers and
    admins, who opened it. See
    [MARKETPLACE_RATINGS.md](../MARKETPLACE_RATINGS.md#views-and-opens).
  - **Quick stars.** Five stars sit near the top of each package page. Five
    stars is saved at once. Fewer opens the review form with those stars
    picked, because a rating under five stars still needs a written review.

- 2026-10-12: `market_packages.ID_RE` follows the manifest schema's id
  pattern (dots allowed, up to 96 characters). `muse-spark-1.3-contributor`
  and `nemotron-3.5-lightning` were missing from `/api/market/packages`
  because the old pattern refused the dot. The four custom model and Computer
  Security packages were also added to
  `marketplace_reviews.FIRST_PARTY_PACKAGES`; both gaps failed
  `test_market_packages` and `test_marketplace_reviews` on main.
- 2026-10-12: Browser Extension 2.18.0 and Marketing 1.34.0: your Reddit
  accounts' posts and views. The user pressed **Update** on their Reddit
  profile and nothing updated: the extension read only subreddits, threads,
  rules and searches. The desk's watch list now has up to 10 accounts
  (`ada_lovelace`, `forkmesh`, …); **Update u/_name_ posts and views** on
  a profile adds it, records each post and comment in Marketing's posts, then
  reads each one's views in a background tab, since Reddit shows them only
  to the author on the post's page
  ([extension README](../../marketplace/packages/browser-extension/content/extension/README.md#your-reddit-accounts-posts-and-views-2180),
  [Marketing README](../../marketplace/packages/marketing/README.md#your-reddit-accounts-1340)).
- 2026-10-12: Browser Extension 2.17.0: the popup and the refresh panel
  suggest the Marketing channel updated longest ago, skipping any updated in
  the last hour, with **Go to _channel_** and, on its page, **Update
  _channel_**. The user asked for it and for the popup's refresh card and
  **This page** card to become one card without repeated buttons or text; see
  "Next channel to update" in the extension's README.
- 2026-10-12: Food Guide 1.0.0. The user asked for a special agent
  responsible for suggesting appropriate food to eat, shipped as a plug-in.
  The package carries no code: it ships Basil
  (`.forkmesh/teammates/food-advisor.json`) and the Suggest a meal shortcut
  through the `content.teammate` and `content.shortcut` sockets, so a user
  adds them to a repository from the Workflows Repository view. Basil asks
  once for the meal, allergies, foods avoided, what is on hand, time and
  budget, offers three suggestions with reasons and a pick, never suggests a
  named allergen, sends medical, pregnancy, eating-disorder and child cases to
  a clinician, and has no permissions, so it cannot change files. Off by
  default. It declares no dependency on `workflows-repository-files`, though
  that view is how its files are added: `build_market_catalog.market_parent`
  nests a package under its first dependency, and a grandchild of Workflows
  would not be shown on /market at all
  ([README](../../marketplace/packages/food-guide/README.md)).
- 2026-10-10: Core features as plugins. The user circled almost every Home
  tile on the iPhone and asked to "make sure these are full fledged plugins
  and turned off by default on new projects on desktop, mobile and web, nest
  any that have dependencies". 23 new account-synced packages (Agents, Inbox,
  Code, Chat, the repository tabs, Desktops, Special Agents, Shortcuts, Hosts,
  Network, Coves, On-device AI, Activity, Stats, Organizations, World) and the
  per-device Sign in; Notes, Tasks, Calendar, Ledger, Tour, Design, Status
  Page, Speech, Glasses Alert, Timers and the health packages and Vault moved
  to account sync with `install.default: false`. The new manifest field
  `install.installedBefore: "2026-10-11"` keeps them installed for older
  accounts and projects; `GET /api/market/packages` and
  `plugin_registry.PLUGINS` carry it, and `plugin_data` falls back to it (and
  to a data package's own `account_installed` hook) when the account has no
  row ([core-plugins.md](core-plugins.md)).
- 2026-10-10: CRM 1.28.0: confetti on every device when an email is tracked.
  The user asked that sending an email to the CRM, once it is tracked, set
  off major confetti and celebration on all their devices. The CRM worker
  files a `plugin_activity` ping with `meta.celebrate`, and the web
  dashboard, the desktop, iOS and Android each play a full-screen confetti
  show with the ping's title when a fresh one arrives
  ([CRM README](../../marketplace/packages/crm/README.md#confetti-on-every-device-when-an-email-is-tracked-1280)).
- 2026-10-10: CRM 1.27.0: the web Contacts tab opens with the best person
  to contact, picked by the desktop's next-best-contact rule over the whole
  organization, with Email with Zoho, Message on Mastodon and Message on X
  buttons, Mark contacted and Skip
  ([CRM README](../../marketplace/packages/crm/README.md)).
- 2026-10-09: No toasts in packages. The user asked for no toasts anywhere,
  with the information shown inline where it is appropriate. Design System
  1.0.1: copying a swatch or icon name says **Copied** (or why it failed, in
  the error colour) inside the tile that was clicked instead of a pill at the
  bottom of the window; the page was the only user of its `.ds-toast` style,
  which is gone. Tasks 1.0.6 and Notes 1.0.1: the package views carry the
  dashboard partials' inline notices (`[data-tasks-notice]` above the task
  list with the Undo button for a trashed task; `[data-note-notice]` lines in
  the note, share popover and version history) in place of the
  `[data-task-toast]` and `[data-note-toast]` cards. The browser extension
  injects no toast of its own: `sentToast()` in `content.js` only reads
  Gmail's own "Message sent" link to find the sent message id. The analytics
  SDK's survey card asks a question rather than reporting one, and its thank
  you shows inside the card that was answered, so both stay.
- 2026-10-09: Marketing 1.33.0 and Browser Extension 2.16.0: the desktop
  responds to what the browser reads. The user asked that refreshing Reddit
  and other pages with the extension update the desktop app and show
  activity about it, and reported the refresh panel's "Sent to the Reddit
  desk: 0 new threads, 0 rule sets, 0 view readings.". The first read after a
  page loads, each refresh-panel run and each channel update now write a line
  to `POST /api/marketing/<org>/activity`. Every Marketing write rings the
  writer's devices with the `package:marketing` account event. The desktop
  forwards `package:<id>` events to the package page
  (`NativePackageWidget::packageChanged`), and Marketing reloads and lists
  the lines as **From your browser**. The Reddit send now says what it did,
  or why nothing qualified
  ([extension README](../../marketplace/packages/browser-extension/content/extension/README.md#browser-activity-on-the-desktop-2160),
  [Marketing README](../../marketplace/packages/marketing/README.md#browser-activity-1330)).
- 2026-10-09: Marketing 1.33.1: the Discord channel counts the server's own
  messages as posting. The user reported it still said **Needs attention**
  after messages had been posted. A Discord refresh through the
  organization's connector now reads the last 14 days of messages
  (`runtime.discord_recent_messages`) and stores only the newest time as
  `feed.lastPostAt`, which channel health reads
  ([Marketing README](../../marketplace/packages/marketing/README.md#discord-messages-count-as-posting-1331)).
- 2026-10-09: Workflows became a package (`builtin:workflows`, like Tasks)
  with 14 nested packages, and the `content.action`, `content.shortcut`,
  `content.skill` and `content.teammate` sockets were implemented for
  repository files a package ships ([workflows.md](../app/workflows.md)).
- 2026-10-08: Marketing 1.32.0 and Browser Extension 2.15.0: the Reddit
  channel gives more to go on, and its numbers say where they came from. The
  user asked for clues (which subreddits to post to, what to talk about),
  said the engagement numbers looked off and wanted to see how they were
  worked out, reported a refresh failing with `channel_http_429`, and asked
  for a button that walks the browser through Reddit like a person. The
  Reddit desk gains **Where to post next** (watched subreddits ranked by
  open threads, replies landed and rules on file, with what blocks each) and
  **What to talk about** (the watch list's searches plus the phrases that
  come back in logged threads), both counted from the desk's own data and
  shown on the web, the desktop and the weekly report. The engagement rate
  now divides by the first of reach, impressions, views and followers the
  snapshot holds, instead of falling through to the follower count, and
  `health.engagement` carries the whole sum, its labels and how long the two
  numbers have been unchanged, shown in every tooltip. A rate-limited Reddit
  refresh is named `reddit_rate_limited`, retries on `old.reddit.com` and
  then waits 15 minutes before touching Reddit again. **Walk the watch list**
  has the extension visit every watched page in one tab, pacing each one with
  reader-speed scrolling and pointer movement (`reddit-walk.js`, which never
  clicks, types or posts) so Reddit renders its lazy listings, then sends
  what it read to the desk
  ([Marketing README](../../marketplace/packages/marketing/README.md#reddit-clues-the-walk-and-what-the-engagement-rate-divides-by-1320),
  [Reddit](../community/reddit.md)).
- 2026-10-08: Marketing 1.31.0: the X channel always has a post ready to go.
  The user asked for one written by Claude Opus 5.5 from past posts,
  engagement and the marketing plan, with the text and image ready to copy
  and a one-tap post from a phone. The worker keeps it sealed in the new
  `marketing_ready_posts` table (`GET`/`PUT`/`PATCH /ready-post`, the
  overview's `readyPosts`); Herald, a new special agent on the `x` channel,
  writes it through the new MCP tools `marketing_x_context` and
  `marketing_save_ready_post`; no manifest permission changes
  ([package README](../../marketplace/packages/marketing/README.md#ready-x-post-1310),
  [../marketing/x-ready-post.md](../marketing/x-ready-post.md)).
- 2026-10-08: Marketing 1.30.1: the Discord post list is read again on
  every visit. The user posted in Discord and the channel page did not
  change: the list was read once per organization and then kept, so a page
  opened before the post kept its first read. The desktop and the web now
  re-read it whenever the Discord channel page is drawn and the list in
  hand is more than 60 seconds old, reuse it inside that minute so repaints
  cannot flood Discord, count a failed read as a read, and say when the
  list was read
  ([Marketing README](../../marketplace/packages/marketing/README.md#the-discord-post-list-is-read-again-on-every-visit-1301)).
- 2026-10-08: Marketing 1.30.0: the Discord channel page lists what was
  posted in the last 6 days. The user asked to see the last posts there.
  The host adds `runtime.discord_recent_messages(org_bi, days)` (new
  manifest permission `organization.discord.messages.read`, package
  installed in the org, current owner grant), which reads the latest 50
  messages of each public channel chosen in the organization's Discord
  connector through the bot path the connector's chat already uses and
  keeps those inside the window; nothing is stored. Marketing serves it as
  `GET /api/marketing/<org>/discord-posts`, shown as a table on the desktop
  Discord channel page and a list on the web Discord card
  ([Marketing README](../../marketplace/packages/marketing/README.md#discord-posts-on-the-channel-page-1300)).
- 2026-10-08: Marketing 1.29.0: wider Mastodon search for people to follow.
  The user asked for more people to follow from better searches and
  reported that recorded numbers were not all coming in. Suggestions now
  also come from the account's followers and the people who engage with
  its posts, more topics (including its featured and followed hashtags),
  trending posts and the directory, read concurrently. A channel refresh
  keeps Following and Posts, which it used to drop, and is labelled as a
  refresh ([Marketing README](../../marketplace/packages/marketing/README.md#wider-mastodon-search-for-people-to-follow-1290)).
- 2026-10-08: Browser Extension 2.14.0: the refresh bar moves to the side
  panel. The user asked for no code injected into any channel site and no
  **Allow channel sites** step. The in-page bar, its content script and the
  script registered on channel sites are gone; the same plan (a button per
  page, read pages ticked, one main button) is now in Chrome's side panel,
  which follows the active tab, and in a first popup card with **Open
  refresh panel**, and the toolbar badge counts the site's unread pages. X
  and Reddit still sync what `x.js` and `reddit.js` read; other channels are
  refreshed by Marketing's server (`POST /refresh`), and those it cannot
  refresh only get navigation buttons. The new `tabs` permission shows as
  "Read your browsing history", so Chrome disables the update until it is
  accepted
  ([extension README](../../marketplace/packages/browser-extension/content/extension/README.md#refresh-panel-2140)).
- 2026-10-08: Browser Extension 2.13.0: a refresh bar on the page. The user
  asked for a top bar inside the page, on sites the extension refreshes
  Marketing data from, with a refresh button that leads to the page to read
  and one button per page when several are involved. The new
  `refresh-bar.js` draws a dismissible strip in a closed shadow root on X
  (profile, Media, Mentions, a search for the handle), Reddit (each watched
  subreddit, search, missing rules page and monitored thread, and the
  channel's page) and any granted channel site such as the Mastodon server
  (the channel's saved page), with read pages ticked. On a page to read the
  main button runs the popup's own update, X sync or Reddit desk send;
  elsewhere it goes to the next unread page
  ([extension README](../../marketplace/packages/browser-extension/content/extension/README.md#refresh-panel-2140)).
- 2026-10-08: Marketing 1.28.0: followed Mastodon handles are kept. The
  user reported People to follow still suggested people the account
  follows, and asked for the follower count refresh to keep the usernames.
  A Mastodon refresh now reads the whole following list through its
  `Link: rel="next"` pages (up to 2000) and keeps the handles in the
  settings record; suggestions drop them before ranking. The host's
  `outbound_text` returns the `Link` header as `link` for this
  ([Marketing README](../../marketplace/packages/marketing/README.md#followed-mastodon-handles-are-kept-1280)).
- 2026-10-08: Stretch Break (`stretch`), account-scoped like Sugar Break. The
  user asked for a plug-in that guides them through a stretch routine for
  their back and posture after sitting at a computer (hip circles, arm swings
  that rotate the torso, stretches to the left and right, neck rolls) and a
  lymph tapping routine (arms, around the eyes, the head), with a voice that
  says what to do, an example person doing it with them, and Apple Health.
  The routines, cues and the example person's moves are data served by the
  Worker (`GET /api/stretch/library`, also inside `GET /api/stretch`), so web,
  desktop and iPhone run the same session; each client only interpolates pose
  keyframes and draws a stick figure with the arithmetic written in the
  package README. Arm positions for tapping are solved by two-bone inverse
  kinematics in the Worker; in a flat front view one hand cannot reach the
  other wrist with the arm held straight out, so the tapped arm is bent across
  the body. Voices are the platform's own (`speechSynthesis`, the desktop's
  system voice, `AVSpeechSynthesizer`); nothing goes to a speech service.
  Apple Health has no stretch type, so the iPhone saves each session as an
  `HKWorkout` (Flexibility, or Mind and Body for lymph tapping) and sends
  `health: true` with the session. The preview PNG was rendered with resvg
  (`vl-convert-python`) because no Chromium was available to
  `rasterize_market_previews.py`.

- 2026-10-07: Marketing 1.25.1: the Discord channel's Open link goes to a
  public invite. The user reported that the Discord Open link in Marketing
  went to a login page. Every Open button opened the saved channel link, and a
  `discord.com/channels/...` link shows Discord's login to anyone not signed
  in. Settings now carry a per-channel `openUrl` the web and desktop Open
  buttons follow; for Discord it is an invite (`discord.gg/<code>`) taken from
  the channel link, the vanity invite, or the server widget's
  `instant_invite`, which a refresh now records as the feed's `siteUrl`
  ([Marketing README](../../marketplace/packages/marketing/README.md)).
- 2026-10-07: Marketing 1.25.0 and CRM 1.24.0: suggested emails. The user
  asked for the Email channel to list emails to send, each opening Zoho Mail
  filled in with one click. `GET /api/marketing/{org}/email-suggestions`
  reads `crm.email_suggestions` in-process: unanswered replies, follow-ups
  after 14 days and intros for new investor and partnership contacts, filled
  from CRM's outreach templates, with a `comp.do?ct=mailto:…` compose URL on
  the organization's Zoho data center. The desktop Email page and the web
  Email card list them
  ([Marketing README](../../marketplace/packages/marketing/README.md#suggested-emails-on-the-email-channel-1250)).
- 2026-10-07: Marketing 1.24.1: the Google channel gets its link without
  setup. The user asked for the channel link on the Google channel and for it
  to be set up and refreshed. The link was filled only from the Analytics
  site host or the attribution landing page, so an organization with neither
  saw "Add the channel link" and Refresh answered `channel_url_required`.
  `_settings` now falls back to the instance's own host, the same host the
  referring-sites panel already treats as the site
  ([Marketing README](../../marketplace/packages/marketing/README.md)).
- 2026-10-07: nested packages in the Market. The user asked for the status
  checks to be added and removed with the one base Status Page package and
  shown nested inside it. A package nests under its first `dependencies`
  entry when that package is in the catalog (`market_parent` in
  `app/tools/build_market_catalog.py`, `PackageRegistry::marketParent` and
  `nestedPackages` on the desktop, `MarketModel.marketParent`/`nested` on
  iPhone and Android). Nested packages get no card or row of their own:
  `/market` lists them in a collapsible "Includes N packages" block inside
  the parent's card, the desktop Market card shows one tick box per nested
  package, and the phones show the count on the row and an "Included
  packages" switch list on the parent's page. Only the parent has Install and
  Remove; a nested package's switch is off and locked while the parent is
  removed. Search still finds a parent through its nested packages' names.
  Installing the parent on the desktop now also installs its nested packages
  that are on by default (`PackageRegistry::install`), the same as the
  account engine already did. Details: [status-page.md](status-page.md#market-nesting).
- 2026-10-09: Glasses Alert finds the glasses and shows it is working. The
  user asked for an arrow towards the glasses, a live closeness indicator, a
  small indicator over the whole app while watching, and icons for every
  other Bluetooth device heard so they can see detection running. One phone
  cannot measure direction from Bluetooth, so the arrow comes from the
  compass and where the signal was strongest as the user turns (the body
  blocks it from behind). The logic is `GlassesHoming` and `GlassesCensus`
  in both phone cores, with tests. Details in the package README.
- 2026-10-07: `glasses-alert` (Glasses Alert) added for Android and iPhone.
  The user asked for a plug-in that uses Bluetooth scanning to find Meta
  glasses nearby and breaks through silence with lots of vibration. The
  company IDs come from the open-source Nearby Glasses and NoPeek detectors;
  the code is ForkMesh's own (Nearby Glasses is AGPL). The logic that matches
  adverts and decides when to alarm lives in the phone cores (`glasses/GlassesWatch.kt`,
  `FMTools/GlassesWatchModels.swift`) with tests on both. Android rings on the
  alarm stream at full volume and vibrates as an alarm, which silent mode does not mute.
  It switches total-silence Do Not Disturb to alarms only while ringing, if
  allowed. iOS plays through the ring switch with Core Haptics, but scans only
  in the foreground because iOS drops unfiltered scans in the background.
- 2026-10-07: the Aquarium package ([aquarium.md](aquarium.md)), the first
  package that covers the whole main window rather than adding a page. It
  draws over a picture of the window and plays with the Genie spider through
  a new host socket, `desktop/src/PetHostFeed.h`.
- 2026-10-07: package dependencies, account installs and declared settings.
  The user asked to make the green-dots status monitors a plugin with
  settings, each check a dependent plugin, custom monitors, and an uninstall
  that removes it from desktop, mobile and web at once. Hosts now read
  `dependencies` (install pulls dependencies in, removal takes dependents
  out), `install.sync: "account"` (install state and settings on the account,
  `/api/plugins/@account`) and top-level `settings` (boolean and choice,
  rendered by each host). Status Page 2.0.0 and 18 `status-check-*` packages
  use them; see [status-page.md](status-page.md).
- 2026-10-07: desktop Market search. The user asked for search on the
  ForkMesh Market on desktop; the top search box only finds installed
  packages. The Market page header now has a **Search the Market…** box
  (`marketSearch`, `MainWindow::applyMarketSearch` in
  `desktop/src/MainWindowMarket.cpp`). It hides the cards on all three tabs
  that do not contain every typed word in the package's id, name, summary,
  creator handle or name, category, type, licence, price label or permission
  labels (case-insensitive). Each tab then reads `All (3 of 20)`, and a line
  says when nothing matches. Cards are filtered in place, not rebuilt, so
  typing reloads no screenshots, and a catalog refresh keeps the query. The
  `--package-only` window test checks a match, a miss and clearing the box.
- 2026-10-07: Marketing 1.21.0: the special agent on a channel page.
  Issue 1764 asked for a channel with a special agent attached to show it on
  the channel's page. A special agent definition lists its channels in
  `marketingChannels`; the page shows each attached agent's status, schedule
  and focus with Open, Run now and a message line, through the new
  `specialAgents` and `specialAgentAction` host functions, which only a
  package declaring `desktop.agents.run` receives
  ([Marketing README](../../marketplace/packages/marketing/README.md#special-agent-on-a-channel-page-1210)).
- 2026-10-07: Marketing 1.19.0: Mastodon people to follow and compact
  channel pages. The user asked for the desktop's Mastodon page to suggest
  people to follow with one-click follow and searches to check with links,
  and for the channel pages to be more compact with more information. A new
  `GET/POST /api/marketing/{org}/mastodon` ranks people from the home
  server's suggestions, hashtag timelines and account search, and follows
  with the channel's access token (owners and admins); every channel page
  gets a smaller header, eight compact KPI cards and tighter panels
  ([Marketing README](../../marketplace/packages/marketing/README.md#mastodon-people-to-follow-and-compact-channel-pages-1190)).
- 2026-10-07: Browser Extension 2.12.1: X posts on the signed-out profile.
  The user reported that **Update X stats from this page** added no links
  although a new post was on screen. Signed out, X serves a different page
  with no `data-testid` attributes and no `<time>`, so `x.js` read only the
  following and followers links and stored no posts. It now reads that page's
  articles, text, `data-engagement-action` counts, "posts" header and, for the
  date, the post id
  ([extension README](../../marketplace/packages/browser-extension/content/extension/README.md#x-profile-stats-for-marketing)).
- 2026-10-07: Marketing 1.18.0 and Browser Extension 2.12.0: guided Connect
  X. The user asked for the X API key entry to guide them by connecting to
  the browser extension, walking through each page with guidance and
  automating as much as possible. The Connect X dialog on web and desktop
  starts the extension's guide, which opens X's developer console, rings the
  next control and presses it, fills in the user authentication form for Read
  and write, regenerates the keys, reads all four off X's windows and saves
  them through `POST /x-connection`; the desktop updates itself once X is
  connected
  ([Marketing README](../../marketplace/packages/marketing/README.md#connect-x-with-the-browser-extension-1180),
  [extension README](../../marketplace/packages/browser-extension/content/extension/README.md#connect-x-for-posting-2120)).
- 2026-10-07: Browser Extension 2.11.0: live X posts. The user asked for
  **Update X stats from this page** to add the latest posts and keep updating
  in real time. The click now also sends the posts read on the profile to
  Marketing's posted links and makes the tab live: new posts and counts read
  there are sent within ten seconds, and a changed header count is recorded
  as a new X snapshot at most once a minute, until **Stop live updates**
  ([extension README](../../marketplace/packages/browser-extension/content/extension/README.md#x-profile-stats-for-marketing)).
- 2026-10-07: Marketing 1.17.0 and Browser Extension 2.10.0: X following
  count and post images. The user asked for the X stats to keep track of the
  following count too, and for the extension to read the most recent posts on
  the profile with their images. The extension keeps each post's photos, video
  posters and link-card images (up to four, from `pbs.twimg.com` only), shows
  the newest posts with thumbnails in its X widget, and sends the images with
  the posted links; the desktop's X channel page charts and tabulates
  **Following** and shows each posted link's first image
  ([Marketing README](../../marketplace/packages/marketing/README.md#x-following-and-post-images-1170),
  [extension README](../../marketplace/packages/browser-extension/content/extension/README.md#x-profile-stats-for-marketing)).
- 2026-10-06: Marketing 1.16.0 and Browser Extension 2.9.0: posted links.
  The user asked to add links to posts on any marketing channel by hand on the
  desktop and track them, and for X to scroll the profile in the browser and
  have the extension read every post's stats and add the links. Marketing
  keeps posted links (`/posts`) with views, likes, replies, reposts and a
  reading history, on every channel; the desktop's Channels page and each
  channel page list them with **Add link**. The extension's X **Sync now**
  now adds every post read while scrolling the profile, or updates its counts
  ([Marketing README](../../marketplace/packages/marketing/README.md#posted-links-1160),
  [extension README](../../marketplace/packages/browser-extension/content/extension/README.md#x-profile-stats-for-marketing)).
- 2026-10-06: Marketing 1.15.0 and Browser Extension 2.8.0: monitored Reddit
  threads. The user asked to add threads and monitor their views through the
  extension, starting with their reply in r/devops's weekly self-promotion
  thread, and to see them on the Marketing dashboard. A desk thread can now be
  monitored: the extension's popup adds one from a thread or comment link,
  reads its views, points and comments (and the comment's) in the user's own
  signed-in Reddit tab, since Reddit shows views only to the poster, and sends
  each reading to the desk, which keeps a history and shows the views and
  their change on a **Monitored** tab, on the web and, as the user then asked,
  on the desktop, which can also add a monitored thread from a link.
  **Check now** and an opt-in six-hour check open each thread in a background
  tab to read it. Comment links in the
  `/comment/<id>/` form no longer become a broken thread address
  ([Marketing README](../../marketplace/packages/marketing/README.md#monitored-threads-1150),
  [extension README](../../marketplace/packages/browser-extension/content/extension/README.md)).
- 2026-10-06: Browser Extension 2.7.0: a **This page** card at the top of
  the popup. The user asked that on Reddit or any channel's site the popup
  show the buttons for what the extension can do there to get the site's data
  and work with it; the Reddit desk and channel actions sat in collapsed
  widgets that did not say whether the open page counted. The card reads the
  tab's address against the desk's watch list (`fm-reddit-stats` now returns
  `watched` and `watchedQueries`) and the Marketing channels, and offers Send,
  Refresh, rules, Update and Sync. The user then asked that every page
  matching one of the channels get **Update _channel_ stats from this page**,
  so it is on every page of a channel's site (the server already accepts any
  host of the channel). Off the saved page the card warns that this page's
  numbers become the channel's and adds **Update from saved page**.
- 2026-10-06: Marketing 1.12.0 and Browser Extension 2.6.0: the Reddit
  channel guides people to its numbers. The user asked for the Reddit channel
  and the extension to walk them through getting Reddit's stats. Reddit's
  **Refresh** only imports posts from the public feed (Reddit gives servers no
  counts), so the channel sat on "Stale numbers" and the desktop rail's next
  step, Refresh, never fixed it. The worker now serves a three-step
  `health.guide` for Reddit (add the subreddit link, install the extension,
  press Update) that the web card and the desktop rail show until every step
  is done (on the desktop also in the channel table's Status cell and
  tooltip, with Reddit's members and weekly numbers labelled, since the user
  asked for it in the desktop app too), and the extension reads a subreddit's
  header itself: members,
  weekly visitors (`reach`) and weekly contributions (`engagements`), never
  the post scores on the listing. `/capture` accepts `reach` and
  `engagements` for this
  ([Marketing README](../../marketplace/packages/marketing/README.md#reddit-channel-numbers-1120),
  [extension README](../../marketplace/packages/browser-extension/content/extension/README.md)).
- 2026-10-06: Marketing 1.14.0: GitHub stars, forks and followers. The user
  asked to show GitHub's stars, followers and forks in the marketing counts.
  The GitHub refresh had folded stars into `followers` (for a repository
  link) and stars plus forks into `engagements`; it now records `stars`,
  `forks` and `followers` separately, the tile and rail lead with stars, and
  audience growth for GitHub follows `health.starsChange`. Asked for the
  desktop app too, the desktop channel page charts stars and forks, lists
  them in its recorded numbers, and its Log numbers dialog takes them
  ([Marketing README](../../marketplace/packages/marketing/README.md#github-stars-forks-and-followers-1140)).
- 2026-10-06: Marketing 1.13.0 and CRM 1.22.0: an Email channel. The user
  asked for an email marketing channel connected to the organization's Zoho
  that tracks outbound email and closed deals. The catalog's `email` entry
  is now Email and a starter channel; its Refresh reads
  `crm.email_channel_report` in-process: outbound email in 30 days (CRM
  sends, outreach, Zoho Mail Sent folder), linked replies, and contacts at a
  won pipeline stage with their deal value. Zoho stays the CRM's connection
  because package secrets and outbound origins are per package. The CRM's
  Zoho sync now keeps half its limit for the Sent folder
  ([Marketing README](../../marketplace/packages/marketing/README.md),
  [CRM README](../../marketplace/packages/crm/README.md)).
- 2026-10-06: Marketing 1.11.0 and Browser Extension 2.5.0: a Google
  channel. The user asked for a Google marketing channel that tracks visits
  from Google and how many pages `site:forkmesh.com` lists. The catalog's
  `google-search` entry is now named Google (the id stays, so content,
  metrics and exports keep working). Its numbers are two new snapshot fields:
  `visits` (sessions from Google in the last 30 days, from Analytics) and
  `indexedPages` (the `site:` result count, read by the extension in the
  person's own browser, since Google refuses server-side searches)
  ([Marketing README](../../marketplace/packages/marketing/README.md)).
  The user then asked for it on desktop and phones and in the desktop
  channel rail: Google became a default channel so every organization's rail
  has its square, the desktop table gained Visits and Pages on Google
  columns, and the iOS and Android Channels rows show both numbers.
- 2026-10-06: Analytics 1.1.0 and Marketing 1.9.0: Marketing gains a
  Performance page and a report section that read the organization's
  Analytics package through a new `campaigns` query: visits, bounces and
  conversions per UTM campaign, creative and landing page, joined with the
  Ads manager's spend and the Reddit desk's subreddits, plus a ranked "where
  to improve" list. The user asked to turn Analytics on and connect it to
  marketing to see how effective campaigns are and where to improve; until
  now the Growth scorecard and ad results were typed in by hand. Marketing
  calls `analytics.campaign_report` in-process instead of over HTTP because
  both modules are staged flat into the same worker and the caller has
  already checked membership. No new tables
  ([analytics README](../../marketplace/packages/analytics/README.md),
  [Marketing README](../../marketplace/packages/marketing/README.md),
  [../app/analytics.md](../app/analytics.md)).
- 2026-10-06: Marketing 1.8.0: refreshing the Discord channel uses the
  organization's Discord connector. The user asked for the Discord marketing
  channel to be updated through the connector: the public invite gives counts
  only while an invite link is saved and live, and the widget gives no member
  count, whereas the bot the owner already authorized can read the server's
  approximate member and online counts directly. The host adds
  `runtime.discord_guild(org_bi)` (manifest permission
  `organization.discord.read`, package installed in the org, current owner
  grant), backed by one new allowed bot path
  `GET /guilds/<id>?with_counts=true` whose response is cut down to name,
  description, vanity code and the two counts before it leaves the provider
  adapter. Without a connector the refresh still reads the public invite or
  widget ([Marketing README](../../marketplace/packages/marketing/README.md)).
  Browser Extension 2.4.1's Discord **Update** calls the same `refresh`, so it
  gets the connector's counts too.
- 2026-10-06: Browser Extension 2.4.1: Marketing's **Update** for Discord
  no longer opens the Discord web app in a tab. Opening it, reading it and
  closing it a second later logged the person out of Discord (the web app
  keeps its sign-in token out of storage while a tab is open and writes it
  back as the tab unloads, so a short-lived extra tab can lose it). The
  extension now asks the worker's `refresh` for the `discord` channel, which
  reads the public invite API (`approximate_member_count`,
  `approximate_presence_count`) with no Discord session, and the popup
  neither reads a Discord tab nor asks for Discord site access
  ([extension README](../../marketplace/packages/browser-extension/content/extension/README.md)).
- 2026-10-06: Marketing 1.7.0 and Browser Extension 2.4.0: every channel has
  an **Update** button (web dashboard table and the new Channels page, the
  desktop channel table, the iOS and Android Channels lists). The extension
  opens the channel's saved link, reads the counts the page shows and records
  them through the new `POST /api/marketing/<org>/capture`. The user asked
  for a one-click "update with extension" on all channels, on desktop, mobile
  and web, and for the package to hold everything needed to run marketing,
  organized. Only the browser can read signed-in channel pages and most
  networks have no free API, so the person's own browser is the reader (as
  for the Reddit desk). Desktop and phones cannot reach an extension, so they
  open the web page with `capture=<id>` and the page starts the update; phone
  browsers have no extensions, so there the page falls back to manual
  logging. The worker now computes per-channel health and 30-day growth and
  serves a playbook per category, so every surface shows the same checks.
  Site access stays optional and per site (granted on `capture.html` or all
  at once from the popup) rather than a standing permission on every site
  ([Marketing README](../../marketplace/packages/marketing/README.md),
  [extension README](../../marketplace/packages/browser-extension/content/extension/README.md)).
- 2026-10-06: Marketing 1.6.1 adds **Copy rules for the scout** to the
  Reddit desk's rules panel: the rules on file as dated Markdown for a Reddit
  scout run prompt, so the scout gets the rules without a session on the desk.
- 2026-10-05: Marketing 1.6.0 adds a Reddit desk on the web surface: a
  watch list of subreddits and searches, a shared log of Reddit threads to
  answer (new table `marketing_reddit_threads`) whose replied state needs the
  person's own rules and disclosure checks and the reply's address, a tagged
  link per thread, and the watched subreddits' rules. Browser Extension 2.3.0
  fills it: `reddit.js` reads the watched subreddit listings, watched
  searches, threads and rules pages the person opens on `www.reddit.com` and
  `old.reddit.com`, and the popup's **Send to Reddit desk** logs them as
  threads to review and saves the rules (`/api/marketing/<org>/reddit-rules`,
  any member). The operator asked to link the desk to the browser extension
  to get the data that way, because ForkMesh's servers get 403 from Reddit;
  the person's own browser, reading only pages they open, is the reader.
  Nothing posts, votes or sends anything to Reddit
  ([Marketing README](../../marketplace/packages/marketing/README.md),
  [extension README](../../marketplace/packages/browser-extension/content/extension/README.md)).
- 2026-10-04: Marketing 1.5.0 adds a Growth page on the web surface: a
  weekly funnel scorecard that keeps unmeasured numbers empty instead of zero,
  a backlog of experiments scored by impact, confidence and ease, a UTM link
  builder that checks links against the organization's attribution
  allowlists (the Ads manager runs the same checks), and a Markdown weekly
  report. The user asked the Marketing special agent to make the package
  useful for running marketing; until now the funnel, backlog and allowlist
  checks lived only in `docs/marketing/plan.md` and were done by hand. Two
  new package tables, `marketing_experiments` and `marketing_funnel`. The
  desktop surface does not have the page yet
  ([package README](../../marketplace/packages/marketing/README.md)).
- 2026-10-04: Design System (`design-system`, on by default). The user asked
  for one place showing the desktop, mobile and web design specs with every
  icon and button, at `/design`, linked from desktop and mobile, and removable
  as a plugin. Two host sockets were added for it rather than package-id
  branches: `www.page` (the website stages a package page) and desktop rail
  links (`browserPath`). The inventory is generated at website build time from
  `Theme.h`, `icons.qrc`, `styles.css` and the phone theme files, because a
  hand-kept spec drifts from the apps. The phones have no generic per-device
  switch, so `PhonePackages` gained `design-system` on iOS and a first
  `PhonePackages` on Android (`packages/design-system/on`, default on); the
  phone Market row now carries the `www.page` route as its web surface.
  `website_fingerprint` (`tools/next_autodeploy.py`) now hashes the
  `marketplace` tree as well as `www`, so a package page change redeploys
  forkmesh.com; the www deploy target lists the inventory sources too.

- 2026-10-02: Sugar Break (`sugar-break`), account-scoped like Water Intake
  (a random key per account, breaks and events sealed with it,
  `purge_account_data` on account deletion). The user asked for a sugar break
  with a set length, one month by default, started with a button, and a +1
  for every time they skip sugar. So a break is a start day plus a length
  (30 days unless chosen otherwise), and +1 is an event rather than a daily
  tick: a day can hold several skips, and a day is sugar-free when it has a
  skip and no slip. Apple Health has no "sugar-free day" type, so the iPhone
  only reads Dietary Sugar (grams) and shows it beside each day; it never
  writes to Health and never decides a day on its own. Worker permission ids
  use `account.sugarbreak.*` because permission ids may not contain hyphens.
- 2026-10-01: CRM (Sales pass): the investor deck is a hard block outside an
  investor pipeline; links are `?list=<id>` / `?contact=<id>` without the
  organization, resolved by `GET /api/crm/-/open/<id>`, and a partner with no
  CRM organization opens a sheet shared with them in shared-only mode; new
  views (My queue, Waiting on us, Possible duplicates, Paying members, Stage
  behind evidence) and bulk assign/tag. A package may now declare the
  `platform.accounts.evidence` permission: the build copies every manifest
  permission id into the registry (`plugin_data.permissions(plugin)`), and only
  a package declaring it may call the host's `account_evidence(actor, emails)`,
  which answers only for a ForkMesh administrator (`_is_admin`, TOTP enrolled)
  and returns, per address with an active user account, the account name,
  signup date and paid (platform `role_grants`), prize (promotion
  `role_grants`) and trial (`plan_trials`) windows. Details:
  `marketplace/packages/crm/README.md`.
- 2026-10-01: Sleep (`sleep`), account-scoped like Pedometer (a random key
  per account, nights sealed with it, `purge_account_data` on account
  deletion). A night is keyed by the day you woke up and keeps one reading per
  source; the night shows the hand-added reading first, then Apple Health
  (an Apple Watch or another sleep app), then the phone's own detection. The
  iPhone works the phone reading out from Core Motion's activity history: the
  longest stretch around the account's bedtime and wake time where the phone
  lay still (stationary, medium or high confidence), minus the times ForkMesh
  itself was open, with short gaps (20 minutes, or 75 inside the schedule)
  counted as awake time. Details and the reasoning live in the package
  README. Detected nights can be written back to Apple Health as
  `asleepUnspecified` samples tagged with the night, so a later detection
  replaces them. The desktop rail page uses the existing `moon` octicon.
- 2026-10-01: the iOS and Android Market showed "It was removed or you no
  longer have access." and listed nothing. Both apps read
  `GET /api/market/packages`, which was written on Next's
  `agent/native-ws19-packages` branch (60d3c66) and never merged, so the app
  answered `not_found`. The route is ported, and its rows come from the staged
  registry: a runtime read of `marketplace/catalog.json` would find nothing on
  the core nodes. The phone apps can also remove an organization package now
  (`POST /api/plugins/<org>/<package>/uninstall`, owner or admin). The server
  deletes the data in the same request and returns the only copy, so the phone
  opens a save dialog for it straight away and keeps a Save button until the
  user saves or discards it. A `plugin_not_installed` answer counts as removed.
- 2026-09-30: `cleanup` (Disk Cleanup) package: a rail page listing Claude
  Code, Codex, worktree, build, sandbox and cache debris with In use /
  Attached / Orphaned / Protected status and confirmed, guarded deletion;
  see [cleanup.md](cleanup.md).
- 2026-10-02: the CRM's **Default** table view (web and desktop) ends with
  **Last contacted** (`lastContactedAt`), so every list shows each contact's
  last activity without opening the Columns menu or switching to **All**.
- 2026-09-30: CRM 1.20.0 (Sales pass over the forkmesh CRM): every contact
  write now places the contact in a stage its flow has, not only imports; the
  bulk edit carries `owner`, and the web toolbar filters by owner and
  follow-up; **Connect Zoho Mail** is one click through a ForkMesh Zoho app
  (`package_secret` reads only `PACKAGE_<PLUGIN>_*` host variables); a sheet
  shared for editing lets partners outside the organization add contacts and
  move stages in that one pipeline; preparing the investor deck for a contact
  outside an investor pipeline asks first. Details:
  `marketplace/packages/crm/README.md`.

- 2026-09-30: CRM 1.19.1: a CSV import stored every row without a stage as
  `lead` and every row without a flow as `customers`, whether or not the
  organization's flows had them. A contact whose stage its flow lacks sits in
  no stage column of the pipeline, which stranded 315 forkmesh contacts.
  `_upsert_contact` (CSV import and **Copy** of a shared sheet) now places
  each row with `_flow_placement` against the saved flows: an unknown flow
  becomes the first flow, and a missing or unknown stage becomes that flow's
  first stage. It applies to merged rows too, so re-importing the same file
  moves already-stranded contacts into their flow's first stage. Creating,
  editing and bulk-moving a contact through the API still write the stage
  they are sent unchecked.

- 2026-09-29: Pedometer (`steps`) and Water Intake (`water`), both
  account-scoped like Calorie Counter (a random key per account, rows sealed
  with it, `purge_account_data` on account deletion). The iPhone app is the
  only writer of steps: it reads daily sums from HealthKit (and the motion
  sensor for the last seven days) and posts them to `/api/steps/sync`; web and
  desktop show them. Water is logged anywhere; the iPhone writes each drink
  to Apple Health and imports water other apps logged there. Calorie Counter
  gained the same Apple Health switch on iPhone, with no Worker change. The
  shared reconcile (`HealthReconcile`, `HealthBridge`) lives in
  `native/ios/ForkMeshCore/Sources/FMTools/HealthModels.swift`; the HealthKit
  code in `native/ios/ForkMesh/Platform/HealthKitBridge.swift`. New desktop
  rail icons `footprints` and `drop` are ForkMesh drawings under
  `desktop/resources/icons/forkmesh/`, served from the octicons prefix.
- 2026-09-29: Docs package (`docs`, on by default): the whole `www/docs`
  library on a desktop rail page, matching the web library's areas, order,
  titles and summaries (`www/tests/test_docs_library.py` pins the guide
  order, areas and reserved slugs to `app/tools/docs_site.py`). New
  `build.json` field `repositoryResources` embeds a repository folder.
  `FIRST_PARTY_PACKAGES` in `app/src/marketplace_reviews.py` also gained the
  missing `backups` and `tour`.
- 2026-09-28: package GETs follow allowed-origin redirects in
  `_PluginApiRuntime._outbound_text`. Before, every 3xx was refused, so the
  Marketing Blog channel set to `https://forkmesh.com/blog/` (308 to `/blog`)
  failed its refresh with 502 before feed discovery could run
  (`app/tests/test_plugin_outbound.py`).
- 2026-09-28: CRM 1.18.0: desktop contact pictures come first from the
  ForkMesh user with the contact's email address (the administrator user
  directory's `avatarPng`, read through `userRecords` at most every fifteen
  minutes) and last from Gravatar (`gravatar.com/avatar/<sha256>?d=404`), after
  Mastodon, Bluesky and GitHub. A 404 picture is cached on disk as an empty
  file for the week, so Gravatar misses are not re-asked on every load.
- 2026-09-29: Browser extension 2.2.1 and CRM 1.19.0: a Zoho message linked
  from the Spam folder was logged with the subject "Spam": the reading pane's
  subject selectors missed, and the fallback was the tab title, which is the
  folder name. The Zoho reader now rejects folder names, also looks in the
  selected message row, and the panel's subject is an editable field. A
  sender shown only by address gets the name beside it in the header. The
  worker stored `fromName` only when the request carried it, which the
  extension never did; an inbound link now falls back to the contact name
  sent, then the record's name. The desktop **All activity** tab's Contact
  column showed the message's `to`, which is empty on received mail; it now
  shows the linked contact's name. The **Email ID** column (was Provider ID)
  opens the message on a single click. Every click on a list or pipeline tab
  sets the stage filter to **New** (else its first stage); a reload, which
  does not change tabs, keeps a stage picked by hand, and a top-search pick
  switches to the contact's stage.
- 2026-09-29: Browser extension 2.2.0: the panel links an email to a contact
  picked from the CRM (**Link to an existing contact…**, or **Link to
  contact** in the header), for a reply from an address the CRM does not know
  yet. The picker reads `GET /contacts` once per panel (now with flow and
  stage) and suggests records sharing the address's domain, plus-less local
  part, company or name; the link sends `contactId`, which `POST /emails/link`
  already took, and the worker adds the address to that record's email or
  **Other emails**. No CRM release was needed.
- 2026-09-28: Browser extension 2.1.0: mail sent from Gmail or Zoho Mail,
  including the compose windows the CRM opens from a contact's address, is
  linked to the first recipient who already has a CRM record, without a
  click. It reuses `POST /emails/link` with `createContact: false`, so a send
  to strangers creates nothing, and waits ten seconds (or the tab closing) for
  Gmail's undo period and its "View message" id. On by default; the popup's
  **Track emails I send** turns it off. No CRM release was needed.
- 2026-09-28: CRM 1.18.0: the contacts table has views. Every list opens in
  **Default** (Name, Email, Mastodon, Bluesky, Twitter, LinkedIn, GitHub);
  **All** shows every column the list carries; custom views are named column
  layouts shared across the organization's lists, stored in the settings as
  `tableViews`, which any member may save. Column edits in the table now change
  the open view instead of the list's own layout.
- 2026-09-28: CRM 1.17.0: the rail's CRM button shows how many contacts have
  never been contacted (`lastContactedAt` unset). Native pages get
  `navigationBadge()`, `prepareNavigationBadge()` and the host's
  `navigationBadgeChanged` for a rail badge.
- 2026-09-28: CRM 1.16.0: **Copy link** on a contact (web card, web and
  desktop right-click menu) copies a `/dashboard/crm?org=&contact=` address that
  opens the contact's card for organization members, and on a list (web tab
  menu and sheet dialog, desktop list menus) a `?org=&list=` address that opens
  the list for everyone it is shared with; the web heading menu can
  now delete built-in columns (owners and admins) and restore them, as the
  desktop already could.
- 2026-09-27: `calories` analyze and receipt answer 503 `ai_unavailable`
  (with `reason`, `retryable`, `retryAfterMs`) instead of 502 when host AI
  fails or is not configured; see
  [docs/app/package-ai-failures.md](../app/package-ai-failures.md).
- 2026-09-27: `backups` package: one rail page that organizes the desktop's
  snapshots, kept snapshots, package data copies, restore leftovers and the
  fleet database backups; snapshot/restore stay in the host behind
  `desktop/src/BackupHostFeed.h`.
- 2026-09-25: rail right-click "Open in new window" for native package pages
  (Watch and the rest).
- 2026-09-27: CRM 1.15.0: a team leaderboard and a daily activity chart
  (emails, outreach, contacts added per member, in the viewer's time zone) on
  the web Team tab and the desktop Team leaderboard page.
- 2026-09-26: CRM 1.14.0: desktop lists move to a sidebar with faces and a
  stage bar, contacts open in a side panel, a People grid, list merging,
  built-in columns can be deleted, and lists sharing a pipeline no longer
  overlap.
- 2026-09-23: `analytics` package replaces PostHog (12317aa01). X/Reddit boards
  become opt-in (7cc28ee45).
- 2026-10-02: `speech` (Speech Playground) added, iOS first: the first package
  whose only surface is the iPhone app. It declares `entrypoints.ios`, which
  the hosts ignore, so the desktop Market shows it as "Catalog only".
- 2026-09-27: `calories` 2.3.0 food search autocompletes as you type, brand-aware,
  on Open Food Facts' search service.
- 2026-09-27: `calories` 2.4.0: log by typing in the prompt box on web,
  desktop, iOS and the Flutter app, a Quick add panel, and receipt photos that
  fill My foods. Desktop native pages gain `hostPromptTarget()`.
- 2026-09-22: `voice-changer` added, moved from browser extension to desktop
  package, made free; Polar package purchases removed (3431ce43d, ebaea8b83,
  5c7e2e1a7). Live static fixed with a rate matcher and noise reduction
  (1d54c2ff7, 9bbdcf889).
- 2026-09-21: rail right-click remove with data export, import on install
  (5a4da5daf). CRM 1.13.0.
- 2026-09-18: `vault` added; `browser-extension` split out of `crm`
  (e2d9e2568). Top search asks every package (ab8fd14f4).
- 2026-09-17: `calories` 2.0 per-account encrypted diary; marketing ads manager.
- 2026-09-13: `timers` package; Sky Office moved into a district package.
- 2026-08-28: generic plugin sockets; native packages self-contained.

## Related docs

- [`marketplace/README.md`](../../marketplace/README.md): package format, lifecycle, publishing.
- [`docs/marketplace-extraction-inventory.md`](../marketplace-extraction-inventory.md): #653 per-candidate decisions.
- [`www/docs/plugins/README.md`](../../www/docs/plugins/README.md): planned plugins (#677).
- [`docs/desktop/README.md`](../desktop/README.md), [`docs/app/README.md`](../app/README.md), [`docs/world/README.md`](../world/README.md).
- 2026-09-28: CRM Mastodon and Bluesky lookups also read 406 and 410 as
  answers ("this instance no longer serves the API"), not failures. qoto.org
  redirects every path to `social.cleverlibre.org/about`, which now answers 406.
  The account lookup for a qoto.org contact therefore logged `net GET ERR 406`
  and pinged on every CRM refresh. The contact's handle is simply stale, the
  same answer as a 404 (`test_every_account_lookup_reads_a_dead_instance_as_an_answer`).

## Market page preview images (2026-09-30)

`app/tools/build_market_catalog.py` points each card's screenshot at
`/market/previews/packages/<package>/preview/<file>`, and the www build
(`stage_market_previews` in `app/tools/build_site_assets.py`) copies every
`marketplace/packages/*/preview/*.{png,svg,webp,jpg,jpeg}` there. The cards
used to load `/api/repo/forkmesh/forkmesh/raw?path=…`, which serves SVG files
as `application/octet-stream` with `Content-Disposition: attachment` and
`nosniff` (so a repository SVG can never run as a document); an `<img>` cannot
display that, so every SVG preview on forkmesh.com/market was blank. Static
copies are served with their image type, cache like other assets, and cost no
API reads. `app/tests/test_market_previews_static.py` fails if a card points
at the raw API again or at a preview the build does not ship.

## Desktop Market preview cache (2026-10-07)

Opening the desktop Market rebuilt every card, and each card loaded its
screenshot with `QPixmap(path)` and then scaled it. Half the previews are SVG,
so that rendered each one at its full width and height on the GUI thread,
twice per package (the All tab and the Installed or Not installed tab). With
about 55 packages one click on Market froze the window for 2.9 s (stall
`STALL-20261007T164635638`, sampled inside `QSvgTinyDocument::draw` under
`refreshMarketPage`). `MainWindow::marketPreviewPixmap` now reads each preview
once with `QImageReader::setScaledSize`, so an SVG is drawn straight at the
240×135 card size, and keeps the result in `m_marketPreviewCache`, keyed by
path, box and file time. Later opens, install-count refreshes and installs
reuse it. `test_window_resize` checks that a rebuild of the shelf decodes no
preview again and that the first open decodes at most one per package.

## Market preview images on phones (2026-10-01)

The native iOS and Android Market showed no images: `GET /api/market/packages`
(`app/src/market_packages.py`) sent no screenshots, and half the previews are
SVG, which neither `UIImage` nor Android's `BitmapFactory` decodes. Each row
now carries `screenshots: [{url, alt}]`, up to eight, built from the manifest's
`screenshots` list. The URL is absolute,
`https://forkmesh.com/market/previews/packages/<package>/preview/<file>`,
because only the www build stages previews (app.forkmesh.com answers 404 there).
A listed `.svg` is sent as its `.webp` sibling. A screenshot whose file is
missing, or whose path is outside `preview/`, is left out. `build_app_plugins.py`
passes the catalog root and manifest path, so the staged `MARKET_PACKAGES` of
deployed nodes carries the same list.

Every SVG preview has a committed WebP made by
`python3 app/tools/rasterize_market_previews.py`. The tool renders through
headless Chromium (`FORKMESH_CHROME`, `$PATH`, or Playwright's
`chrome-headless-shell`) at the SVG's own width and height, then saves WebP
quality 85 with Pillow; `--force` re-renders. The website still shows the SVG.
A new SVG preview without its WebP fails
`test_every_svg_preview_has_a_webp_for_phones`.

Since 2026-10-13 the phone raster is WebP, not PNG (2.1 MB to 0.84 MB for 81
files): iOS 17 and Android minSdk 26 both decode it. A package whose manifest
lists `preview/market.png` itself keeps PNG, because the desktop Market card
reads that file with `QImageReader`, and WebP needs Qt's optional
qtimageformats plugin, which the Windows package does not ship and many Linux
desktops lack. Those 19 PNGs were palette-quantized (256 colours,
Floyd-Steinberg) and capped at 1280 px wide instead, 9.9 MB to 3.2 MB.

Phones keep only `https` URLs (`PackageScreenshot`). The Market list shows the
first screenshot as a 16:9 thumbnail; on Android this replaces the extension
tile. The package page shows every screenshot above the summary, swiping
sideways when there is more than one. iOS loads them with `AsyncImage`.
Android uses `MarketImages` (`feature/market/MarketImages.kt`), which fetches
through `HttpClient`, downsamples to 320 px for thumbnails and 1280 px for the
page, and keeps decoded bitmaps in a 32 MB LRU cache.
