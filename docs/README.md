# ForkMesh developer documentation

This is the index of developer documentation for this repository. Source files
hold code only (`AGENTS.md`, "Source is code only"); the explanation of why code
is the way it is belongs on the matching page below. Each subsystem has one home
page with the same headings (Code map, Architecture, Protocols and interfaces,
Configuration, Build/run/test, Operations, Security, Traps, Recent changes,
Related docs). When you move knowledge out of a source comment, put it under the
right heading of the subsystem's home page, or on a topic page next to it and
link that page from the home page.

Public, user-facing documentation lives in [`www/docs/`](../www/docs/README.md)
and is published to forkmesh.com/docs/library by `app/tools/docs_site.py`. Code
and tests refer to many `www/docs` files by path, so do not move or rename them.

Start with [architecture.md](architecture.md) for how the pieces fit together,
then [PLAN.md](PLAN.md) and [OPERATIONS.md](OPERATIONS.md) for the core-node
stack that serves forkmesh.com since 2026-09-24.

## Subsystems

| Subsystem | Code | Home page | Topic pages |
| --- | --- | --- | --- |
| Desktop app (Qt) | `desktop/` | [desktop/README.md](desktop/README.md) | [build-and-test](desktop/build-and-test.md), [diagnostics](desktop/diagnostics.md), [networking](desktop/networking.md), [windows](desktop/windows.md), [git-locking](desktop/git-locking.md), [agents-ui](desktop/agents-ui.md), [lifecycle](desktop/lifecycle.md), [genie](desktop/genie.md), [branch-diffs](desktop/branch-diffs.md); public guide [`www/docs/qt-client.md`](../www/docs/qt-client.md) |
| Agent runs (desktop, mirrors, web, phone) | `desktop/src/*Agent*`, `server/agents.go`, `app/src` | [agents/README.md](agents/README.md) | [tools inventory](agents/tools.md), [memory containment](agents/memory.md), [access rules](agents/access-rules.md), [server/agent-jobs.md](server/agent-jobs.md) |
| Go daemon: mirror, core and runner roles | `server/` | [server/README.md](server/README.md) | [RUNNERS.md](RUNNERS.md), [HOSTED_REPOS.md](HOSTED_REPOS.md), [agent-jobs](server/agent-jobs.md), [merge-executor](server/merge-executor.md), [pull-rules](server/pull-rules.md), [intake-and-attestation](server/intake-and-attestation.md), [actions-runner](server/actions-runner.md), [runner-actions](server/runner-actions.md), [gateways](server/gateways.md), [fleet-operations](server/fleet-operations.md); public [`www/docs/operations/mirror-node.md`](../www/docs/operations/mirror-node.md) |
| App (Python, on the core nodes) | `app/` | [app/README.md](app/README.md) | [PERF_PAGES.md](PERF_PAGES.md), [MARKETPLACE_RATINGS.md](MARKETPLACE_RATINGS.md), [APPS.md](APPS.md), [desktop-commands](app/desktop-commands.md), [auth-sessions](app/auth-sessions.md), [analytics](app/analytics.md), [payments-and-plans](app/payments-and-plans.md), [worker-footprint](app/worker-footprint.md), [cron-and-durable-objects](app/cron-and-durable-objects.md), [d1](app/d1.md), [web-composer](app/web-composer.md), [chat-private-rooms](app/chat-private-rooms.md), [chat-unread-reminders](app/chat-unread-reminders.md), [chat-formatting](app/chat-formatting.md), [chat-direct-calls](app/chat-direct-calls.md), [web-chat-layout](app/web-chat-layout.md), [ecosystem](app/ecosystem.md), [challenges](app/challenges.md), [workflows](app/workflows.md), [feedback-bounties-links](app/feedback-bounties-links.md), [branch-ops](app/branch-ops.md), [desktop-agents-web](app/desktop-agents-web.md), [settings-agents-and-runners](app/settings-agents-and-runners.md), [task-markdown](app/task-markdown.md), [task-trash](app/task-trash.md), [task-board-filters](app/task-board-filters.md), [world-boot-latency](app/world-boot-latency.md), [docs-book](app/docs-book.md), [agents-page](app/agents-page.md), [organization-bot-scopes](app/organization-bot-scopes.md), [update-notice](app/update-notice.md), [not-found-tracking](app/not-found-tracking.md), [badges](app/badges.md), [marketing-email](app/marketing-email.md), [task-email](app/task-email.md), [remote-forks](app/remote-forks.md), [provider-mirrors](app/provider-mirrors.md), [chat-read-sync](app/chat-read-sync.md) |
| Website and docs library | `www/`, `tools/www_push_check.py` | [www/README.md](www/README.md) | [auth-mesh](www/auth-mesh.md), [templates](www/templates.md), [genie-spider](www/genie-spider.md) |
| Office (2D, per organization) | `office/`, `app/src/org_office*.py` | [office/README.md](office/README.md) | |
| World (3D) | `world/` | [world/README.md](world/README.md) | [performance gate](world/README.md#performance-gate) (`tools/world_bench.py`, `world/perf-budget.json`) |
| Native apps (iOS, Android) | `native/` | [native/README.md](native/README.md) | [native/PARITY.md](native/PARITY.md) (owned by the native parity program), [native/DESKTOP_AGENTS.md](native/DESKTOP_AGENTS.md), [native/RUNNER_AGENTS.md](native/RUNNER_AGENTS.md), [native/DESKTOP_PINGS.md](native/DESKTOP_PINGS.md), [native/PING_QUICK_FIXES.md](native/PING_QUICK_FIXES.md), [native/MESH_PUSH.md](native/MESH_PUSH.md), [native/PUSH_ICONS.md](native/PUSH_ICONS.md), [native/REFERENCE_LINKS.md](native/REFERENCE_LINKS.md), [native/DESKTOP_UPDATE.md](native/DESKTOP_UPDATE.md), [native/PROGRESS.md](native/PROGRESS.md), [native/GENIE.md](native/GENIE.md), [native/MAC_BUILD.md](native/MAC_BUILD.md), `native/ios/docs/`, [`www/docs/mobile/`](../www/docs/mobile/) |
| Marketplace packages | `marketplace/` | [marketplace/README.md](marketplace/README.md) | [`marketplace/README.md`](../marketplace/README.md), package READMEs, [marketplace-extraction-inventory.md](marketplace-extraction-inventory.md), [Disk Cleanup](marketplace/cleanup.md), [Computer Security Checkup](marketplace/computer-security.md), [Status Page](marketplace/status-page.md), [Aquarium](marketplace/aquarium.md), [Core features as plugins](marketplace/core-plugins.md) |
| Releases and installers | `tools/forkmesh-release-*`, `tools/package/`, `.forkmesh/releases/` | [release/README.md](release/README.md) | [self-setup-node](release/self-setup-node.md), [`.forkmesh/releases/README.md`](../.forkmesh/releases/README.md), [`www/docs/design/release-binary-publishing.md`](../www/docs/design/release-binary-publishing.md) |
| Security | cross-cutting | [security/README.md](security/README.md) | [security/sentinel.md](security/sentinel.md), [security/agent-host-boundaries.md](security/agent-host-boundaries.md), [security/findings.md](security/findings.md), [VAULT.md](VAULT.md), [IDS.md](IDS.md), [REKEY.md](REKEY.md), [`www/docs/security/`](../www/docs/security/), [`www/docs/protocol/threat-model.md`](../www/docs/protocol/threat-model.md), [dependency-security-audit-2026-09-13.md](dependency-security-audit-2026-09-13.md) |
| Protocol | `conformance/protocol/` | [`www/docs/protocol/README.md`](../www/docs/protocol/README.md) | |
| Finance (CFO special agent): reviews and payouts | `.forkmesh/teammates/cfo.json`, `.forkmesh/skills/cfo-*.skill`, `desktop/src/MainWindowRewardPool.cpp`, `app/src/ledger.py` | [finance/cfo.md](finance/cfo.md) | [app/payments-and-plans.md](app/payments-and-plans.md), [desktop/agents-ui.md](desktop/agents-ui.md#special-agents-formerly-brains-team-and-teammates) |
| Discord community (Hearth special agent) | `tools/discord_community.py`, `.forkmesh/teammates/hearth.json` | [community/hearth.md](community/hearth.md) | [desktop/agents-ui.md](desktop/agents-ui.md#special-agents-not-brains-2026-10-01) |
| Reddit outreach (Reddit special agent) | `tools/reddit_scout.py`, `tools/reddit_profile.py`, `.forkmesh/teammates/reddit-agent.json` | [community/reddit.md](community/reddit.md) | [attribution.md](attribution.md) |
| Website crawl and inspection (Crawler special agent) | `tools/site_crawler.py`, `.forkmesh/teammates/site-crawler.json` | [www/site-crawler.md](www/site-crawler.md) | [www/README.md](www/README.md) |
| Marketing (Marketing manager and Blogger special agents) | `.forkmesh/teammates/marketing.json`, `.forkmesh/teammates/blogger.json`, `marketplace/packages/marketing/` | [marketing/README.md](marketing/README.md) | [attribution.md](attribution.md), [app/marketing-email.md](app/marketing-email.md), [community/reddit.md](community/reddit.md), [community/hearth.md](community/hearth.md) |
| Tools | `tools/` | [tools/README.md](tools/README.md) | [projects-layout.md](tools/projects-layout.md), [mcp-create-agent.md](tools/mcp-create-agent.md), [mcp-agent-windows.md](tools/mcp-agent-windows.md) |
| Conformance suites | `conformance/` | [conformance/README.md](conformance/README.md) | |
| Fleet, node rendering, systemd units, operator tools | `deploy/`, `tools/next_*.py`, `tools/cutover_*.py` | [deploy/README.md](deploy/README.md) | [OPERATIONS.md](OPERATIONS.md), [NEXT_ADOPTION.md](NEXT_ADOPTION.md), `deploy/core/edge_client.md` |
| CPython host for the App | `app/host/` | [host/README.md](host/README.md) | `app/host/RQLITE_FINDINGS.md`, [PHASE0_MEASUREMENTS.md](PHASE0_MEASUREMENTS.md) |

## Stack documents

| Document | What it covers |
| --- | --- |
| [PLAN.md](PLAN.md) | The plan: trusted core and untrusted runners, phases 0 to 7, performance gates; a status section says where the running stack departs from it |
| [OPERATIONS.md](OPERATIONS.md) | Bring-up, verification, routine operations (Turnstile, native builds), recovery, destroy, cost, known gaps, security hardening |
| [VAULT.md](VAULT.md) | Sealed secrets vault and peer unseal |
| [REKEY.md](REKEY.md) | Re-keying the imported database off production's `DATA_KEY` |
| [IDS.md](IDS.md) | Host intrusion detection, lockdown and quarantine |
| [RUNNERS.md](RUNNERS.md) | Runner tier, job sandbox, web sign-in, the sign-in-needed indicator on the desktop and phones, burst pool |
| [HOSTED_REPOS.md](HOSTED_REPOS.md) | Repositories hosted on the core nodes |
| [INSTANCES.md](INSTANCES.md) | "Start your own instance" (`/start`): the form, the SSH setup from ForkMesh's servers, the instance installer, white label, the colour theme, plugins (blog card, Genie spider and status page included), the instance's own docs and desktop pages, email providers |
| [CORE_PUSH.md](CORE_PUSH.md) | Owner pushes over HTTPS to repositories on the core nodes, including `forkmesh/forkmesh` |
| [GIT_ERROR_MESSAGES.md](GIT_ERROR_MESSAGES.md) | What `git` and the desktop show when a clone, pull or push fails: plain-text `remote:` errors, transport failures, the public help page |
| [MIRROR_RETIREMENT.md](MIRROR_RETIREMENT.md) | Retiring mirror35–38 and the old tunnel mirrors mirror2–34: where each role went, backups, DNS |
| [NODE_OWNERSHIP.md](NODE_OWNERSHIP.md) | Which account owns the fleet nodes, and their version fields |
| [NODE_LOGS.md](NODE_LOGS.md) | Following every fleet node's journal on the operator desktop's Log page |
| [fleet/AUTOSCALING.md](fleet/AUTOSCALING.md) | App-capacity and burst-runner autoscaling |
| [PERF_PAGES.md](PERF_PAGES.md) | Page performance passes (repository page, pulls/issues/discussions, compression) |
| [MARKETPLACE_RATINGS.md](MARKETPLACE_RATINGS.md) | Market star ratings with improvement reviews |
| [PHASE0_MEASUREMENTS.md](PHASE0_MEASUREMENTS.md) | Phase 0 memory and timing measurements on one 1 GB node |
| [CUTOVER.md](CUTOVER.md) | Moving production onto this stack on 2026-09-24 (history; owned by the cutover work) |
| [MAIL_LOG.md](MAIL_LOG.md) | The private #email-log notices channel: a note for every outgoing email |
| [APPS.md](APPS.md) | ForkMesh Apps: third-party apps installed through OAuth (in progress, owned by the Apps work) |
| [NEXT_ADOPTION.md](NEXT_ADOPTION.md) | How Next's code and history came into this repository, and the operator-folder move |

## Other documents here

- [engineering/destructive-actions.md](engineering/destructive-actions.md): the rule against unguarded recursive deletes and destructive git commands, the guarded delete helpers in every language, and the contract test that enforces them.
- [engineering/large-file-split-plan.md](engineering/large-file-split-plan.md): ranked plan for splitting the largest source files (dashboard fragments, window tests, MainWindow feature units, entry.py, world.js), which large files are generated, and the source-text tests each move breaks.
- [attribution.md](attribution.md): acquisition attribution (UTM capture, reporting).
- [experiments/public-index-search-to-clone-2026-09.md](experiments/public-index-search-to-clone-2026-09.md): the #478 experiment record.
- [experiments/first-repository-activation-2026-10.md](experiments/first-repository-activation-2026-10.md): the empty-dashboard activation experiment (marketing plan item A1).
- [operations/database-audit-2026-09-10.md](operations/database-audit-2026-09-10.md): D1 audit, with a status block for what has been done since.
- [dependency-security-audit-2026-09-13.md](dependency-security-audit-2026-09-13.md): dependency audit snapshot.
- Repository rules: [`AGENTS.md`](../AGENTS.md), [`CONTRIBUTING.md`](../CONTRIBUTING.md), [`SECURITY.md`](../SECURITY.md), [`deploy-checklist.md`](../deploy-checklist.md).
- [`UPSTREAM.md`](../UPSTREAM.md): the Next repository's seed commit and ported fixes (history).

## Removed or renamed

When a document is deleted, add a line here: the old path and where its
content went.

- The World's "Connect your instance" panel and Cloudflare instance launcher
  ([world/README.md](world/README.md), "Start your world here") and the old
  `/start` Cloudflare + Vultr launcher (`app/src/start_setup.py`) were replaced
  on 2026-10-07 by [INSTANCES.md](INSTANCES.md).
