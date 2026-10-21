# Status Page package and its checks

`marketplace/packages/status-page` (Market id `status-page`) is the
green-dots status monitor: a dot per ForkMesh system from `/api/status` on the
desktop debug bar, under the version on iPhone and Android, and on the web's
dashboard page `/dashboard/status-monitors`. Each check is its own package
(`marketplace/packages/status-check-*`) that depends on it, the user can add
their own monitors, and the install state follows the account across every
device.

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

Every check is on by default except Mirrors, since a fleet of mirrors would
crowd the row.

The checks choose which dots a user sees; they do not start or stop the
server's sampler. `record_status_sample` keeps checking every system each
minute for the public `/status` page and the admin alerts, whatever any
account has installed. A row no installed check claims is hidden. A new
`/api/status` row needs a check package (or a `systems` entry in an existing
one), or nobody sees its dot; `test_checks_cover_every_status_row` fails
until it has one.

A third-party package can add dots the same way: depend on `status-page`,
set `install.sync: "account"`, and declare a `status.monitor` contribution.

### Empty collections are not outages

The `flagship_repository` probe reads what the repository page renders. A
collection with no records is a valid state the page shows as an empty tab, so
the probe must pass on it. On 2026-10-21 the last two discussions on
forkmesh/forkmesh were deleted, git dropped the empty `.forkmesh/discussions`
folder, and the probe's tree read of that folder answered 404, turning the row
red with "Discussions collection failed (HTTP 404)". The probe now reads the
`.forkmesh` tree at the pinned commit first, as it already did for the pull
ledger, and only reads a discussion when the folder lists one. Requesting the
absent folder itself cannot tell "not in this revision" from "no mirror served
it": the router only downgrades to 404 when every endpoint it reached agreed.

### Redacted private-repository errors keep their route family

The error log renames a failing private or unpublished repository route to
`/private-or-unpublished-repository/<kind>` (or `/repository-route-redacted/`),
where `<kind>` is `[api:<route>]`, `[git-info-refs]`, `[git-upload-pack]`,
`[release-blob]` or `[page]`. The sampler used to bucket by path prefix, so a
redacted `/api/repo/<o>/<r>/pulls` 503 no longer started with `/api/` and
painted the Website row red (2026-10-21, 17:56 to 18:16 UTC).
`_status_error_route_path` maps each kind back to a stand-in path of the same
route family before bucketing, so a redacted API error lands on API, a git
error on Realtime, a page error on Website, and a 502/503/504 on a mirror-served
content route is skipped exactly as its public form is.

### A new fleet node's uptime starts at its first sample

Core, runner and mirror rows appear when a node joins the fleet. The 30-day
uptime and coverage used to count every day of the window as expected, so a
core added on 2026-10-14 showed 25.78% uptime on 2026-10-21 while every one of
its daily bars was green. For those rows the days before the node's first
recorded sample are left out of the 30-day expected total. Their daily bars
still show no data. Fixed rows keep counting every day, because a gap there is
a missed sample, not a system that did not exist yet.

## Market nesting

Every Market nests a package under its first `dependencies` entry, so the
checks appear only inside Status Page: a collapsible "Includes N packages"
list on `/market` (`nest_packages`, `render_nested` in
`app/tools/build_market_catalog.py`), a tick box per check on the desktop card
(`marketNestedPackage-<id>`), and an "Included packages" switch list on the
phones. Removing Status Page removes every check; installing it brings back
every check that is on by default, in the account engine and in the desktop
registry (`PackageRegistry::install`).

## Account install state

Status Page and its checks are account-synced (`install.sync: "account"`;
rules in [core-plugins.md](core-plugins.md)). Status Page is
`install.default: false` with `installedBefore: "2026-10-11"`.

- `POST /api/plugins/@account/<id>/install` installs the package and its
  dependencies.
- `POST .../<id>/uninstall` stores it as removed, resets dependants to their
  defaults and calls `purge_account_data` for each (for Status Page this
  deletes the custom monitors).
- `POST .../<id>/settings` with `{"settings": {...}}` validates against the
  manifest's `settings` (400 `invalid_setting`).
- Every answer carries the full `packages` list, and install/uninstall add
  `changed`, so a client applies one response.
- Organizations cannot install account-synced packages; deleting the account
  deletes its rows (`plugin_data.purge_account`).

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

- Desktop: the debug bar dots and the package page
  ([desktop/diagnostics.md](../desktop/diagnostics.md)).
- iPhone and Android: footer dots and the package page
  ([native/NAVIGATION.md](../native/NAVIGATION.md)).
- Web: `/dashboard/status-monitors` (loads on open and Refresh only). Signed-in
  links to `/status` carry `data-status-entry` and are hidden while the
  account lacks `status-page` (`www/public/api-client.js`,
  `forkmesh.statusPage.v1`). Signed-out visitors and the public `/status`
  incident page are unaffected.

### Web layout

Every grid in `content/web/view.html` (the page body, the two-column row, its
right column, and the Checks, Settings and Your monitors lists) sets
`grid-cols-1`, so each track is `minmax(0, 1fr)`. A bare `grid` sizes its
implicit column to the widest row's min-content, and the one-line truncated
check summaries and monitor addresses then pushed every check row past its
card: the switches drew over the Settings column and the view scrolled
sideways. `.sp-switch` sets `min-height: 0` because the dashboard gives every
button `min-height: 44px` on touch screens, which stretched the switches into
tall pills on phones. Inputs and the select take the accent focus border.

### Event-only states

The error and notice boxes are shell `fm-flash` alerts (danger and blue info)
with one icon; `message()` writes into their `[data-status-message-text]` child
so the icon survives. The install panel is an `fm-blankslate`, Add monitor and
Install are the small green `fm-btn-cta`, and Remove Status Page and each
monitor's Remove are `fm-btn-danger` (red text, red fill on hover), matching
the dashboard rule that green is only for creating something.

## Tests

`python3 -m pytest marketplace/packages/status-page/tests app/tests/test_status_page_web.py` covers the
manifests, catalog validation, the account engine against the real generated
registry, and the custom monitors API.
