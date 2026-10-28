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
| World voice, camera and channels | `world/public/world/world-voice*.js`, `world-channels.js`, `app/public/call-avatar-*.js` | [world/voice.md](world/voice.md) |
| World Genie spider | `world/public/world/scene/genie-spider.js`, `scene/runtime/genie-spider.js` | [world/genie-spider.md](world/genie-spider.md) |
| Native apps (iOS, watchOS, Android) | `native/` | [native/README.md](native/README.md) |
| Marketplace packages | `marketplace/` | [marketplace/README.md](marketplace/README.md) |
| Plugins and the core: which plugins a build adds | `marketplace/plugins.json`, `marketplace/tools/marketplace_lib.py`, `desktop/cmake/ForkMeshPackageModules.cmake`, `app/tools/build_app_plugins.py` | [marketplace/plugins.md](marketplace/plugins.md) |
| Codebase analysis (Code Map, Size Map, Strata, Ember, Synapse) | `desktop/src/RepoInsight*`, `marketplace/packages/{code-map,size-map,strata,ember,synapse}/`, `native/ios/ForkMeshCore/Sources/FMCodeInsight`, `native/android/core/.../codeinsight`, `native/fixtures/insight` | [marketplace/codebase-analysis.md](marketplace/codebase-analysis.md) |
| Releases and installers | `tools/package/`, release tooling, `.forkmesh/releases/` | [release/README.md](release/README.md) |
| Changelog (forkmesh.com/changelog and the apps' What's new) | `changelog/changelog.json`, `tools/changelog.py` | [changelog.md](changelog.md) |
| Fleet rendering and operator tools | `deploy/`, `tools/next_*.py` | [deploy/README.md](deploy/README.md) |
| Security | cross-cutting | [security/README.md](security/README.md) |
| Tools | `tools/` | [tools/README.md](tools/README.md) |
| Conformance suites | `conformance/` | [conformance/README.md](conformance/README.md) |
| Marketing | `marketplace/packages/marketing/`, marketing special agents | [marketing/README.md](marketing/README.md) |
| Invoicing (CRM-linked invoices, email, open tracking) | `marketplace/packages/invoices/` | [marketplace/invoices.md](marketplace/invoices.md) |

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
| [INSTANCES.md](INSTANCES.md) | self-hosted white-label instances started from `forkmesh.com/start`; user guide [www/docs/start-your-own-instance.md](../www/docs/start-your-own-instance.md) |
| [MAIL_LOG.md](MAIL_LOG.md) | the #email-log channel, mail tracking and core mail delivery |

## Features and topics

