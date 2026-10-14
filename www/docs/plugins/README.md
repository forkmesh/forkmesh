# Planned plugins

Features that may move out of the ForkMesh core into optional Marketplace
packages. Nothing on this page is shipped; each family gets its own issue once
scheduled. The package format and lifecycle are in
[`marketplace/README.md`](../../../marketplace/README.md).

A **plugin** here is a `.fmpkg` package with a manifest: it declares its
entrypoints and permissions, owns its state, removes everything on uninstall,
and has reversible migrations. It is not a library loaded at runtime.

## Admission test

A candidate qualifies only when every answer is yes:

1. ForkMesh still browses, collaborates on, mirrors and preserves repositories
   without it.
2. It has one owner, lifecycle and configuration surface, and a reason to
   version independently.
3. Install and uninstall can be tested without rewriting or orphaning Git
   history, identity, collaboration records or security state.
4. The host can expose a narrow capability instead of arbitrary filesystem,
   process, network, database or secret access.
5. A second implementation, provider, content pack or user group benefits from
   the choice.

## Candidate families

| Family | Host socket |
| --- | --- |
| World decorative elements | `world.object` |
| Portable operations skills and shortcuts | `content.skill`, `content.shortcut` |
| Analytics (already extracted) | `web.analytics-provider` |
| Membership provider adapter | `worker.webhook`, `membership.provider` |
| Push notification adapter | `worker.notification-provider` |
| Coding-agent provider adapters | `desktop.agent-provider` |
| Repository statistics panel | `desktop.repo-panel` |
| Developer diagnostics panel | `desktop.diagnostics-panel` |
| Git conformance toolkit | `cli.command` |
| Live sky and satellite data | `world.object`, `world.data-source` |

Each socket is neutral (no package IDs or provider names in host code), carries
explicit resource budgets, and has a defined teardown.

## Stays in core

Authentication, sessions, identity and keys; Git object parsing, refs and
clone/fetch/push correctness; mirror eligibility, signed health and clone
routing; issue, pull, review, merge and release record formats; permission
evaluation, audit and abuse policy; Marketplace signature verification,
installation and rollback; wallet and payout authority; agent sandboxing and
final approval of repository mutations; chat encryption and membership; World
renderer budgets.

## Before extracting a family

- A neutral socket and a permission list that is not "arbitrary code", "all
  network", "all repositories" or "all secrets".
- Install, activate, deactivate, uninstall, reinstall, upgrade, rollback and
  crash/restart tests.
- Package-owned settings, cache and data with tested export and purge.
- Resource budgets, cancellation, and a documented degraded state when the
  package or its provider is unavailable.
