# ForkMesh Marketplace packages

> Developer notes for this directory (sockets, generators, host runtime,
> traps) are in [docs/marketplace/README.md](../docs/marketplace/README.md).

This directory is the source catalog for creations that move through the
ForkMesh ecosystem. Software extensions, virtual goods, agents, themes and
other creations use one package envelope instead of a separate installer per
category.

## Package layout

A distributable `.fmpkg` is a ZIP-compatible archive:

```text
manifest.json
content/
preview/
migrations/                 # optional, but every up file has a down file
LICENSE
SIGNATURE
```

`manifest.json` follows [`manifest.schema.json`](manifest.schema.json): creator,
package type, version, price/licence model, entry points, permissions,
screenshots, dependencies and provenance. Every listing supplies at least one
image under `preview/` with alternative text. `SIGNATURE` signs the canonical
manifest and the digest of every archived file, so mirrors can carry the bytes
but cannot change the creator or content.

The source tree keeps one creation under `packages/<id>/`. A package with a
`content/` folder is a plugin; a build adds only the plugins listed in
[`plugins.json`](plugins.json) (see
[docs/marketplace/plugins.md](../docs/marketplace/plugins.md)), and on Linux
each desktop plugin is its own module beside the core. Install state and UI
contributions go through the same lifecycle as downloaded packages, so
plugins are removable without leaving actions, settings or package-owned data
behind. Checked-in bundled sources inherit the signed release bundle's
integrity and carry no `SIGNATURE`; exporting one as a downloadable `.fmpkg`
must add a creator signature, and a marketplace must reject an external archive
that omits it or whose digests do not verify.

## Catalogs and sources

Catalogs use `schemaVersion: 2`. Each entry carries the package id, version,
safe manifest path, and for remote packages an archive SHA-256, byte size and
ordered locations, each using one transport:

- `local` for a checked-out or installed catalog;
- `git` for any Git URL with optional ref and catalog path;
- `https` for a directly hosted catalog or `.fmpkg` archive.

`provider` is presentation metadata only. `marketplace/sources.example.json`
shows an ordered failover registry. Sync and installation use
`marketplace/tools/marketctl.py`; external artifacts are size-bounded,
digest-checked, path-traversal checked and staged before activation.

Package metadata and assets stay inside the package. Desktop resource and
source lists are generated from package-owned `content/desktop/build.json`
descriptors (which may declare `mocHeaders`, `testSources` and allowlisted Qt
`testLibraries`), and the World module list from declared `world` entrypoints.
Adding a package never requires editing a core allowlist.

Application features use package-owned **contributions**: each names a
host-neutral `socket`, one of the package's `entrypoints`, and socket-specific
`config`. Hosts implement the sockets they know and ignore the rest. The App
provides `dashboard.page` (an HTML entrypoint with route, title, navigation and
an optional script that calls
`window.ForkMeshDashboard.registerPage(pageId, initializer)`) and `worker.http`
(a Python entrypoint under `/api/plugins/<package-id>`). Generated pages,
modules and route tables are written only to ignored `app/dist` and
`app/src_build` staging directories during deployment. Only packages marked
`install.bundled` are activated by the App host; catalog publication alone
grants no execution capability, and `install.default` is a client install-state
choice, not permission to ship server code.

## Lifecycle contract

An installer completes these phases as one logical operation:

1. Validate the manifest, type, version, signature, permissions and content
   paths before activating anything.
2. Record the licence separately from installation state, so uninstalling does
   not erase a purchase or attribution.
3. Activate declared contributions only after installation succeeds.
4. On removal, stop active work, detach every UI contribution, revoke runtime
   capability handles, and delete package-owned settings and cache. User-created
   output and purchase receipts remain in the user's Library.

