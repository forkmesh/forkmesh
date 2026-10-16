# ForkMesh developer documentation

Index of the developer documentation. Source files hold code only
(`AGENTS.md`); the reasons behind code live on the matching page here. Each
subsystem has one home page; topic pages sit next to it and are linked from
it. Public, user-facing documentation lives in `www/docs/` and is published to
forkmesh.com/docs; code and tests refer to those files by path, so do not move
or rename them.

Start with [architecture.md](architecture.md), then [PLAN.md](PLAN.md) and
[OPERATIONS.md](OPERATIONS.md) for the stack that serves forkmesh.com.

## Subsystems

| Subsystem | Code | Home page |
| --- | --- | --- |
| Desktop app (Qt) | `desktop/` | [desktop/README.md](desktop/README.md) |
| Agent runs | `desktop/src/*Agent*`, `server/`, `app/src` | [agents/README.md](agents/README.md) |
| Go daemon (mirror, core, runner roles) | `server/` | [server/README.md](server/README.md) |
| App (Python on the core nodes) | `app/` | [app/README.md](app/README.md) |
| CPython host for the app | `app/host/` | [host/README.md](host/README.md) |
| Website and docs library | `www/` | [www/README.md](www/README.md) |
| Office (2D) | `office/` | [office/README.md](office/README.md) |
| World (3D) | `world/` | [world/README.md](world/README.md) |
| Native apps (iOS, Android) | `native/` | [native/README.md](native/README.md) |
| Marketplace packages | `marketplace/` | [marketplace/README.md](marketplace/README.md) |
| Releases and installers | `tools/package/`, release tooling, `.forkmesh/releases/` | [release/README.md](release/README.md) |
| Fleet rendering and operator tools | `deploy/`, `tools/next_*.py` | [deploy/README.md](deploy/README.md) |
| Security | cross-cutting | [security/README.md](security/README.md) |
| Tools | `tools/` | [tools/README.md](tools/README.md) |
| Conformance suites | `conformance/` | [conformance/README.md](conformance/README.md) |
| Marketing | `marketplace/packages/marketing/`, marketing special agents | [marketing/README.md](marketing/README.md) |

## Served stack

| Document | Covers |
| --- | --- |
| [PLAN.md](PLAN.md) | architecture of the served stack and which host serves what |
| [OPERATIONS.md](OPERATIONS.md) | bring-up, routine operations, recovery, hardening |
| [NEXT_ADOPTION.md](NEXT_ADOPTION.md) | operator files and growing the fleet |
| [CUTOVER.md](CUTOVER.md) | the move from the Worker stack, switches and cutover tools |
| [VAULT.md](VAULT.md) | sealed secrets vault and peer unseal |
| [REKEY.md](REKEY.md) | offline `DATA_KEY` rotation |
| [IDS.md](IDS.md) | host intrusion detection, lockdown and quarantine |
| [RUNNERS.md](RUNNERS.md) | runner tier, job sandbox, sign-in, burst pool |
| [fleet/AUTOSCALING.md](fleet/AUTOSCALING.md) | app-replica and burst-runner autoscaling |
| [NODE_LOGS.md](NODE_LOGS.md) | node journals on the operator desktop |
| [NODE_OWNERSHIP.md](NODE_OWNERSHIP.md) | node account ownership and version fields |
| [deploy/auto-deploy.md](deploy/auto-deploy.md) | automatic deploys of `main` |
| [deploy/self-deploy.md](deploy/self-deploy.md) | phone-approved deploys run by the cores |
| [HOSTED_REPOS.md](HOSTED_REPOS.md) | repositories hosted on the cores |
| [CORE_PUSH.md](CORE_PUSH.md) | owner pushes, tag moves and branch deletes on the cores |
| [INSTANCES.md](INSTANCES.md) | self-hosted white-label instances |
| [MAIL_LOG.md](MAIL_LOG.md) | the #email-log channel, mail tracking and core mail delivery |

## Features and topics

- [finance/grants/README.md](finance/grants/README.md): the ranked grant list, its CRM import and one-click ChatGPT drafts.
- [APPS.md](APPS.md): third-party apps through OAuth.
- [marketplace-usage-feed-mastodon.md](marketplace-usage-feed-mastodon.md): privacy and federation boundaries for the Usage, Feed, and Mastodon packages.
- [MARKETPLACE_RATINGS.md](MARKETPLACE_RATINGS.md): Market ratings and reviews.
- [attribution.md](attribution.md): acquisition attribution.
- [PERF_PAGES.md](PERF_PAGES.md): page performance techniques.
- [GIT_ERROR_MESSAGES.md](GIT_ERROR_MESSAGES.md): what failing Git commands show.
- [engineering/destructive-actions.md](engineering/destructive-actions.md): guarded deletes and the contract test.
- [security/NODE_LINK_GRANT.md](security/NODE_LINK_GRANT.md), [security/agent-host-boundaries.md](security/agent-host-boundaries.md), [security/sentinel.md](security/sentinel.md), [security/findings.md](security/findings.md).
- [dependency-security-audit-2026-09-13.md](dependency-security-audit-2026-09-13.md): dependency audit snapshot.
- [release/linux-baseline.md](release/linux-baseline.md), [release/self-setup-node.md](release/self-setup-node.md).

## Special agents

| Agent | Charter |
| --- | --- |
| CFO (financial review, payouts) | [finance/cfo.md](finance/cfo.md) |
| Hearth (Discord community) | [community/hearth.md](community/hearth.md) |
| Reddit scout | [community/reddit.md](community/reddit.md) |
| Site crawler | [www/site-crawler.md](www/site-crawler.md) |
| Sentinel (security) | [security/sentinel.md](security/sentinel.md) |

Repository rules: `AGENTS.md`, `CONTRIBUTING.md`, `SECURITY.md`.
