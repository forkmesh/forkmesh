# ForkMesh Marketplace packages

> Developer notes for this directory (sockets, generators, host runtime,
> traps) are in [docs/marketplace/README.md](../docs/marketplace/README.md).

This directory is the source catalog for creations that can move through the
ForkMesh ecosystem. Software extensions, virtual goods, agents, themes, and
other future creations use the same package envelope instead of inventing a
new installer for every category.

## Package layout

A distributable `.fmpkg` is a ZIP-compatible archive with this shape:

```text
manifest.json
content/
preview/
migrations/                 # optional, but every up file has a down file
LICENSE
SIGNATURE
```

`manifest.json` follows [`manifest.schema.json`](manifest.schema.json). The
manifest identifies the creator, package type, version, price/licence model,
entry points, permissions, screenshots, dependencies, and provenance. Every
listing supplies at least one image under `preview/`, with concise alternative
text for accessibility. `SIGNATURE` signs the canonical manifest and the digest
of every archived file; marketplace mirrors may carry the bytes, but they cannot
silently change the creator or content.

The source tree keeps one creation under `packages/<id>/`. Native bundled
extensions are compiled with the desktop client, while their install state and
UI contributions still go through the same lifecycle as downloaded packages.
That makes built-ins removable without leaving actions, active work, settings,
or package-owned data behind.

Checked-in bundled sources inherit the signed release bundle's integrity and
do not contain a freestanding `SIGNATURE` file. Exporting one as a downloadable
`.fmpkg` must add a creator signature; a marketplace must reject an external
archive that omits it or whose file-digest set does not verify.

## Catalogs and sources

Catalogs use `schemaVersion: 2`. Every entry carries its package id, version,
safe manifest path, and (for remotely distributed packages) an archive SHA-256,
byte size, and ordered set of locations. Catalog locations use one of three
transports:

- `local` for a checked-out or installed catalog;
- `git` for any Git URL and optional ref/catalog path; or
- `https` for a directly hosted catalog or `.fmpkg` archive.

The transport is the contract. `provider` is presentation metadata only, so the
same code consumes ForkMesh repositories and mirrors, GitHub, GitLab, and other
Git or HTTPS hosts. `marketplace/sources.example.json` shows an ordered failover
registry. Sync and installation use `marketplace/tools/marketctl.py`; external
artifacts are size-bounded, digest-checked, path-traversal checked, and staged
before activation.

Package metadata and assets stay inside the package. The desktop resource and
source lists are generated from package-owned `content/desktop/build.json`
descriptors, while the World module list is generated from declared `world`
entrypoints. Adding a package never requires editing a core resource allowlist.
Native descriptors may also declare `mocHeaders`, plus focused `testSources`
and allowlisted Qt `testLibraries`; the desktop build creates those MOC units
and package test targets without naming the package in core CMake.

Application features use the same package-owned boundary. A manifest may add
`contributions`, each naming a host-neutral `socket`, one of the manifest's
package-owned `entrypoints`, and socket-specific `config`. Hosts discover the
sockets they implement and ignore unfamiliar ones, so a new package category
does not require a new central registry or a package-id branch in core code.

The App currently provides `dashboard.page` and `worker.http` sockets. A
dashboard contribution supplies an HTML entrypoint plus declarative route,
title, navigation, and optional script-entrypoint metadata. Its script calls
`window.ForkMeshDashboard.registerPage(pageId, initializer)` and receives the
bounded dashboard plugin API. A Worker contribution supplies a Python
entrypoint and a route under `/api/plugins/<package-id>`. Package-specific
pages, scripts, modules, and route tables are written only to ignored
`app/dist` and `app/src_build` staging directories during deployment; they are
never generated into tracked application sources.
Only packages explicitly marked `install.bundled` are activated by this
release-time App host. Catalog publication by itself does not grant package
code a dashboard or Worker execution capability; `install.default` remains a
client install-state choice rather than permission to ship server code.

## Lifecycle contract

An installer must complete these phases as one logical operation:

1. Validate the manifest, type, version, signature, permissions, and content
   paths before activating anything.
2. Record the licence separately from installation state so uninstalling does
   not erase a purchase or creator attribution.
3. Activate declared contributions only after installation succeeds.
4. On removal, stop active work, detach every UI contribution, revoke runtime
   capability handles, and delete package-owned settings/cache. User-created
   output and purchase receipts remain in the user's Library.