Database migrations are declared under `lifecycle.migrations`, each with a
unique id and distinct `up` and `down` SQL paths under `migrations/`. Package
SQL cannot control transactions: the host applies ups in order and downs in
reverse order in host-owned transactions. Uninstall quarantines the package,
runs all downs, updates discovery atomically, and restores package and schema if
removal fails. `marketctl install` and `marketctl uninstall` default to
`<install-root>/plugin-state.sqlite`, so both use the same migration ledger. The
desktop treats the verified on-disk catalog as authoritative; external removal
goes through `marketctl` so downs cannot be skipped. ForkMesh instances use the
same engine with `--database-scope organization`; device and organization
migrations have separate ledgers with the same contract.

On the desktop every installed package answers the window's top search box.
Removing a package offers to save its data first (a JSON bundle for
device-local data, `dataLifecycle.localData`, or the package's lifecycle
export for server-side records), and reinstalling offers to import it.

### Dependencies, account installs and settings

- `dependencies: [{"id", "version"}]` names required packages. The catalog
  refuses an unknown id, a cycle, or a dependency whose `install.sync` differs.
  Installing a package installs its dependencies; removing one removes every
  installed package that depends on it.
- `install.sync: "account"` keeps install state and declared setting values on
  the user's ForkMesh account. Desktop, iPhone, Android and the web use
  `/api/plugins/@account`, so installing or removing on one device applies
  everywhere.
- `install.installedBefore: "YYYY-MM-DD"` (only with `install.default: false`)
  installs the package for projects and accounts created before that date, so a
  feature can default off for new projects without disappearing from existing
  ones. The server compares the account's `created_at` (missing counts as old);
  the desktop compares the project's `project.json` `created`
  ([docs/marketplace/core-plugins.md](../docs/marketplace/core-plugins.md)).
- `install.places: ["mobile", "desktop", "web"]` lists the apps an
  account-synced package can appear in (`mobile` is iPhone and Android; all
  three when omitted). Each install keeps the user's own pick from that list.
- `settings: [{"key", "type", "label", "default", "options"}]` declares
  settings every host renders itself: `boolean` (a toggle) and `choice` (a
  picker over `options[].value` strings). The server rejects undeclared keys
  and values.

Status Page and its `status-check-*` checks use all of these
([docs/marketplace/status-page.md](../docs/marketplace/status-page.md)).

