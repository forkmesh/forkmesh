# Status Page package and its checks

`marketplace/packages/status-page` (Market id `status-page`, 2.1.0) is the
green-dots status monitor: a dot per ForkMesh system from `/api/status` on the
desktop debug bar, under the version on iPhone and Android, and on the web's
dashboard page `/dashboard/status-monitors`. Each check is its own package
(`marketplace/packages/status-check-*`) that depends on it, the user can add
their own monitors, and the install state follows the account across every
device.

## Why

On 2026-10-07 the user asked to make the green-dots status monitors a plugin
with settings, to make each check a dependent plugin, to allow custom
monitors, and for an uninstall to remove it completely from desktop, mobile and
web, with an install bringing it back on all of them. Until then (1.0.0,
2026-10-02) the package only gated the desktop dots and the iOS footer, was
switched per device (`packages/status-page/on` on the phone), had no settings,
and Android had no dots. The manifest's `dependencies` field existed but no
host read it.

## Code map

| Path | Responsibility |
| --- | --- |
| `marketplace/packages/status-page/manifest.json` | Parent package: `install.sync: "account"`, declared `settings`, the `dashboard.page` contribution, and the `worker` block for `/api/status-monitors`. |
| `marketplace/packages/status-page/content/worker/status_monitors.py` | Custom monitors API, sealed per account in `status_monitor_lists`; `purge_account_data`. |
| `marketplace/packages/status-page/content/web/` | The web page: dots, checks, settings, your monitors, remove. |
| `marketplace/packages/status-check-*/` | One package per check: `dependencies: [status-page]`, a `status.monitor` contribution whose config (also in `content/monitor.json`) lists the `/api/status` row ids (`systems`) or row prefixes (`prefixes`) it shows. |
| `marketplace/tools/marketplace_lib.py` | `validate_settings`, `validate_dependencies` (unknown ids, cycles, mixed `install.sync`). |
| `app/src/plugin_data.py` | Account install engine: `/api/plugins/@account`, dependency resolution, settings validation, `account_package_installed`, account purge. |
| `app/src/market_packages.py` | `/api/market/packages` rows carry `default`, `accountSync`, `dependencies`, `settings`, `monitors`. |
| `app/tools/build_app_plugins.py` | Staged `PLUGINS` entries carry `accountSync`, `dependencies`, `settings`. |
| `app/src/entry.py` | `_PluginApiRuntime.account_plugin_installed(account_bi)`. |

## The checks

| Package | Rows |
| --- | --- |
| `status-check-website` | `website`, `desktop_website` |
| `status-check-world-mobile` | `world_mobile` |
| `status-check-status-page` | `status_page`, `desktop_status_page` |
| `status-check-api` | `api` |
| `status-check-errors` | `errors` |
| `status-check-database` | `database` |
| `status-check-email` | `email` |
| `status-check-repository` | `flagship_repository` |
| `status-check-release-linux`, `-android`, `-windows`, `-macos` | `release_<platform>` |
| `status-check-git-hosting` | `git_hosting` |
| `status-check-realtime` | `realtime` |
| `status-check-durable-objects` | `durable_objects` |
| `status-check-cores` | every `core:<node>` |
| `status-check-runners` | every `runner:<name>` |
| `status-check-mirrors` | every `mirror:<name>` (off by default) |

Every check is on by default except Mirrors: no surface showed mirror dots
before 2.0.0, and a fleet of mirrors would crowd the row.

The checks choose which dots a user sees; they do not start or stop the
server's sampler. `record_status_sample` keeps checking every system each
minute for the public `/status` page and the admin alerts, whatever any
account has installed. A row no installed check claims is hidden. A new
`/api/status` row needs a check package (or a `systems` entry in an existing
one), or nobody sees its dot; `test_checks_cover_every_status_row` fails
until it has one.

A third-party package can add dots the same way: depend on `status-page`,
set `install.sync: "account"`, and declare a `status.monitor` contribution.

## Market nesting

On 2026-10-07 the user asked for the checks to be added or removed through the
one base Status Page package and shown nested under it. Before that, the
Market listed each `status-check-*` package as its own card or row with its
own Install button, 19 entries for one feature.

Every Market now nests a package under its first `dependencies` entry when
that package is in the catalog, so the checks appear only inside Status Page:

- `/market` (`app/tools/build_market_catalog.py`, `nest_packages`,
  `render_nested`): a collapsible "Includes 18 packages, added and removed
  with it" list in the Status Page card, with each check's name, version,
  summary and repository link.
- Desktop (`MainWindow::refreshMarketPage`): the Status Page card has a tick
  box per check (`marketNestedPackage-<id>`), off and locked while Status
  Page is removed. The tab counts and search use top-level cards, and a
  search for a check's name shows the Status Page card.
- iPhone and Android: the Market row says "Includes 18 packages"; the
  Status Page screen has an "Included packages" switch list under the account
  switch. Turning a check off or on from that list skips the confirmation.