Database migrations are declared under `lifecycle.migrations`. Each migration
must provide a unique id and distinct `up` and `down` SQL paths under
`migrations/`. Package SQL cannot control transactions: the host applies ups in
order and downs in reverse order in host-owned transactions. Uninstall first
quarantines the package, runs all downs, updates discovery atomically, and
restores the package and schema if removal fails.

`marketctl install` and `marketctl uninstall` default to
`<install-root>/plugin-state.sqlite`; both commands therefore use the same
migration ledger without extra flags. The desktop treats the verified on-disk
catalog as authoritative and will not fake-remove an external package by merely
hiding its UI—external removal goes back through `marketctl` so downs cannot be
skipped.

ForkMesh instances use the identical engine with
`--database-scope organization`; device and organization migrations have
separate ledgers, and both scopes require the same paired-down contract.

### Dependencies, account installs and settings

- `dependencies: [{"id", "version"}]` names packages this one needs. The
  catalog refuses an unknown id, a cycle, or a dependency whose `install.sync`
  differs. Installing a package installs what it depends on; removing a
  package removes every installed package that depends on it.
- `install.sync: "account"` keeps the install state, and the values of the
  declared settings, on the user's ForkMesh account instead of each device.
  Desktop, iPhone, Android and the web read and change it through
  `/api/plugins/@account`, so removing the package on one device removes it
  from all of them, and installing it brings it back everywhere.
- `install.installedBefore: "YYYY-MM-DD"` (only with `install.default:
  false`) installs the package for projects and accounts created before that
  date, so a feature can turn off by default for new projects without
  disappearing from existing ones. The server compares it with the account's
  `created_at` (missing counts as old), the desktop with the project's
  `project.json` `created`. `GET /api/market/packages` rows carry it
  ([docs/marketplace/core-plugins.md](../docs/marketplace/core-plugins.md)).
