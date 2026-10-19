# Core features as plugins, off by default in new projects

Most of what the Home grid, the bar tabs and the repository tabs offer is a
Market package. Each package is the feature's on/off switch, Market listing
and install state; the feature code stays in each app (desktop, iPhone,
Android, web). New projects and accounts start with these packages removed,
except the phone starter pack below; older ones keep what they had. Repos, Search, Market and Settings stay core
and always show.

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
  `design-system`, `status-page`, `logs`, `diagnostics`, `speech`, `glasses-alert`.
- **Installed before 2026-10-14, account-synced:** `workflows` (its
  `workflows-*` packs nest under it).
- **Installed before 2026-10-19, account-synced:** `files` (the file
  explorer, [files.md](files.md)).
- **No `installedBefore`, account-synced:** `timers`, `vault`, `calories`,
  `steps`, `water`, `sleep`, `sugar-break`, `stretch`, `food-guide` (mobile
  only), `phone-security` (mobile only, [phone-security.md](phone-security.md)),
  `eye-control` (mobile only, [eye-control.md](eye-control.md)),
  `strata`, `ember`, `synapse` (mobile and desktop since 1.1.0; desktop
  installs from before are carried to the account,
  [codebase-analysis.md](codebase-analysis.md#on-phones)).
- **Per device:** `sign-in` (installed before 2026-10-11); `code-map` and
  `size-map`, the repository screen's Code map button and Size map tab
  (installed before 2026-10-17, desktop only,
  [codebase-analysis.md](codebase-analysis.md)).

Each package gates its feature's surfaces in every app (tabs, tiles, rail
buttons, repository tabs). `crm` stays organization-installed; `market`,
`repos`, `search` and `settings` stay core.

Settings always keeps a way to sign in and out, so removing `sign-in` never
locks anyone out. The phone top bar avatar always shows; `sign-in` gates only
the guest's Sign in chip and the menu's Sign in / Sign out. On phones
`sign-in` is on until the user removes it, in new projects too.

## Phone starter packs

The first time a new phone project opens (after the welcome and sign-in
pages, before the shell), it asks once "Choose how to start" and offers three
packs, Standard preselected, with a single Continue:

| Pack | id | Installs |
|---|---|---|
| Minimal | `minimal` | `code` only (no agents, chat, inbox or tour) |
| Standard (default) | `standard` | `agents`, `chat`, `inbox`, `code`, `tour` |
| Everything | `everything` | every gated package except the organization packages (what a legacy project gets) |

`sign-in` stays a per-device switch, on by default whatever the pack. Web and
desktop keep the off-by-default rule.

- Where: iOS `StarterPack` beside `PackageGate.starterPackages` in
  `FMApi/Navigation/PackageGate.swift`, screen `StarterPackView`, shown by
  `AppHomeView` while `SettingsService.needsStarterPack`; Android
  `StarterPack` beside `PackageGate.STARTER` in `C/market/PackageGate.kt`,
  screen `StarterPackScreen`, shown by `Features.Root` while
  `PackageGateFeature.starterPackPending`.
- The choice is stored per project next to the project installs
  (`<namespace>packages/starter-pack` on iOS, `packages/starterPack` in the
  project prefs on Android). Until it is stored the project behaves as
  `standard`, so projects created before the packs existed are unchanged.
- Legacy projects are never asked and keep what they had.
- A pack package counts as installed in the new project until the project
  removes it; the account answer does not hide it, because a new account has
  no row for it. Project choices made later in the Market override the pack,
  in both directions.
- Before the catalog loads, a gated package with no entry is installed when
  the project is legacy or the package is in the chosen pack.

Why: an October 2026 audit found new installs opened on Repos, Search, Market
and Settings only, with nothing to do, so Kaif's fixed starter set (Standard)
turned the three bar tabs, the Code tab and the tour on. A single fixed set
forced agents on people who want a quiet start, so the owner asked for a
choice: Minimal for code only, Everything for people who want the legacy
layout, Standard for everyone else.

Nesting: every Market nests a package under its first `dependencies` entry, so
Pull requests sits under Branches, Special Agents under Agents, and the
`status-check-*` packages under Status Page. `logs-app`, `logs-desktop` and
`logs-hosts` sit under Logs and independently control the mobile stream types.
Installing a child installs its
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

On a self-hosted instance (`INSTANCE_NAME` set, `instance_features.branded`)
step 2 counts every account as created before every date
(`_instance_created_at` in `plugin_data.py`), so an account there starts with
the whole pre-marketplace set: the repository tabs (Code, Issues, Pull
requests, Branches, Commits, Releases, Discussions, Actions, Agents) and the
navigation entries (Chat, Network, Agents and the rest). Why: the cut-off
only exists so that new forkmesh.com accounts pick features in the Market
onboarding; an instance has no onboarding, every account on a new instance
(its admins included) is created after the date, and the web hides every
`data-plugin-entry` the account does not have, so signed-in users saw fewer
repository tabs than signed-out visitors. Removing a package still stores an
`installed: false` row, which wins as in step 1. ForkMesh-company surfaces
among them (World, the ledger) stay hidden by the instance's own plugin
switch (`INSTANCE_PLUGINS`, see [INSTANCES.md](../INSTANCES.md)).
forkmesh.com is unchanged.

The server marks answers from steps 2 and 3 with `"legacy": true`
(`_legacy_only` in `plugin_data.py`). "Created before the date" means the
account record's `created_at` (missing or unreadable counts as old) on the
server and web, the project's `project.json` `created` on desktop (the home
project counts as old), and a per-project legacy marker on phones (a project
added on the phone is marked new when it is added).

