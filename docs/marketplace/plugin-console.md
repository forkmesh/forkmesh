# Plugin console (web dashboard)

Part of [the marketplace docs](README.md). Code: `app/src/plugin_console.py`
(API), `app/public/dashboard/js/09-plugins.js` and
`app/public/dashboard/partials/views/plugins.html` (page),
`market_packages.package_facts`, `build_app_plugins.registry_source`
(`facts`), `app/tests/test_plugin_console.py`.

`/dashboard/plugins` lets an administrator of an instance see and manage
every Market package (plugins and switches, [plugins.md](plugins.md)) from
the browser. The owner asked for it while ForkMesh moves to a slim core with
separate plugins: an operator needs one place that says what a build ships,
who installed what, which data a package keeps and whether its routes fail.
The desktop's own pages for module memory, rebuilds and installs from a
repository are separate work; this page is the web side.

The sidebar link carries `data-admin-only`, so it shows only for a session
with `isAdmin`; the API refuses everyone else anyway.

## The page

- **List.** Every package from `market_packages.payload()` (the staged
  `MARKET_PACKAGES` on a deployed App, the whole catalog in a source
  checkout). Each row: name, id, version, Plugin or Switch, what it ships
  (Worker, dashboard page, site page, desktop module or desktop code linked
  into the core, World), its available places, and how many accounts and
  organizations have it. A filter and a select narrow it to plugins,
  switches, packages installed for you, packages with a Worker module, or
  packages keeping server data. A package the build does not contain is
  marked "Not in this build".
- **Detail** (`?package=<id>`, tabs in `&tab=`):
  - *Overview*: the facts above plus data scope, install model, default
    and `installedBefore`, dependencies and dependents (links), install
    counts per place, and the organizations that installed it.
  - *Data*: the package's tables with row count and size, plus the core
    install records for it (`account_package_installs` rows for an
    account-synced package, `org_plugin_installs` rows otherwise). Browse
    opens a paged table viewer, newest rows first, 50 a page, values over
    400 characters cut.
  - *Logs*: `error_log` rows from the last 30 days whose path is the
    package's API prefix, its `/api/plugins/<id>` namespace, its dashboard
    route or its site route; and `sensitive_audit_log` rows for the package
    (installs, removals, settings, places, console reads and exports).
  - *Settings*: the settings the manifest declares (key, label, type,
    default, choices), its permissions and the outside origins its Worker
    may call.
- **Actions.**
  - *Install for me* / *Remove for me* and the places checkboxes call the
    existing `POST /api/plugins/@account/<id>/{install,uninstall,places}`,
    the same calls the Apps page makes, and refresh the sidebar.
  - For organization packages, type an organization name: the page reads
    `GET /api/plugins/<org>` and, for an owner or admin of it, offers
    install, remove (which downloads the organization's data first, as
    always) and the organization export.
  - *Export data* downloads every table of the package as JSON.

Colours follow `theme.json`: blue for links, focus, the selected tab and
the browsed table; no green button (the page creates nothing); Remove is
the destructive secondary button.

## API

All `GET`, all `no-store`, all behind a signed-in session whose account
passes `is_admin` (the `users.is_admin` flag with two-factor enrolled).
Anything else is `401 invalid_session` or `403 admin_required`. The routes
sit under `/api/plugins/@console`, which `plugin_data.handle` hands to
`plugin_console.handle`; an organization name cannot start with `@`, so the
path never collides with the organization routes.

| Route | Answer |
| --- | --- |
| `/api/plugins/@console` | `packages` (the list rows, with `mine`: your own effective install and places for account-synced packages), `staged`, `places` |
| `/api/plugins/@console/<id>` | `package` (plus settings, permissions, outbound origins, dependents), `tables`, `storage`, `installs` (per-place counts, organizations), `logs` (`errors`, `activity`, `prefixes`) |
| `/api/plugins/@console/<id>/rows?table=&offset=&limit=` | one page of one of the package's tables; `404 table_not_found` for any other table, `409 table_not_created` before it exists |
| `/api/plugins/@console/<id>/export` | every table of the package, at most 5,000 rows per table and 8 MB in all (`truncated` says when it stopped) |

Row reads and exports are written to `sensitive_audit_log`
(`plugin.console.rows`, `plugin.console.export`, target type
`package_console`) with the package and table in `details`.