Removing Status Page removes every check, and installing it brings back every
check that is on by default (all but Mirrors). The account engine already did
this; the desktop registry now does too when it installs Status Page itself
(`PackageRegistry::install`), so an offline desktop agrees with the account.

## Account install state

`GET /api/plugins/@account` answers one row per `install.sync: "account"`
package: `{id, installed, settings, places, availablePlaces}`. `installed` is
effective: false while anything the package depends on is removed. Rows live in
`account_package_installs(account_bi, package, installed, settings,
updated_at, places)`, created on first use by `plugin_data` like a package table (no
core migration). With no row, a package is installed when its manifest says
`install.default`, or when it sets `install.installedBefore` and the account
was created before that date ([core-plugins.md](core-plugins.md)).

Since 2.1.0 (2026-10-10) Status Page itself is `install.default: false` with
`installedBefore: "2026-10-11"`: accounts created from 2026-10-11 start without
it (and so without any check, since every check depends on it), older accounts
keep it. The checks keep their own defaults, so installing Status Page on a new
account brings every check but Mirrors.

- `POST .../<id>/install` installs the package and, transitively, its
  dependencies.
- `POST .../<id>/uninstall` stores the package as removed, deletes the rows of
  every package that depends on it (back to their defaults, hidden while the
  parent is removed), and calls `purge_account_data` for each. For Status Page
  that deletes the custom monitors. Installing Status Page again brings back
  every default check, with default settings and no monitors.
- `POST .../<id>/settings` with `{"settings": {...}}` checks every key and
  value against the manifest's `settings` and answers 400 `invalid_setting`
  otherwise.
- Every answer carries the full `packages` list, and install/uninstall add
  `changed` (ids whose effective state flipped), so a client applies one
  response instead of reloading.
- `GET /api/plugins/` (the dashboard's Plugins column) lists account-synced
  packages by the same effective state.
- Organizations cannot install account-synced packages
  (`plugin_installs_per_account`), and they are left out of an organization's
  plugin list.
- Deleting the account deletes its rows (`plugin_data.purge_account`).

The desktop signs these calls like its other `/api/plugins` lifecycle calls
(`kPluginRequestProof`); the web and the phones use their session.

## Custom monitors

`/api/status-monitors` (proof `forkmesh-status-monitors-request-v1`): `GET`
lists `{id, name, url}`, `POST /monitors` adds, `PUT /monitors/<id>` edits,
`DELETE /monitors/<id>` removes. Names are 1 to 40 characters. URLs are http or
https with a host, no user or password, no spaces, at most 300 characters. An
account keeps at most 20. The list is one sealed row per account. While Status
Page is removed, `GET` answers `installed: false` with no monitors and writes
answer 409 `status_page_not_installed`.

The server never fetches these URLs: each device probes them itself (a GET
with a 10 s timeout, green below HTTP 400) and shows dot `custom:<id>`. That
keeps arbitrary addresses away from the core nodes' network, and shows what
that device can actually reach. A browser can only use a `no-cors` fetch, so
the web shows green when the address answered at all.

## Settings

| Key | Type | Default | Effect |
| --- | --- | --- | --- |
| `onlyProblems` | boolean | false | Hide dots that are green, custom monitors included. |
| `refreshMinutes` | choice `2`/`5`/`15`/`30` | `15` | How often a client rereads `/api/status` (and reprobes) while everything is green; anything not green keeps the faster cadence. |

## Surfaces

- Desktop: the debug bar dots, the account sync and the settings page;
  [desktop/diagnostics.md](../desktop/diagnostics.md).
- iPhone and Android: the footer dots (above the bottom bar on Android), the
  package page with the account switch, settings and your monitors;
  [native/NAVIGATION.md](../native/NAVIGATION.md) 2.0.72.
- Web: the package page at `/dashboard/status-monitors` (Plugins column)
  shows the dots in two rows (ForkMesh systems, your monitors), the checks
  with on/off switches, the settings (saved on change), your monitors, and
  **Remove Status Page**. It loads on open and on **Refresh** only, so
  `refreshMinutes` has no effect there, and it skips the desktop-only probe
  rows. Every signed-in link to `/status` (site header and footer, the
  "System status" pill, the dashboard sidebar, profile footer, home, mobile
  and hackathon menus) carries `data-status-entry`. `www/public/api-client.js`
  hides those while the account's `/api/plugins/` list lacks `status-page`
  (`rememberStatusPage`, `refreshStatusPage`, `statusPageShown`; cached per
  account in localStorage `forkmesh.statusPage.v1`, rechecked at most every
  5 minutes). Signed-out visitors always see them, and the public `/status`
  page and the links in alert pings and emails stay, because that page is
  ForkMesh's public incident page.

## Tests

`python3 -m pytest marketplace/packages/status-page/tests app/tests/test_status_page_web.py` covers the
manifests, catalog validation, the account engine against the real generated
registry, and the custom monitors API.