The public landing page is [forkmesh.com/market](https://forkmesh.com/market).
Its package list is generated from `catalog.json`; re-run
`python3 app/tools/build_market_catalog.py` after adding or changing a package
(`app/tests/test_market_landing.py` fails while page and catalog disagree).
Planned extractions are described in
[`www/docs/plugins/README.md`](../www/docs/plugins/README.md).

## Notable packages

| Package | What it is |
| --- | --- |
| [`capture`](packages/capture/manifest.json) | Desktop region capture and annotation. |
| [`discord`](packages/discord/manifest.json) | Desktop Discord bot connector; removable without leaving its token, history or avatars. |
| [`ledger`](packages/ledger/manifest.json) | Link to the host's public financial ledger plus private local expense and income records; never handles wallet secrets. |
| [`vault`](packages/vault/manifest.json) | End-to-end encrypted password vault (Argon2id, AES-256-GCM, X25519 + ML-KEM-768 via X-Wing, Ed25519). The server holds no key. Clients: web, browser extension, desktop and mobile. |
| [`browser-extension`](packages/browser-extension/manifest.json) | The ForkMesh Chrome extension for the vault, CRM and Marketing; no worker module. |
| [`analytics`](packages/analytics/manifest.json) | First-party product analytics with a public write-key route, sealed event batches and no stored IP addresses. |
| [`calories`](packages/calories/manifest.json) | Account-scoped calorie diary; each account's rows are sealed with its own key. |
| [`sentry`](packages/sentry/manifest.json) | Read-only Sentry issues on the desktop Log page. |
| [`vscodium`](packages/vscodium/manifest.json) | Adds an "open in VSCodium" action to the branch diff header via `DesktopPackageExtension::branchActions()`. |
| [`winamp`](packages/winamp/manifest.json) | An installable desktop theme (palette plus QSS). |
| [`gemma-4`](packages/gemma-4/manifest.json), `nemotron-*`, `muse-spark-*` | Opt-in agent model packages described by [`agent.schema.json`](agent.schema.json); models are pulled through allowlisted Ollama operations. |
| [`strata`](packages/strata/manifest.json), [`ember`](packages/ember/manifest.json), [`synapse`](packages/synapse/manifest.json) | Desktop codebase analysis for any repository you open: architecture and complexity with technical debt (Strata), file-level hotspots and code health (Ember), and the import graph as a 3D brain (Synapse). `code-map` and `size-map` switch the built-in Code map and Size map. |
| [`timers`](packages/timers/manifest.json) | Top-bar timers, countdowns and alarms with absolute deadlines. |
| [`forkmesh-ide`](packages/forkmesh-ide/manifest.json) | Source for the VS Code-compatible issue and agent extension (catalog content until the Market can install VSIX files). |
| [`forkmesh-sky-office`](packages/forkmesh-sky-office/manifest.json) | A World district package installed through the generic `install(host)` contract and loaded lazily. |
| [`x-banner-board`](packages/x-banner-board/manifest.json), [`reddit-banner-board`](packages/reddit-banner-board/manifest.json) | World `virtual.object` boards over a bounded rendering host. |
| [`voice-changer`](packages/voice-changer/manifest.json) | Desktop live voice changer with a ForkMesh Voice microphone, plus a web page to try voices and record clips; audio never reaches the server. |
| [`face-changer`](packages/face-changer/manifest.json) | Web page with the call face, outfit, background and voice changer, also opened inside the iPhone and Android apps: record and share a clip, snapshot, pop out. Video never reaches the server. |
| [`speech`](packages/speech/README.md) | On-device text to speech in the iPhone app. |
| [`crm`](packages/crm/manifest.json), [`marketing`](packages/marketing/manifest.json) | Organization packages with web and desktop pages and a worker API. |
| [`invoices`](packages/invoices/README.md) | Organization invoices linked to CRM contacts, emailed with a private view-and-pay link, with open, view, pay-click and overdue tracking. |
| [`competitor`](packages/competitor/manifest.json), [`notes`](packages/notes/README.md), [`tasks`](packages/tasks/README.md), [`calendar`](packages/calendar/README.md) | Organization competitor list; dashboard pages for host notes, tasks and calendar services. |

Most optional packages ship with `install.default: false` and appear only once
installed from the Market.

## Package types

The schema reserves a shared namespace: `extension.native`, `extension.vscode`,
`virtual.world`, `virtual.avatar`, `virtual.object`, `virtual.clothing`,
`virtual.animation`, `virtual.audio`, `agent`, `service.agent`,
`service.development`, `service.design`, `service.hosting`, `service.compute`
and `theme`. Adding a type extends the schema and capability policy; it does
not create another marketplace.

## Publish a package

The catalog is open to ForkMesh members; packages need not be owned by the core
`@forkmesh` account. Creator verification is a trust signal, not permission to
participate, so a first submission sets `creator.verified` to `false`; a
maintainer changes it only after independently checking the creator identity.
The creator named in the manifest keeps ownership of the work.

Build the archive without copying it into this repository:

1. Include the manifest, package content, licence, and at least one real PNG,
   JPEG or WebP preview (16:9 when possible); each `screenshots` entry needs a
   safe `preview/` path and alternative text.
2. Run `python3 marketplace/tools/marketctl.py validate <package-directory>` and
   `marketctl.py pack <package-directory> <package-id>.fmpkg`, which prints the
   deterministic archive's size and SHA-256.
3. Host the manifest and archive on public HTTPS, with source on any supported
   Git or HTTPS host, then submit those immutable locations and digests through
   `/market#review`.
4. Five distinct active ForkMesh members with valid SOL addresses approve the
   exact manifest digest (shown by checked wallet value, highest first). The
   submitter cannot approve their own submission.
5. A different current owner of the linked `forkmesh/forkmesh` organization
   repository, who was not a member reviewer, makes the final approval or
   records a rejection reason. Only then does the package enter the public
   community catalog.

Publication grants no arbitrary host execution: entrypoints and permissions
remain declarative capabilities supplied by the target host, and the installer
activates only content its host understands.