Answers with no row that step 1 to 3 do not cover (not default, no
`installedBefore`, no per-account data hook) carry `"unchosen": true`
(`_unchosen`). The account has never chosen the package, so a desktop that
has its own stored install keeps it and posts an install to the account
(`PackageRegistry::hasStoredInstall`, `MainWindow::applyAccountPackages`).
This keeps desktop installs made while a package was desktop-only, such as
`voice-changer` before 1.1.0 added its web page and account sync. Removing
a package stores a row with `installed: false`, so a removal is never undone
this way.

Account-scoped data alone does not establish a legacy install choice:
`_hook_fallback` checks that the package actually provides an
`account_installed` hook. Packages such as Instance Feed use only the shared
account registry and must remain `unchosen` until an account row exists,
including when a desktop install initially failed because the server's
catalog was older.

### Projects

The web has no projects and uses the account answer. Desktop and phones:

- A **legacy project** follows the account answer, `legacy` entries
  included.
- A **new project** keeps its own install list (desktop: the project's
  settings file; phones: `packages/project-installs` on iOS,
  `packages/projectInstalls` on Android). A package counts as installed when
  the project installed it and the account still has it, or when it is
  `install.default` and neither the project nor the account removed it, or
  (phones) when it is in the project's starter pack and the project did not
  remove it.
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

## Desktop offline mark

Some packages only work while the user's desktop is reachable: `agents`,
`desktops`, `special-agents`, `shortcuts`, `hosts` and `usage`. While the
desktop is offline, each of their icons gets a small amber dot at the
bottom-left corner that slowly fades in and out (one cycle takes about two seconds).
It tells the user why the page is empty before they open it. The bottom-left
corner was chosen because it is the only one free: counts sit top-right,
live markers top-left, and the update dot (phone) and activity lamp
(desktop) bottom-right. With Reduce Motion on, the dot stays solid.

The list is kept by hand in three places, because no manifest field says
whether a package needs the desktop:

- iOS: `PackageGate.desktopReliantPackages`.
- Android: `PackageGate.DESKTOP_RELIANT`.
- Desktop: `PackageRegistry::reliesOnDesktop`.

Which icons get the dot, and what counts as offline:

- **iPhone.** Home grid tiles (`ShellHomeIcon`) and bottom bar cells
  (`ShellBarCellLabel`), through `ShellDesktopOfflineDot`. Offline means
  `DesktopControlHub.reach` is `.offline`. `.connecting` and `.notPaired`
  show no dot, so the dot does not flash each time the app opens, and a
  user with no desktop is not warned about one.
- **Android.** Menu grid tiles (`ShellMenuTile`), plus the Agents tab and
  rail item (`BadgeIcon`). It uses the same rule, `DesktopReach.Offline` on
  `AgentsFeature.hub`, read through `rememberDesktopOffline`.
- **Desktop.** Rail buttons (`ActivityRailButton::setDesktopOffline`, drawn
  by both the classic and the Next painter) and Home board tiles
  (`HomeTile::desktopOffline`). Here, offline means this desktop is not
  online on the relay (`updateConnectionStatus`, where the top-right
  connection dot is not green), so phones and the web cannot reach it.
  `syncDesktopOfflineDots` marks the Agents and Team buttons and any native
  package page in the list.

## Hosts

- Desktop: [docs/desktop/README.md](../desktop/README.md).
- iPhone: [docs/native/NAVIGATION.md](../native/NAVIGATION.md).
- Android: [docs/native/ANDROID.md](../native/ANDROID.md).
- Web: [docs/www/README.md](../www/README.md).
- Manifest field: [marketplace/README.md](../../marketplace/README.md#dependencies-account-installs-and-settings).
- Status Page specifics: [status-page.md](status-page.md).