- [finance/grants/README.md](finance/grants/README.md): the ranked grant list, its CRM import and one-click ChatGPT drafts.
- [APPS.md](APPS.md): third-party apps through OAuth; `examples/apps/hello-app` is the worked example.
- [desktop/networking.md](desktop/networking.md) ("Visualizer"): the desktop Log → Visualizer, including opening a table bubble in the Database tab.
- [app/account-data.md](app/account-data.md): Settings > Data: phone settings saved encrypted to the account, the inventory of everything ForkMesh keeps about a person, the export and per-item delete.
- [marketplace-usage-feed-mastodon.md](marketplace-usage-feed-mastodon.md): privacy and federation boundaries for the Usage, Feed, and Mastodon packages.
- [MARKETPLACE_RATINGS.md](MARKETPLACE_RATINGS.md): Market ratings and reviews.
- [attribution.md](attribution.md): acquisition attribution.
- [app/instance-legal-pages.md](app/instance-legal-pages.md): Terms, Privacy and Security pages on self-hosted instances.
- [app/ai-review.md](app/ai-review.md): automated AI review of pull requests with Cloudflare Workers AI (instance settings, per-repository opt-in, budgets, privacy, prompt-injection handling).
- [app/privacy-mode.md](app/privacy-mode.md): privacy mode for self-hosted instances: no visitor logs, what remains and why.
- [app/accounts-and-moderation.md](app/accounts-and-moderation.md): email-free sign-up with recovery codes, random usernames, moderators, blocks, hidden posts, locked threads, first-post approval and new-account limits.
- [app/instance-federation.md](app/instance-federation.md): self-hosted instances joining the forkmesh.com directory and the ForkMesh World.
- [app/security-advisories.md](app/security-advisories.md): private vulnerability reporting, security advisories (OSV export, signed repository ledger) and researcher credits.
- [app/recent-push-banner.md](app/recent-push-banner.md): the "had recent pushes · Compare & pull request" bar for branches the signed-in account just pushed.
- [app/comment-reactions.md](app/comment-reactions.md): emoji reactions on issue, pull request and discussion comments (storage, API, limits, first paint, picker, gratitude group and Thank you sticker, custom reactions, animation).
- [app/translations.md](app/translations.md): on-device post translation (browser Translator API, no network) and signed community translations attached to posts.
- [app/repo-media.md](app/repo-media.md): uploading videos into issue and pull request markdown, stored in the replicated database and served with Range support.
- [app/pull-revisions.md](app/pull-revisions.md): updating a pull request with new commits or a rewritten branch (signed revisions, no force push).
- [app/comment-edits.md](app/comment-edits.md): editing and deleting your own issue and pull request comments (signed `comment-edit`/`comment-delete` events).
- [app/comment-gifs.md](app/comment-gifs.md): GIFs and stickers in comment composers (GIPHY proxy, key handling, rendering, reduced motion).
- [app/seasonal-theme.md](app/seasonal-theme.md): the Halloween theme an administrator or moderator switches on in Settings, Appearance (orange colours, spider web, witch hats, auto-off).
- [app/comment-tips.md](app/comment-tips.md): wallet-to-wallet Solana Pay tips on comments (requests, read-only verification, limits, settings).
- [app/branch-protection.md](app/branch-protection.md): protected default branches (merge only an owner- or admin-approved pull request) and the per-repository Actions switch.
- [app/username-autocomplete.md](app/username-autocomplete.md): the shared username picker on chat channel invites, new channels, repository collaborators and advisory credits, and account search behind every @mention list.
- [app/profile-links.md](app/profile-links.md): usernames and @mentions link to the person's profile on the web, desktop, server pages, world and phones.
- [app/diff-viewer.md](app/diff-viewer.md): the full-screen web diff viewer for pull requests, branch comparisons and commits.
- [app/issue-views.md](app/issue-views.md): the per-issue view count on issue pages and the issue list, deduplicated per viewer and day, with automated clients not counted.
- [app/issue-assignment-deadline.md](app/issue-assignment-deadline.md): the optional assignee deadline bar on issues, set with `assignmentHours` in `.forkmesh/issues.json`.
- [app/issue-assignment-limit.md](app/issue-assignment-limit.md): the issue page's Assign button and the one-open-issue rule for self-assignment (freed by a linked pull request), set with `assignmentLimit` in `.forkmesh/issues.json`.
- [app/bot-avatars.md](app/bot-avatars.md): built-in avatars for bots and system accounts (Actions, git node, runners, AI review, `[bot]` names) and the account override.
- [app/profile-avatar.md](app/profile-avatar.md): how profile and header avatars fall back to the account lookup and contributor rosters, like issue avatars.
- [app/ai-disclosure.md](app/ai-disclosure.md): the "AI model(s) used" field on pull requests (optionally required per repository) and the copyable AI prompt on open issues.
- [app/pull-review-prompt.md](app/pull-review-prompt.md): the "Diff and files" box on open pull requests: diff, patch and file links, Summarize in ChatGPT, and the copyable review prompt with its security-minded default and `.forkmesh/pulls.json` `reviewPromptTemplate`.
- [PERF_PAGES.md](PERF_PAGES.md): page performance techniques.
- [GIT_ERROR_MESSAGES.md](GIT_ERROR_MESSAGES.md): what failing Git commands show.
- [Folder About boxes](../www/docs/folder-about.md): the `<folder>/.forkmesh/info.json` file format for a folder's own About box and logo (user guide); [app/directory-about.md](app/directory-about.md) has the API, caching and permissions.
- [engineering/destructive-actions.md](engineering/destructive-actions.md): guarded deletes and the contract test.
- [engineering/github-actions.md](engineering/github-actions.md): pings when a GitHub Actions workflow on `main` breaks or recovers, `tools/github_actions_status.py`, and failure annotations readable without a GitHub login.
- [security/NODE_LINK_GRANT.md](security/NODE_LINK_GRANT.md), [security/agent-host-boundaries.md](security/agent-host-boundaries.md), [security/sentinel.md](security/sentinel.md), [security/findings.md](security/findings.md).
- [dependency-security-audit-2026-09-13.md](dependency-security-audit-2026-09-13.md): dependency audit snapshot.
- [release/linux-baseline.md](release/linux-baseline.md), [release/self-setup-node.md](release/self-setup-node.md).

## Special agents

Each special agent is a workflow in `.forkmesh/workflows/` that the desktop
runs; see [app/workflows.md](app/workflows.md#desktop-workflows-and-special-agents).

| Agent | Charter |
| --- | --- |
| CFO (financial review, payouts) | [finance/cfo.md](finance/cfo.md) |
| Hearth (Discord community) | [community/hearth.md](community/hearth.md) |
| Reddit scout | [community/reddit.md](community/reddit.md) |
| Site crawler | [www/site-crawler.md](www/site-crawler.md) |
| Sentinel (security) | [security/sentinel.md](security/sentinel.md) |

## Coding agents

`app/public/forkmesh-agent.md`, served at
`https://app.forkmesh.com/forkmesh-agent.md`, is the one guide written for
outside coding agents: starting an instance, working on a repository, and the
pull request workflow. The start page copies a one-line prompt that points an
agent at it. Keep it in step with [INSTANCES.md](INSTANCES.md),
`www/docs/start-config.md` and the pull request section of `CONTRIBUTING.md`;
`app/tests/test_start_instance_page.py` checks the parts that must match.
User guide: [www/docs/coding-agents.md](../www/docs/coding-agents.md).

Repository rules: `AGENTS.md`, `CONTRIBUTING.md`, `SECURITY.md`.
