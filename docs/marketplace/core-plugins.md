# Core features as plugins, off by default in new projects

Most of what the Home grid, the bar tabs and the repository tabs offer is a
Market package. Each package is the feature's on/off switch, Market listing
and install state; the feature code stays in each app (desktop, iPhone,
Android, web). New projects and accounts start with these packages removed,
except the phone starter set below; older ones keep what they had. Repos,
Search, Market and Settings stay core and always show.

The manifest field `install.installedBefore` (`YYYY-MM-DD`, with
`install.default: false`) installs a package for projects and accounts
created before that date, so turning a feature into a plugin never takes it
away from existing users.

## The packages

Every package below has `install.default: false`. Account-synced packages
(`install.sync: "account"`, `GET /api/plugins/@account`) keep their install
state on the ForkMesh account and follow every device.

- **Installed before 2026-10-11, account-synced:** `agents`,
  `special-agents` (depends on `agents`), `inbox`, `code`, `chat`,
  `bug-reports`, `issues`, `commits`, `branches`, `pull-requests` (depends on
  `branches`), `releases`, `actions`, `discussions`, `desktops`, `shortcuts`,
  `hosts`, `network`, `coves`, `on-device-ai`, `activity`, `stats`,
  `organizations`, `world`, `notes`, `tasks`, `calendar`, `ledger`, `tour`,
  `design-system`, `status-page`, `speech`, `glasses-alert`.
- **Installed before 2026-10-14, account-synced:** `workflows` (its
  `workflows-*` packs nest under it).
- **No `installedBefore`, account-synced:** `timers`, `vault`, `calories`,
  `steps`, `water`, `sleep`, `sugar-break`, `stretch`, `food-guide` (mobile
  only).
- **Per device:** `sign-in` (installed before 2026-10-11).

Each package gates its feature's surfaces in every app (tabs, tiles, rail
buttons, repository tabs). `crm` stays organization-installed; `market`,
`repos`, `search` and `settings` stay core.

Settings always keeps a way to sign in and out, so removing `sign-in` never
locks anyone out. The phone top bar avatar always shows; `sign-in` gates only
the guest's Sign in chip and the menu's Sign in / Sign out. On phones
`sign-in` is on until the user removes it, in new projects too.

## The phone starter set

A new phone project starts with `agents`, `chat`, `inbox`, `code` and `tour`
installed (iOS `PackageGate.starterPackages`, Android `PackageGate.STARTER`),
plus `sign-in`. Web and desktop keep the off-by-default rule.

Why: the 15 Oct 2026 audit found that installs since 11 Oct opened on
Repos, Search, Market and Settings only, with no Sign in chip for guests and
no tour, so a new user had nothing to do. The set is the three bar tabs, the
repository Code tab and the tour; the rest stays one Market install away.

- A starter package counts as installed in a new project until that project
  removes it. The account answer does not hide it, because a new account has
  no row for it and the server answers `installed: false` either way.
- Removing one in a new project records `false` for the project, as for any
  other package; installing it again works as usual.
- Before the catalog loads, a gated package with no entry is installed when
  the project is legacy or it is a starter package.
- Legacy projects are unchanged.

Nesting: every Market nests a package under its first `dependencies` entry, so
Pull requests sits under Branches, Special Agents under Agents, and the
`status-check-*` packages under Status Page. Installing a child installs its
parent; removing a parent removes its children.

## The effective-state rule

A surface that belongs to a package (Home tile, bar tab, rail button,
repository tab, sidebar link, search hit, command palette entry, assistant
route, quick action, recents row) shows only while the package is effectively
installed (it and everything it depends on are installed) and the app's own
place is in the package's places. Deep links and direct routes keep working;
hiding is about entry points.

### Account state

The account-level install state of one package, in order:

1. An account-synced package with an account row: the row decides.
2. No row: installed when `install.default` is true, or when `installedBefore`
   is set and the account (or project) was created before that date.
3. No row, and a package that keeps its own per-account data
   (`dataLifecycle.scope: "account"`) without `installedBefore`: installed
   when the package's `account_installed` hook says the account set it up
   through its own install panel.

The server marks answers from steps 2 and 3 with `"legacy": true`
(`_legacy_only` in `plugin_data.py`). "Created before the date" means the
account record's `created_at` (missing or unreadable counts as old) on the
server and web, the project's `project.json` `created` on desktop (the home
project counts as old), and a per-project legacy marker on phones (a project
added on the phone is marked new when it is added).

### Projects

The web has no projects and uses the account answer. Desktop and phones:

- A **legacy project** follows the account answer, `legacy` entries
  included.
- A **new project** keeps its own install list (desktop: the project's
  settings file; phones: `packages/project-installs` on iOS,
  `packages/projectInstalls` on Android). A package counts as installed when
  the project installed it and the account still has it, or when it is
  `install.default` and neither the project nor the account removed it, or
  (phones) when it is in the starter set and the project did not remove it.
  `legacy` answers never turn a package on there.
- Installing in a new project also installs on the account (so the web,
  settings and places work) and records it, with its dependencies, for the
  project. Removing in a new project removes it from that project only.
  Market wording follows ("Install in this project", "Remove from this
  project").

### Places

An account-synced install has places: `mobile`, `desktop` and `web`.

- `install.places` in the manifest lists where a package can show at all
  (left out, all three); Market rows and the `PLUGINS` registry carry it as
  `places`.
- `account_package_installs.places` (comma separated, empty means every
  available place) stores the user's pick. `GET /api/plugins/@account`
  entries carry `places` and `availablePlaces`.
  `POST /api/plugins/@account/<id>/install` takes an optional
  `{"places": [...]}`; dependencies get the same places.
  `POST /api/plugins/@account/<id>/places` changes them (400
  `invalid_places`, 409 `plugin_not_installed`).
- `GET /api/plugins/` (the web sidebar) lists an account-synced package only
  when `web` is one of its places. A dependency only has to be installed, not
  placed. An answer without `places` counts as every place.
- Every Market has a "Show on" row of Phone / Desktop / Web switches; the
  last one cannot be turned off.

### Server details

`app/src/plugin_data.py`:

- `_own_states` resolves each package's state from account rows, the
  fallback (`_fallback_installed`) and, for hook packages (`_hook_fallback`),
  the package hook.
- `account_package_installed(runtime, account_bi, plugin, created_at=None)`
  reads `created_at` only when needed, through the runtime's
  `account_created_at(account_bi)`; a runtime without it counts the account as
  old.
- Removing a hook package from the Market deletes its account row after
  `purge_account_data`, so setting it up again through its own panel counts
  as installed. A Market install writes a row.
- Organizations cannot install account-synced packages
  (`plugin_installs_per_account`).
- `GET /api/market/packages` rows and `plugin_registry.PLUGINS` carry
  `installedBefore` (`market_packages.installed_before`).

Tests: `marketplace/tests/test_core_plugins.py`; iOS `PackageGateTests`,
Android `PackageGateTest` and `AccountPackagesTest`; desktop
`test_package_registry.cpp`.

## Hosts

- Desktop: [docs/desktop/README.md](../desktop/README.md).
- iPhone: [docs/native/NAVIGATION.md](../native/NAVIGATION.md).
- Android: [docs/native/ANDROID.md](../native/ANDROID.md).
- Web: [docs/www/README.md](../www/README.md).
- Manifest field: [marketplace/README.md](../../marketplace/README.md#dependencies-account-installs-and-settings).
- Status Page specifics: [status-page.md](status-page.md).