### Which tables belong to a package

A package has no migration folder: its Worker module creates its own tables
(`DATA_SCHEMA_UP` and similar constants, or `CREATE TABLE IF NOT EXISTS` in
its code). The console reads the module's `*SCHEMA*` constants and its
source file and takes every `CREATE TABLE` name, then drops any name the
core schema (`schema.SCHEMA_STATEMENTS`) also creates, so the browser can
never reach `users` or another core table through a package. Only tables
present in `sqlite_master` are counted or browsed. Size is the sum of
`LENGTH` over every column, an estimate of stored bytes, not pages on disk.

### Activity needs the package in the audit details

The lifecycle audits used to hash `<account>:<plugin>` or `<org>:<plugin>`
into `target_token`, so nothing could find a package's history. Every
`plugin.install`, `plugin.uninstall`, `plugin.settings` and `plugin.places`
audit in `plugin_data.py` now also records `{"package": <id>}` in
`details`, and the console matches on that. History from before the change
does not show.

### Facts the Market row lacks

The Market row says whether a package has desktop code, not whether it is a
separate module, and folds the dashboard and site routes into one `web`
field. `market_packages.package_facts` adds `kind` (a package with
`content/` is a plugin), the Worker module, the dashboard and site routes,
`desktop` (`module`, `core` for `coreLinked`, or empty for a switch, whose
desktop code is core code) and World. The build stores it in the generated
registry as `facts`, because a deployed App has no `marketplace/` folder;
a source checkout reads the manifests (`catalog_facts`).

## Instance-wide places

The Overview tab's **On this instance** box sets where anyone on the
instance can use a package, on top of each account's own choice. The owner
asked for it so an operator can run a lightweight instance: keep a plugin
off the web, or turn it off altogether, without rebuilding.

- `POST /api/plugins/@console/<id>/policy` with `{"places": [...]}` (a
  subset of the manifest's places) or `{"reset": true}`. Same-origin,
  admin only, audited as `plugin.console.policy`. Choosing every place the
  manifest allows is the same as reset: the row is deleted.
- Stored in `package_place_policy` (`package`, `places` comma list,
  `updated_at`, `updated_by`). No row means no limit. The table is created
  on first save, like `account_package_installs`; reads treat a missing
  table as no rules.
- **Some places**: `plugin_data._available_places` is the manifest's places
  narrowed to the rule, so `availablePlaces` and `places` in
  `GET /api/plugins/@account` shrink, a stored account choice outside the
  rule is dropped from the answer (not from the row, so it comes back after
  a reset), `places` requests outside it are `400 invalid_places`, and the
  web sidebar (`GET /api/plugins/`) hides any package without `web`,
  account-synced or not. Desktop and phones already follow `places`, so
  they need no change.
- **No place**: the package is *off on this instance*. It is never sent as
  an empty `places` list, because the iPhone app reads an empty list as
  "every place" (`PackagePlace.list`). Instead `_effective_installed` is
  false for it and for every package that needs it, the account answer
  carries `"disabledHere": true` with `installed: false`, account and
  organization installs are `409 plugin_disabled_here`, organization state
  rows carry `disabledHere`, and its Worker API answers
  `404 plugin_disabled_here` (`plugin_api_handler` in `entry.py`). Its data
  and install rows are kept, so turning it back on restores everyone's
  installs.
- Each App process caches the rules for 30 seconds
  (`plugin_data.POLICY_TTL_MS`); the node that saves reloads at once, other
  nodes and processes follow within that time.

## Not covered yet
- Worker log lines that are not in `error_log` (the desktop's live
  Worker tail has them, [Logs](../desktop/README.md)).
- Per-account values of a package's settings.

## Running it locally

`python -m forkmesh_host build`, then `serve` with `--db sqlite:<path>`,
`FORKMESH_VAR_DATA_KEY=<test value>`, `FORKMESH_PUBLIC_HOSTS=localhost`
and `FORKMESH_VAR_APP_ORIGIN=http://localhost:<port>` (the host answers only
its configured names). `instance-bootstrap` creates an admin; the first
login answers `totp_setup_required` with the secret to enroll. Keep the data
directory in a temporary folder, never `~/projects/forkmesh/data`.
