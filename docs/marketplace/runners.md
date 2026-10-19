# Runners plugin

The `runners` Market package (`marketplace/packages/runners/`) lets an
account manage the runners it registered from the desktop app and from both
phones: see their status and provider sign-in, sign a provider in, switch the
active provider account, pause, resume and revoke. The web keeps the core
`/dashboard/runners` page (adding and setting runners up), so the package has
no `dashboard.page` contribution and its places are `mobile` and `desktop`.
It is account-synced and off by default, like the other gated packages
([core-plugins.md](core-plugins.md)).

Why a package: before it, runners showed only as a block on the desktop's
Hosts page and as sign-in banners on the phones' Agents screen. There was no
runner list on a phone, and no client could pause or revoke a runner.

## API used

Every call is account-session authenticated (`Authorization: Bearer`) and
answers only for runners the caller registered; anyone else gets `404`
([../RUNNERS.md](../RUNNERS.md#private-runners)).

| Call | Purpose |
|---|---|
| `GET /api/runners` | `{runners: [...], providers, now}`; each runner has `name`, `scope`, `status` (`active`, `paused` or `revoked`), `lastSeenAt` (ms), `activeLeases`, `availability` (per provider: `auth`, `accounts`, `binaryFound`), `logins`, `accountRequests` |
| `GET /api/runners/attention` | runners whose provider sign-in is expired or expiring |
| `POST /api/runners/pause` `{name}` | `active` → `paused`; `{ok, name, status}` |
| `POST /api/runners/resume` `{name}` | `paused` → `active` |
| `POST /api/runners/revoke` `{name}` | → `revoked`, cannot be undone |
| `POST /api/runners/<name>/accounts` `{provider, account, action}` | activate or remove a stored provider account |
| `POST /api/runners/<name>/login` and `/login/<id>/{output,input,cancel}` | provider sign-in terminal |

Pause and resume answer `409 runner_inactive` for a revoked runner, and are
idempotent otherwise.

## Pause

A paused runner keeps its key and stays signed in. It still reports and
finishes the jobs it holds, still polls, and its sign-ins and account
switches still work, but its claims lease nothing (agent jobs and Actions
jobs), and the app never queues new work for it (`_org_agent_pool_target`
and `_org_agent_personal_targets` read only `active` rows). A follow-up to a
session that ran on it hands over to another runner, as for a runner that is
gone (`_runner_gone`). Re-registering a runner sets it back to `active`.

Code: `runner_leases._set_paused` (`PAUSE_ACTIONS`), `_signed_caller`
accepts `active` and `paused` rows and marks the caller `paused`, `_claim`
drops providers and Actions slots when paused; `runner_logins.USABLE_STATUSES`
lets sign-in and account requests through for a paused runner and
`_attention` still lists it. Tests:
`app/tests/test_runner_leases.py::test_a_paused_runner_claims_nothing_until_resumed`.

Why a status value and no new column: a paused runner must still pass
`_signed_caller` for its reports and sign-ins, and `status` is already the
one field every query about a runner's eligibility reads, so `paused` falls
out of every `status='active'` filter with no schema change.

## Clients

- **Desktop:** `builtin:runners`, `marketplace/packages/runners/content/desktop/`,
  API prefix `/api/runners`, rail icon `server`. Needs the account session;
  when `/api/runners` answers `unauthorized` the page shows a signed-out state
  instead of an error. `RunnersModel` turns the payload into display rows
  (reusing `parseRunners`, `parseRunnerAttention`, the provider state text and
  the account and connect URL builders from `desktop/src/RunnerLogin.h`) and
  decides which actions a status offers; `RunnersFeature` draws one
  `settingsRow` per runner in a `card`, with the status dot, scope and status
  `badge`s, the detail line (online, last seen, active jobs) and a line per
  provider with its state badge, active account, Sign in and an Accounts menu.
  Pause and Resume post at once and reload; Revoke and removing an account ask
  first in a `QMessageBox`. Sign in opens the web connect page
  (`/dashboard/runners?runner=…&provider=…&connect=1`) in the browser, because
  the relayed sign-in terminal is built from `MainWindow` internals a package
  cannot reach; Add runner opens `/dashboard/runners`. Both go through the
  host's `openWebPage(path)`, added for this package: it resolves the path on
  the active host with `nativePackageWebPageUrl`, which drops credentials and
  refuses another origin or a `..` segment. The rail badge counts
  `/api/runners/attention` items, read on startup (`prepareNavigationBadge`)
  and on every refresh; there are no timers. Tests:
  `content/desktop/tests/test_runners.cpp` (target `forkmesh-runners-tests`)
  and the `openWebPage` URL checks in
  `desktop/tests/test_native_package_extension.cpp`.
- **iPhone:** route `.runners` (kind `runners`, Operate group after Hosts,
  `/dashboard/runners`, `forkmesh://runners` and
  `forkmesh://dashboard/runners` parse to it), screen
  `native/ios/ForkMesh/Features/Packages/RunnersScreen.swift`, registered in
  `Navigation/RouteFallbacks.swift` and gated by `PackageGate` (hub item and
  route kind `runners`, `MarketPackage.nativeDestinations["runners"]`,
  `PackageRoutes`, `PluginMenu.homeKinds` for the ⋯ button). Core in
  `ForkMeshCore/Sources/FMApi/Runners/`: `RunnerList.swift` parses
  `GET /api/runners` into `RunnerListEntry` (status `RunnerStatus`,
  `activeLeases`, per-provider `RunnerProviderState` with
  `RunnerProviderSignIn` and the active account; revoked rows last) and names
  the `RunnerAction` paths; `RunnersModel` loads, pauses, resumes and revokes
  and holds the signed-out, error and message state. Sign in reuses
  `RunnerSignInSheet`; Accounts opens `RunnerAccountsSheet` narrowed to the
  runner (`only:`), and `RunnerAccounts.listed` now keeps paused runners,
  since their sign-ins and account switches still work. Signed out (or `401`
  / `403`), it asks to sign in. Add runner (toolbar `+`, and the empty state)
  opens `/dashboard/runners` through `ApiService.webLink`. Tests:
  `FMApiTests/RunnerListTests.swift`, plus runners cases in
  `RouteParserTests`, `NavigationCatalogTests`, `PackageRoutesTests` and
  `PluginMenuTests`.
- **Android:** route kind `Runners` (`Route.Runners`, title "Runners", icon
  `dns`, hub item in the Operate group after Hosts, needs registration), parsed
  from `/dashboard/runners`, `forkmesh://runners` and
  `forkmesh://dashboard/runners`. Gated by `PackageGate.HUB_ITEMS["runners"]`
  and `ROUTE_KINDS[Runners] = "runners"`; Market's `NATIVE_DESTINATIONS` maps
  `runners` to the route, and the plugin ⋯ menu comes from `DetailScaffold`
  via `PackageMenu.packageFor`. Core:
  `native/android/core/.../runners/RunnerList.kt` (`RunnerList.parse` →
  `RunnerRow`/`RunnerProviderState`/`RunnerAuth`, `RunnerStatus`,
  `RunnerList.actions(status)`, `actionPath`/`actionBody`, `load` and `change`
  returning `RunnerListResult`/`RunnerChange`) and
  `RunnerAttentionApi.changeStatus` for the pause, resume and revoke POSTs.
  `RunnerAccounts.sections` now lists paused runners too, since their account
  switches still work. App: `native/android/app/.../feature/runners/`
  (`RunnersFeature.install()` from `Features.kt`, `RunnersScreen`): pull to
  refresh and a Refresh action, signed-out / empty / error states, a per-provider
  Sign in that opens `RunnerSignInScreen` through
  `RunnerAttentionFeature.open`, Accounts opening `RunnerAccountsScreen`,
  Pause/Resume, Revoke behind an `FmConfirmDialog` (destructive), and Add runner
  opening `/dashboard/runners` on the account origin with
  `NavigationFeature.openInBrowser`. Tests: `RunnerListTest`, plus
  `NavigationTest` and `PackageGateTest.testRunnersIsAGatedPackage`.

Each client lists the runners with a status dot (online when seen in the last
10 minutes), a Paused or Revoked label, per-provider sign-in state and the
active account, and offers Sign in, Accounts, Pause or Resume, and Revoke
(with a confirmation). Revoked runners are listed last and offer no actions.
Adding a runner opens `/dashboard/runners` on the active host.