- `install.places: ["mobile", "desktop", "web"]` lists the apps an
  account-synced package can show up in (`mobile` is iPhone and Android).
  Left out, it is all three. Each install keeps the user's own pick from
  that list, so a package can be installed everywhere, on the web only, or
  on the phone and desktop but not the web
  ([docs/marketplace/core-plugins.md](../docs/marketplace/core-plugins.md#2026-10-13-where-each-plugin-shows-up)).
- `settings: [{"key", "type", "label", "default", "options"}]` declares
  settings every host renders by itself: `boolean` (a toggle) and `choice`
  (a picker over `options[].value` strings). The server rejects keys and
  values the manifest does not declare.

Status Page and its `status-check-*` checks are the first packages to use all
three ([docs/marketplace/status-page.md](../docs/marketplace/status-page.md)).

The plugin decomposition audit from issue #677 is published as the planned
plugins documentation: the broad
[opportunity inventory](../www/docs/plugins/opportunities.md) maps the
repository's desktop, App, World, mobile, mirror, tooling, web, skill, and
content boundaries to candidate packages and the host sockets needed to extract
them, and the stingy [shortlist](../www/docs/plugins/shortlist.md) applies a
much higher admission bar and names the boundaries that should remain in the
trusted host.
The narrower per-candidate review for issue #653 — the host API, permissions,
package-owned state, teardown, migration compatibility, tests, and a
keep/extract decision for each row — is
[`docs/marketplace-extraction-inventory.md`](../docs/marketplace-extraction-inventory.md).

The public landing page for the catalog is
[forkmesh.com/market](https://forkmesh.com/market). Its package list is
generated from `catalog.json`, so re-run
`python3 app/tools/build_market_catalog.py` after adding or changing a package —
`app/tests/test_market_landing.py` fails while the page and the catalog
disagree.

The first package is [`capture`](packages/capture/manifest.json), the existing
desktop region-capture and annotation tool moved out of the application core.
[`discord`](packages/discord/manifest.json) followed the same route: the
desktop's Discord bot connector, compiled with the client but attached only
while installed, and removed without leaving its token, cached history, avatars,
or sidebar group behind.
It is bundled and enabled for existing users, but can be removed and installed
again immediately from the Market list without restarting ForkMesh.

[`ledger`](packages/ledger/manifest.json) contributes a removable desktop-rail
shortcut to the public financial system of record on the active ForkMesh host.
It opens the Ledger in the user's browser under an explicit external-link
permission and never receives wallet secrets or performs transfers itself. Its
in-app page also keeps the reader's own expense and income records, stored in
this app's settings on one machine and never published.

[`vault`](packages/vault/manifest.json) is an end-to-end encrypted password
vault, account-scoped like `calories`. All cryptography runs in the browser:
the master password is stretched with Argon2id and never leaves it, items are
AES-256-GCM under per-item keys, and a share seals an item key to a named
ForkMesh user, or to each member of an organization, with X25519 and
post-quantum ML-KEM-768 joined by the X-Wing combiner and signed with Ed25519.
The Worker module files opaque blobs, holds no key, and has no plaintext
column; it decides only who is handed which ciphertext, so it enforces
permissions, organization membership, and revocation. The primitives
WebCrypto lacks are written out in the package and checked against OpenSSL 3.5
vectors on every test run. It is a full client in four places: the dashboard
page, the browser extension, a native desktop rail page (suite `fmv1` on
OpenSSL 3.5), and the mobile app (on its `cryptography` and `pqcrypto`
packages), which can also fill logins into other apps on Android behind the
device's biometric or passcode prompt. Committed interop fixtures make every
implementation open what the others write. Its README states what the server
can see and the limits of the implementation.

[`browser-extension`](packages/browser-extension/manifest.json) is the
ForkMesh extension for Chrome as its own package. It shipped inside `crm`
until the vault needed it too: one extension and one sign-in now serve both,
and an extension release no longer needs a CRM release. It has no Worker
module; each feature calls the API of the package it belongs to. The deploy
stage zips `content/extension` for its `/dashboard/extension` page, and other
packages may pin only the oldest extension build they work with.

[`analytics`](packages/analytics/manifest.json) is first-party product
analytics, and what ForkMesh's own pages use in place of a third-party
provider. An organization creates projects, each with a public write key, and
drops the package's dependency-free SDK on its sites: pageviews, autocaptured
clicks, custom events, identified people and groups, session replay with
every input masked, heatmaps, feature flags with experiments, and surveys. It
is the first package with a public, session-less route: the host still
dispatches it like any package API, and the module itself authenticates the
write key, enforces the project's allowed origins through the host's
`request_origin` capability, and meters a daily quota. Events are stored as
sealed batches, one row per browser flush and merged once the day is over, so
an insight reads hundreds of D1 rows (rqlite rows on the core nodes) rather than tens of thousands; trends,
funnels, retention, paths, stickiness, lifecycle, and experiment significance
are computed in the Worker over the opened batches. No IP address is stored.
Compaction and retention ride the generic `deliver_reminders` package hook.
`tools/sync_assets.py` assembles the single dashboard script and keeps the
site's copy of the SDK identical to the package's.

[`calories`](packages/calories/manifest.json) is the first health package
and the first with `dataLifecycle.scope: "account"`: no organization installs
it. Each signed-in account installs it for itself through the package API
(`POST /api/calories/install`), keeps a private food diary, weigh-in history,
goals, and reminders, and can invite friends by username to challenges (weight
loss by percent or kilograms, days within the calorie goal, days logged) whose
leaderboard shows derived progress only. The shared organization lifecycle
refuses organization installs for account-scoped packages, and account
deletion calls the package's `purge_account_data` hook. It is also the first
package to key its rows per account rather than per instance: installing mints
a 256-bit diary key, kept only wrapped under the host's `DATA_KEY`, and every
row the account owns — the numbers included — is written through the host's
`seal_with`/`open_with` capability with that key, so a database copy opens
nothing and one leaked key opens exactly one diary. Meals are logged by
hand, by searching Open Food Facts, by scanning a barcode with the browser
camera, or by photographing the plate: the package sends the photo once
through the host's bounded `ai_json` Workers AI capability (vision allowlist,
per-account quota) and never stores it. Barcode lookups reach only the
manifest's outbound origins, Open Food Facts and, once the account saves its
own free key, USDA FoodData Central. Reminders ride the host's per-minute
maintenance phase through the generic `deliver_reminders` package hook and
arrive as pings. The same account API backs the dashboard page, a native
desktop rail page (`content/desktop`, registered through the neutral
`NativePackageRegistration` socket), and a mobile screen reached from the
Tools sheet, so one diary follows the user across all three. It is bundled
with `install.default: false`.

[`sentry`](packages/sentry/manifest.json) connects a Sentry organization to
the desktop's Log page. It reads the organization's unresolved issues once a
minute using the API key you provide and never writes to Sentry: a new issue,
or one whose event count grew, becomes a `Sentry:` line in the log and lights
the log's SENTRY activity lane, while the page's Sentry tab aggregates the same
read by issue, project, and level. The API key, connection details, and cached
snapshot are package-owned data; removing the package stops the reads and
clears all three.

[`vscodium`](packages/vscodium/manifest.json) adds a **Codium** button to the
desktop's branch diff header that opens the reviewed branch's worktree (or the
repository checkout) in VSCodium. The button used to be built into the diff
view; it moved out of the core so only people who use VSCodium carry it, and
ForkMesh itself does not. The package fills the neutral
`DesktopPackageExtension::branchActions()` socket: each action is a label,
octicon, tool tip and trigger, the host draws it beside the branch actions,
disables it while the branch has no local checkout, and passes the branch and
its checkout folder to the trigger. It is bundled with
`install.default: false`; install it from the Market to bring the button back.

[`winamp`](packages/winamp/manifest.json) is the first installable desktop
theme. Its declarative palette recolours the complete base theme, then its QSS
entry point adds the compact graphite chrome, square metal controls, and green
display styling. Installing it applies the skin immediately; removing it while
active returns the app to the system theme.

[`gemma-4`](packages/gemma-4/manifest.json) is an opt-in local agent package.
Its declarative [`agent.json`](agent.schema.json) descriptor contributes model
variants, endpoint, runtime, documentation, and avatar to the desktop. The host
allowlists Ollama operations,
downloads the selected model before activation, and keeps the model out of the
picker again after removal. Package cards all expose a settings page; packages
without configurable runtime fields still show their lifecycle, permissions,
version, and local-data policy there. If Ollama rejects a pull because the
model requires a newer runtime, the settings page explains that the runtime
must be updated and changes its Ollama link to **Update Ollama** instead of
showing only the registry URL from Ollama's diagnostic.

The **Custom models** section also contains opt-in agent packages for
[`nemotron-3-ultra`](packages/nemotron-3-ultra/manifest.json),
[`nemotron-3.5-lightning`](packages/nemotron-3.5-lightning/manifest.json), and
[`muse-spark-1.3-contributor`](packages/muse-spark-1.3-contributor/manifest.json).
Each remains outside the built-in model picker until installed. Nemotron 3
Ultra uses Ollama Cloud, Nemotron 3.5 Lightning is locally downloadable, and
Muse Spark Contributor uses the user's Ollama-compatible local model gateway.

[`timers`](packages/timers/manifest.json) is the first package to contribute to
the window's top bar. Once installed it puts a little clock there; clicking it
starts a timer that counts up, a countdown ("25m standup", "coffee in 4m"), or
an alarm that rings at a time of day ("7:30 standup", "demo at 5pm"). The
package owns that model — what a request means, what a chip reads, and when a
countdown or alarm has run out — while the desktop supplies the clock, its
chips and their menus. Deadlines are absolute, so a countdown keeps running
while the app is closed. The entries live in the package's own settings, so
removing it takes the clock off the top bar and the entries out of the profile.
It is bundled with `install.default: false`, so the clock appears only once it
is installed from the Market.

[`forkmesh-ide`](packages/forkmesh-ide/manifest.json) owns the complete source
for the optional VS Code-compatible issue and agent workflow extension. It is
bundled for catalog discovery but has `install.default: false`, so the base
desktop application does not install or activate it. Until the Market gains an
IDE runtime capable of installing VSIX files, the desktop correctly presents it
as catalog/source content rather than claiming an inactive extension is running.

[`forkmesh-sky-office`](packages/forkmesh-sky-office/manifest.json) is the
first *district* package: a whole destination (the aquarium office island, its
glass lift, summit floors, drone roof, and twin-tube hyperloop) instead of one
board. Everything it needs — geometry, rides, floors, access rules, sound
profiles, camera bounds, walk surface, and its own tests — lives in the
package. The World host offers district packages one generic contract:
`install(host)` receives `THREE`, the shared raycast and animation arrays, the
avatar, a sound emitter, floor/space setters, and access hydration, and returns
an instance the scene loops over each frame (`spaces`, `walkSurfaceContains`,
`constrainPlayer`, `update`, `interact`, `interactNearest`,
`cameraDistanceLimit`, `setAccess`, `onWorldReady`, `dispose`). Its spaces are
namespaced `pkg-<package-id>-<floor>`, a shape the presence relay accepts
without knowing which packages exist. The package ships with
`install.default: false`: the generated World module list keeps its code behind
a dynamic `import()`, so it costs nothing until a visitor installs it from World
Settings → Marketplace, and removing it takes every contribution back out.

The next two packages move the World's existing
[`X banner board`](packages/x-banner-board/manifest.json) and
[`Reddit banner board`](packages/reddit-banner-board/manifest.json) into the
same catalog as portable `virtual.object` creations. The World supplies a
bounded rendering host and social-feed snapshot; the packages own their board
identity, appearance, placement, permissions, scene object, and teardown. Both
also ship with `install.default: false`: they stay listed in the Marketplace
and load only once a visitor installs them.

[`voice-changer`](packages/voice-changer/manifest.json) is a free,
desktop-only live voice changer. The desktop changes pitch and vocal tract on
the user's own machine and offers the result as a "ForkMesh Voice" microphone
on Linux, or plays it into a virtual audio cable on Windows and macOS. Its
Worker module only turns a typed description into clamped engine settings
through the host AI capability; no audio reaches the server.

[`speech`](packages/speech/README.md) is a free Speech Playground built
into the iPhone app: type text and hear it read in the phone's most natural
voice, follow the highlighted word, tune speed and pitch, and save the result
as audio. It runs on the device and has no Worker, web or desktop page yet.

[`crm`](packages/crm/manifest.json) and
[`marketing`](packages/marketing/manifest.json) are organization packages with
a web page, a native desktop page and a Worker API each: the CRM keeps
contacts, pipelines, shared sheets and outreach; Marketing keeps a channel
catalog, a content pipeline and an ads manager.
[`competitor`](packages/competitor/manifest.json) keeps an
organization-private competitor list. [`notes`](packages/notes/README.md),
[`tasks`](packages/tasks/README.md) and
[`calendar`](packages/calendar/README.md) own the dashboard pages for host
services (`/api/notes`, `/api/tasks`, `/api/calendar`) that the desktop and
agents also use.

On the desktop every installed package answers the window's top search box,
and removing a package from the rail offers to save its data first: a JSON
bundle for device-local data (`dataLifecycle.localData`), or the package's
lifecycle export for server-side records. Installing such a package again
offers to import the saved copy.

Developer notes (sockets, generators, host runtime, traps, recent changes)
are in [`docs/marketplace/README.md`](../docs/marketplace/README.md).

## Package types

The initial schema reserves the shared namespace described by the product
model: `extension.native`, `extension.vscode`, `virtual.world`,
`virtual.avatar`, `virtual.object`, `virtual.clothing`, `virtual.animation`,
`virtual.audio`, `agent`, `service.agent`, `service.development`,
`service.design`, `service.hosting`, `service.compute`, and `theme`. Adding a
type extends the schema and capability policy; it does not create another
marketplace.

## Publish a package

The catalog is open to ForkMesh members; packages do not have to be owned by the
core `@forkmesh` account. Creator verification is a trust signal, not permission
to participate, so a first submission should set `creator.verified` to `false`.
A maintainer only changes that signal after independently checking the creator
identity. The creator named in the manifest keeps ownership of the work.

Build the archive without copying it into this repository:

1. Include the manifest, package content, licence, and at least one real PNG,
   JPEG, or WebP preview. Use a 16:9 image when possible; each `screenshots`
   entry needs a safe `preview/` path and useful alternative text.
2. Run `python3 marketplace/tools/marketctl.py validate <package-directory>` and
   `marketctl.py pack <package-directory> <package-id>.fmpkg`. The pack command
   prints the deterministic archive's exact size and SHA-256.
3. Host the manifest and archive on public HTTPS, with source on any supported
   Git or HTTPS host, then submit those immutable locations and digests through
   `/market#review`.
4. Five distinct active ForkMesh members with valid SOL addresses approve the
   exact manifest digest. The queue displays those reviewers by checked wallet
   value, highest first. The submitter cannot approve their own submission.
5. A different current owner of the linked `forkmesh/forkmesh` organization
   repository—who did not participate as a member reviewer—makes the final
   approval or records a rejection reason. Only then does the package enter the
   public community catalog.

Publication does not grant an archive arbitrary host execution. Entrypoints and
permissions remain declarative capabilities supplied by the target host, and
the installer activates only content that its host understands.
