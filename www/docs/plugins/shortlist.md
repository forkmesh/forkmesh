# Planned plugins: stingy shortlist

> **Planned, not shipped.** This page is the conservative mode of the plugin
> decomposition audit tracked in issue #677. Each family becomes its own issue
> when it is scheduled; until then the code stays where it is. See
> [Planned plugins](README.md) for both modes.

This is the stingy counterpart to the deliberately broad
[plugin opportunity inventory](opportunities.md). It recommends only
package boundaries that are optional to the core ForkMesh promise, cohesive,
independently useful, removable without damaging repository state, and
realistic to expose through a narrow host capability.

The shortlist contains 12 package families. Some families can publish more
than one artifact, but they should share one host contract rather than cause a
new socket for every implementation.

## Admission test

A candidate belongs here only when every answer is **yes**:

1. Can ForkMesh still browse, collaborate on, mirror, and preserve repositories
   when the package is absent?
2. Does the code have one clear owner, lifecycle, configuration surface, and
   reason to version independently?
3. Can install and uninstall be tested without rewriting or orphaning user Git
   history, identity, collaboration records, or security state?
4. Can the host expose a narrow capability instead of arbitrary filesystem,
   process, network, database, or secret access?
5. Is there a credible second implementation, provider, content pack, or user
   group that benefits from choosing whether to install it?

If any answer becomes “no” during implementation, keep the feature in core.

## The shortlist

| Priority | Package family | Existing code to move | Why it passes the high bar | Required host boundary |
| --- | --- | --- | --- | --- |
| 1 | World element trio | `world/public/world/elements/status-obelisk.js`, `aurora-beacon.js`, and `orbit-sculpture.js` | They already implement self-contained manifests/builders, are cosmetic, and can disappear without affecting protocol state. Publish separately if creator ownership or pricing differs. | Existing World entrypoint generalized as `world.object` |
| 2 | Portable operations skills | `.forkmesh/skills/*.skill` and the review, bug, docs, performance, release, and mirror shortcuts under `.forkmesh/shortcuts/` | Reviewable text content has a small attack surface, versions independently, and is useful only to selected teams or agents. | `content.skill` and `content.shortcut`; capabilities remain host-enforced |
| 3 | Analytics (extracted: `marketplace/packages/analytics`) | `www/public/analytics.js` and its consent/event tests | Analytics is optional and should uninstall to zero collection; the first-party package replaced the third-party provider. | `web.analytics-provider` with an event allowlist, consent state, exact origin, and teardown |
| 4 | Patreon membership adapter | `app/src/patreon.py`, `app/src/patreon_handler.py`, related dashboard UI, migrations, and tests | OAuth, webhooks, reconciliation, and provider-owned records form one replaceable integration; ForkMesh membership semantics remain host-owned. | `worker.webhook` plus a narrow `membership.provider` contract and opaque secret handles |
| 5 | APNS notification adapter | `app/src/apns.py`, mobile push/deep-link services, device-token delivery code, and tests | Apple delivery is platform/provider-specific; notification creation, preferences, and unread state can remain core. | `worker.notification-provider` with opaque credentials, bounded payloads, receipts, and token purge |
| 6 | Codex agent adapter | `CodexAppServerSession`, Codex transcript styling, model discovery, usage reporting, and provider protocol | Provider behavior changes independently and users may choose another agent. Worktrees, sandboxing, approvals, and repository mutation stay in the desktop host. | `desktop.agent-provider`; no raw shell or unrestricted worktree handle |
| 7 | Claude Code agent adapter | `ClaudeStreamSession`, `ClaudeSessionScan`, resume identity, transcript view, and IDE bridge | It is a second implementation of the same provider contract and has provider-specific transport/transcript behavior. | The same `desktop.agent-provider` socket used by Codex |
| 8 | Repository statistics panel | `RepoStatsStore`, size/language/activity views, and their tests | It is read-mostly, nonessential, computationally bounded, and naturally competes with other analytics panels. | `desktop.repo-panel` with immutable repository snapshots and cache quota |
| 9 | Developer diagnostics panel | `NodeDiagnostics`, `SystemStats`, `StartupTrace`, `RestartTiming`, runtime diagnostics, and performance tests | Operators and developers need it; ordinary users do not. It reads bounded telemetry and can be removed without losing product data. | `desktop.diagnostics-panel`; crash uploads and destructive actions are excluded |
| 10 | Git conformance toolkit | `tools/git_conformance.py`, conformance fixtures, generated compatibility report logic, and focused tests | It is a standalone verifier with explicit inputs/outputs, useful to mirror operators and Git implementers but unnecessary at runtime. | `cli.command` with disposable-directory, process, time, and output limits |
| 11 | Live sky and satellites | `world/src/world_satellites.py`, `world/public/world/world-sky.js`, satellite snapshot migration, assets, and tests | This is an optional visual/data experience with clear resource budgets and no role in repository correctness. Split sky styling from live satellite data only if each remains useful alone. | `world.object` plus read-only `world.data-source` with draw-call, triangle, request, and update-rate budgets |

