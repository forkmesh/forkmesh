# Core features as plugins, off by default in new projects

Most of what the Home grid, the bar tabs and the repository tabs offer is now a
Market package. Each package is the feature's on/off switch, Market listing and
install state; the feature code stays in each app (desktop, iPhone, Android,
web). New projects and accounts start with these packages removed, older ones
keep what they had.

## Why

On 2026-10-10 the user circled almost every tile of the iPhone Home grid and
asked: "make sure these are full fledged plugins and turned off by default on
new projects on desktop, mobile and web, nest any that have dependencies". The
tiles that were not circled stay core and always show: Repos, Search, Market,
Settings. Workflows was core too until 2026-10-13, when it became a plugin
([below](#2026-10-13-where-each-plugin-shows-up)).

Turning a feature off for everyone at once would have taken it away from
people who use it, so the manifest gained `install.installedBefore`: a project
or account created before that date starts with the package installed; a newer
one starts with `install.default`, which is false for every package here.

## The packages

Every package below has `install.default: false`. Dates are
`install.installedBefore`; "none" means the package is simply off until it is
installed. "account" packages keep their install state on the ForkMesh account
(`install.sync: "account"`, `GET /api/plugins/@account`) and follow every
device; `sign-in` is per device.

| id | Surfaces it gates | Depends on | installedBefore | Sync |
| --- | --- | --- | --- | --- |
| `agents` | Agents tab, new agent button, agent lists | | 2026-10-11 | account |
| `special-agents` | Special Agents (`teammates`) | `agents` | 2026-10-11 | account |
| `inbox` | Inbox tab; desktop Pings | | 2026-10-11 | account |
| `code` | Code (`currentRepo`), Code tab | | 2026-10-11 | account |
| `chat` | Chat tab, chat rooms | | 2026-10-11 | account |
| `bug-reports` | Bug / Report a bug (`reportBug`) | | 2026-10-11 | account |
| `issues` | repository Issues tab | | 2026-10-11 | account |
| `commits` | repository Commits tab | | 2026-10-11 | account |
| `branches` | repository Branches tab | | 2026-10-11 | account |
| `pull-requests` | repository Pull requests tab | `branches` | 2026-10-11 | account |
| `releases` | repository Releases tab | | 2026-10-11 | account |
| `actions` | repository Actions tab | | 2026-10-11 | account |
| `discussions` | repository Discussions tab | | 2026-10-11 | account |
| `desktops` | Desktops | | 2026-10-11 | account |
| `shortcuts` | Shortcuts | | 2026-10-11 | account |
| `hosts` | Hosts & fleet | | 2026-10-11 | account |
| `network` | Network & nodes | | 2026-10-11 | account |
| `coves` | Coves | | 2026-10-11 | account |
| `on-device-ai` | On-device AI (`localAI`) | | 2026-10-11 | account |
| `activity` | Activity | | 2026-10-11 | account |
| `stats` | Stats | | 2026-10-11 | account |
| `organizations` | Organizations (`orgs`) | | 2026-10-11 | account |
| `world` | World | | 2026-10-11 | account |
| `notes` | Notes | | 2026-10-11 | account |
| `tasks` | Tasks | | 2026-10-11 | account |
| `calendar` | Calendar | | 2026-10-11 | account |
| `ledger` | Ledger | | 2026-10-11 | account |
| `tour` | welcome and feature tour | | 2026-10-11 | account |
| `design-system` | Design (`designSystem`), `/design` | | 2026-10-11 | account |
| `status-page` | status dots, Status (`status`) | | 2026-10-11 | account |
| `speech` | Speech Playground | | 2026-10-11 | account |
| `glasses-alert` | Glasses Alert (`glassesAlert`) | | 2026-10-11 | account |
| `timers` | Timers | | none | account |
| `vault` | Vault | | none | account |
| `calories` | Calories | | none | account |
| `steps` | Steps | | none | account |
| `water` | Water | | none | account |
| `sleep` | Sleep | | none | account |
| `sugar-break` | Sugar Break (`sugarBreak`) | | none | account |
| `stretch` | Stretch | | none | account |
| `food-guide` | Food Guide (`foodGuide`), iPhone only | | none | account |
| `workflows` | Workflows; the `workflows-*` packs nest under it | | 2026-10-14 | account |
| `sign-in` | phone top bar guest Sign in chip, menu Sign in / Sign out | | 2026-10-11 | device |

The packages from `notes` down already existed and moved to account sync with
a version bump. Tour and Design lose their per-phone switches
(`packages/tour/on`, `packages/design-system/on`). Settings always keeps a way
to sign in and out, so removing `sign-in` never locks anyone out.

Unchanged: `crm` stays organization-installed (Home tile only when an
organization the user belongs to has it); `market`, `repos`, `search` and
`settings` stay core.

Nesting: every Market nests a package under its first `dependencies` entry, so
Pull requests sits under Branches, Special Agents under Agents, and the
`status-check-*` packages under Status Page. Installing a child installs its
parent; removing a parent removes its children.

## The effective-state rule

A surface that belongs to a package (Home tile, bar tab, rail button,
repository tab, sidebar link, search hit, command palette entry, assistant
route, quick action, recents row) shows only while the package is effectively
installed: it and everything it depends on are installed. Deep links and direct
routes keep working; hiding is about entry points.

The install state of one package, in order:

1. An account-synced package with an account row: the row decides. On
   desktop and phones the server's answer for a package without a row
   (`legacy` in the payload) does not count; the project's rule does.
2. No row: installed when `install.default` is true, or when `installedBefore`
   is set and the account (or project) was created before that date.
3. No row, and a package that keeps its own per-account data
   (`dataLifecycle.scope: "account"`) without `installedBefore` (vault,
   calories, steps, water, sleep, sugar-break, stretch): installed when the
   package's `account_installed` hook says the account set it up through the
   package's own install panel. The hook is never called for a package that
   has a row. Since 2026-10-13 desktop and phones count such an install only
   in a legacy project, like step 2.
4. A client that has never loaded the catalog treats a gated package as
   installed only in a pre-2026-10-11 project.

Since 2026-10-13 these steps decide alone only in a legacy project (and on
the web). A new project on desktop and phones keeps its own install list
([below](#2026-10-13-plugins-belong-to-the-project)).

"Created before the date" means:

- Server and web: the account record's `created_at` (milliseconds, inside the
  decrypted `users.data`) is earlier than midnight UTC of the date. A missing,
  zero or unreadable `created_at` counts as an old account. An account created
  on 2026-10-11 is new.
- Desktop: the project's `project.json` `created`; the home project and a
  project without `created` count as old.
- Phones: a per-project marker written the first time the new code runs, set
  to legacy when the project's preferences already show earlier use of the app.
  Since 2026-10-13 a project added on the phone (New project, or a checked
  instance) is marked new when it is added, so a sign-in afterwards cannot
  make it look used.

### Server details

`app/src/plugin_data.py` implements steps 1 to 3:

- `_own_states` resolves each package's own state from the account rows, the
  fallback (`_fallback_installed`: `default` or `_legacy`) and, for the
  hook packages (`_hook_fallback`), the package hook.
- `GET /api/plugins/@account`, its install, uninstall and settings actions,
  and `GET /api/plugins/` take `created_at` from the session record. The
  last also applies `installedBefore` to packages that are neither synced nor
  organization-scoped, so `sign-in` is listed for older accounts.
- `account_package_installed(runtime, account_bi, plugin, created_at=None)`
  reads `created_at` only when the package or one of its dependencies has no
  row and depends on `installedBefore`; it asks the runtime's
  `account_created_at(account_bi)` (`_PluginApiRuntime` in `entry.py`, and
  `_PluginState` in `workflows_runtime.py`). A runtime without that method
  counts the account as old.
- Removing a hook package from the Market deletes its account row instead of
  storing it as removed, after `purge_account_data` emptied it. The hook then
  says not installed, and setting the package up again through its own install
  panel (for example `POST /api/calories/install`) counts as installed again
  without a Market round trip. A Market install writes a row, so the package
  shows in every app even before its own data is set up; the screen then shows
  its install panel.
- Status Page has a hook too, but it asks the engine back, so only packages
  without `installedBefore` use the hook fallback.
- Organizations still cannot install account-synced packages
  (`plugin_installs_per_account`).
- `GET /api/market/packages` rows and the staged `plugin_registry.PLUGINS`
  entries carry `installedBefore` (`market_packages.installed_before`); the
  rows leave the key out when the manifest has none, the registry uses `""`.

Tests: `marketplace/tests/test_core_plugins.py`.

## 2026-10-13: new projects of old accounts, and the avatar

The user reported a regression: "add the user avatar to be back on the top
nav and make sure all plugins are turned off so they don't show up on a new
project".

- Plugins showed up in new projects because `GET /api/plugins/@account`
  answers for every account-synced package, and for an account created
  before 2026-10-11 a package without a row answers `installed: true`
  (`installedBefore`). Desktop, iPhone and Android took that answer over
  their own per-project rule. The server now adds `"legacy": true` to such an
  entry (`_legacy_only` in `plugin_data.py`: no row, not `default`, has
  `installedBefore`, installed). The apps ignore a `legacy` entry's
  `installed` and use the project's own rule: installed only in a legacy
  project. A real row (any install or removal) still follows the account to
  every project. The web has no projects and keeps the account answer.
- The phone top bar avatar opens the hub (projects, tools, Settings), so
  removing `sign-in` hid the way in. The avatar now always shows; `sign-in`
  gates only the guest's Sign in chip or banner and the menu's Sign in /
  Sign out. The top bar Glasses radar follows `glasses-alert` on both phones.

Tests: `test_only_dated_fallback_installs_are_marked_legacy`
(`marketplace/tests/test_core_plugins.py`),
`testAnOldAccountsFallbackFollowsTheProject` (iOS `PackageGateTests`, Android
`PackageGateTest`).

## 2026-10-13: where each plugin shows up

The user circled Vault, Calories, Steps, Water, Sleep, Sugar Break, Stretch
Break, Food Guide and Workflows on the iPhone Home grid and asked: "Make sure
these are plugins, and not installed by default on a new project also make it
so that plugins can be installed on mobile, desktop and web or individually to
web only mobile and desktop and not web etc… have them be customizable where
they show up."

Why they still showed up in a new project:

- Vault, Calories, Steps, Water, Sleep, Sugar Break and Stretch have no row
  once their account set them up through their own install panel; the server
  then answered `installed: true` from the package hook (step 3), without
  `legacy`, so every project of that account showed them. The server now
  marks that answer `legacy` too (`_legacy_only` covers `_hook_fallback`
  packages). Desktop and phones treat a `legacy` entry as installed only in
  a legacy project, whatever the package's `installedBefore`. A Market
  install still writes a row and follows the account everywhere. The web,
  which has no projects, keeps the account answer.
- Workflows was core (`install.default: true`, never gated on the phones).
  Workflows 1.1.0 has `default: false` and `installedBefore: "2026-10-14"`:
  accounts and projects from before the change keep it, newer ones start
  without it. Its packs (`workflows-pack-*`, `workflows-examples`,
  `workflows-repository-files`) depend on it, so they nest under it in every
  Market and go with it.

### Places

An account-synced install now has places: the apps it shows in, from
`mobile` (iPhone and Android), `desktop` and `web`.

- The manifest's `install.places` lists the places a package can show in at
  all; left out, all three. Food Guide 2.1.0 is `["mobile"]`. Market rows
  (`GET /api/market/packages`) and the generated `PLUGINS` registry carry it
  as `places`.
- `account_package_installs` gained a `places` column (comma separated, empty
  means every available place). `_ensure_account_schema` adds it to an older
  table with `ALTER TABLE ... ADD COLUMN`, ignoring "duplicate column".
- `GET /api/plugins/@account` entries carry `places` (the user's pick, or
  every available place) and `availablePlaces`. An install
  (`POST .../<id>/install`) takes an optional `{"places": [...]}`; without it
  a fresh install gets every available place and a repeated one keeps the
  stored pick. Dependencies installed along with a package get its places.
  `POST /api/plugins/@account/<id>/places` with `{"places": [...]}` changes
  them for an installed package (400 `invalid_places` for an empty list or a
  place the package does not offer, 409 `plugin_not_installed`). Settings
  saves keep the places.
- `GET /api/plugins/` (the web sidebar) lists an account-synced package only
  when `web` is one of its places.
- Each app shows a package's entry points only when it is effectively
  installed and the app's own place is in its places. A dependency only has
  to be installed, not placed in the same app. An answer without `places`
  (an older server) counts as every place.
- Every Market has a "Show on" row of Phone / Desktop / Web switches for an
  installed account-synced package, limited to its available places. The
  last one cannot be turned off; Remove takes the package off everywhere.

Tests: `marketplace/tests/test_core_plugins.py`
(`test_an_install_shows_where_the_user_picked`,
`test_a_dependency_installed_with_a_child_follows_its_places`,
`test_places_outside_what_the_package_offers_are_refused`,
`test_an_older_install_table_gains_the_places_column`,
`test_data_packages_set_up_without_a_row_are_marked_legacy`,
`test_workflows_stay_for_accounts_from_before_the_change_and_bring_their_packs`).

## 2026-10-13: plugins belong to the project

The user created a new project on the iPhone and Home still showed Agents,
Inbox, Shortcuts, Vault, Calories, Steps, Water, Sleep, Sugar Break, Stretch
Break, Food Guide, Glasses Alert and Workflows, and said: "Plugins should be
based on project - for example I created a new project but these shows up
they should have been hidden and available to install as plugins".

Why they showed:

- A Market install writes an account row, and an account row decided the
  install state in every project (step 1 above). Anything the account had
  installed anywhere, or had a row for, came along into each new project.
- On the phone, the legacy marker of an added project was written on the
  first read after switching to it. When that read came after the sign-in,
  the stored session counted as earlier use and the new project was marked
  legacy, so every `installedBefore` and `legacy` package showed as well.

What changed:

- A legacy project (desktop: created before 2026-10-11 or the home project;
  phone: legacy marker) still follows the account, as before.
- A new project keeps its own install list. Desktop: its own settings file
  already held the install state, and the account answer may now only turn a
  package off there (the account removed it everywhere), never on. Phones: a
  per-project map of package id to installed / removed
  (`packages/project-installs` on iOS, `packages/projectInstalls` on
  Android, in the project's own namespace).
- In a new project a package counts as installed when the project installed
  it and the account still has it (a `legacy` answer counts), or when it is
  `install.default` and neither the project nor the account removed it.
  Nothing else shows, whatever the account installed in other projects.
- Installing in a new project still installs it on the account (so the web,
  its settings and its places work) and records it, with the dependencies it
  brought, for this project. Removing in a new project records it, with its
  installed dependents, as removed for this project only; the account keeps
  it and other projects are unaffected. The data packages' own "remove and
  erase" panels (Steps, Water) still remove from the account as well.
- Market wording in a new project: "Installed in this project", "Install in
  this project", "Remove from this project", and "Your other projects install
  it on their own."
- The phone marks a project new the moment it is added (`RelayDirectory.add`
  and `addNamed` on iOS; the Android project add flow). The iOS Next-project
  retirement ignores that marker when it asks whether a project was used.

The web has no projects and keeps the account answer.

Tests: iOS `PackageGateTests` (`testANewProjectShowsOnlyWhatItInstalled`,
`testInstallingAndRemovingInANewProjectStaysInTheProject`,
`testEachProjectKeepsItsOwnChoices`, and the older new-project cases now
expect account installs from elsewhere to stay hidden), `SettingsServiceTests`
(`testAProjectAddedOnThisPhoneStaysNewAfterSignIn`); desktop
`test_package_registry.cpp` ("an account install does not turn a package on
in a new project" and the checks around it); Android `PackageGateTest` and
`AccountPackagesTest` (listed in
[NAVIGATION.md](../native/NAVIGATION.md#2090-plugins-belong-to-the-project-2026-10-13-ios-and-android)).

## Hosts

- Desktop: [docs/desktop/README.md](../desktop/README.md).
- iPhone: [docs/native/NAVIGATION.md](../native/NAVIGATION.md).
- Android: [docs/native/ANDROID.md](../native/ANDROID.md).
- Web: [docs/www/README.md](../www/README.md).
- Manifest field: [marketplace/README.md](../../marketplace/README.md#dependencies-account-installs-and-settings).
- Status Page specifics: [status-page.md](status-page.md).