## Finish the boundaries already started

Do this before inventing more package types. These are existing packages, not
new shortlist entries:

- Move Calendar, Notes, and Tasks package-specific Worker behavior out of
  `builtin:*` paths and behind the same neutral contribution discovery used by
  package-owned Worker modules.
- Keep CRM, Marketing, and Competitor data schemas, exports, purges, navigation,
  and API routes entirely package-owned.
- Consolidate desktop, organization OAuth, bug-intake, and World-panel Discord
  behavior behind the existing Discord package instead of creating parallel
  core and package implementations.
- Keep Sentry as the contract test for an uninstallable desktop log provider;
  do not add another observability vendor until it uses the same neutral socket.
- Keep X and Reddit boards as the contract tests for World package placement,
  bounded data snapshots, click permissions, and complete scene teardown.
- Replace package-specific `builtin:<feature>` checks with contribution sockets;
  a package manifest must be sufficient for discovery.

## Only ten new socket families

The broad inventory proposes many possible sockets. The stingy plan adds only
the following, and only when the first package using each is ready:

| Socket | Minimum contract |
| --- | --- |
| `world.object` | Package-owned build, placement, bounds, budget declaration, interaction registration, and teardown |
| `world.data-source` | Read-only normalized snapshots, exact origins, size/update limits, backoff, and cache purge |
| `content.skill` / `content.shortcut` | Reviewable content, declared required capabilities, parameters, provenance, and no authority escalation |
| `web.analytics-provider` | Consent gate, fixed event schema, exact origin, no repository content, and uninstall disablement |
| `worker.webhook` | Provider signature verification, bounded body, event allowlist, idempotency, secret handle, and delivery audit |
| `membership.provider` | External-customer mapping and entitlement evidence; plan policy and authorization remain core |
| `worker.notification-provider` | Bounded notification request, opaque provider credential, delivery receipt, token lifecycle, and rate limit |
| `desktop.agent-provider` | Model/session/transcript/usage protocol; host owns approvals, sandbox, worktree, Git, cancellation, and secrets |
| `desktop.repo-panel` / `desktop.diagnostics-panel` | Read-only bounded snapshots, navigation lifecycle, cache quota, refresh, and widget teardown |
| `cli.command` | Explicit executable, arguments, input roots, output schema, timeout, process policy, and disposable workspace |

The paired names in two rows are related specializations of one underlying
host mechanism; they do not justify separate arbitrary-code runtimes.

## Keep these in core

The following broad-inventory ideas fail the stingy admission test or sit below
an enforcement boundary. They may expose data or actions to packages, but their
authoritative implementation should remain host-owned:

- Account authentication, sessions, password recovery, portable identity,
  device trust, private keys, room keys, and signature arming.
- Git object parsing, repository locks, ref validation, worktree quarantine,
  clone/fetch/push correctness, and the mobile owned Git engine.
- Mirror eligibility, signed health, source authority, integrity matching,
  stable clone routing, the direct HTTPS gateway, and the SSH authorization
  broker.
- Issue, discussion, pull, review, merge, project, release, and portable record
  semantics. Optional panels can extend them, but packages must not redefine
  their source-of-truth format.
- Permission evaluation, organization roles, security restrictions, audit-log
  integrity, moderation enforcement, and abuse/rate-limit policy.
- Marketplace manifest/signature verification, dependency resolution,
  installation transactions, migration ledger, quarantine, export, purge,
  rollback, and route allocation.
- Wallet custody, payout arming, transaction signing, reward settlement, and
  financial authorization. A package may display public data or submit a
  narrowly scoped request, never control the trust boundary.
- Native process sandboxing, agent worktree ownership, VM isolation, command
  allowlists, and final approval for repository mutations.
- Core chat encryption and membership enforcement. Provider packages may bridge
  messages only through a scoped, auditable host API.
- World renderer ownership, movement/collision rules, frame and triangle
  budgets, accessibility policy, and asset teardown enforcement.
- The base repository explorer, source-control workspace, issue tracker, pull
  review, chat, notifications, settings, and onboarding. They are central
  product surfaces, not optional Marketplace purchases.

## Stop rules

Do not add a candidate from the broad inventory to this shortlist until all of
the following exist in the same change or an accepted prerequisite:

- A second plausible implementation or a clear group of users who want the
  feature absent.
- A neutral socket with no package IDs or provider names in host code.
- A permission list that does not collapse to “arbitrary code,” “all network,”
  “all repositories,” or “all secrets.”
- Install, activate, deactivate, uninstall, reinstall, upgrade, rollback, and
  crash/restart tests.
- Package-owned settings/cache/data plus tested export and purge behavior.
- Resource budgets and cancellation for network, process, database, rendering,
  or model work.
- A documented degraded state when the provider, package, or external service
  is unavailable.

This smaller plan intentionally leaves most of the broad inventory in core
until real demand and a reusable capability boundary prove otherwise.
