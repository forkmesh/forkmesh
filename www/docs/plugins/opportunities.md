# Planned plugins: opportunity inventory

> **Planned, not shipped.** This page is the generous mode of the plugin
> decomposition audit tracked in issue #677. Rows are candidates, not
> commitments; see [Planned plugins](README.md) for both modes.

This is a deliberately generous inventory of code and content that could move
into ForkMesh Marketplace packages. It is not a promise that every row should
be extracted. The list includes both small, independently useful packages and
larger alternative bundles so product and creator experiments are not ruled out
too early.

For the conservative recommendation, see the
[stingy plugin shortlist](shortlist.md).

The tables contain 316 candidate package rows, plus 15 package boundaries that
already exist and 44 proposed host sockets. Bundle rows deliberately overlap
their smaller components.

In this document, **plugin** means a manifest-backed ForkMesh `.fmpkg`, not a
random dynamically loaded library. A real extraction must keep the Marketplace
lifecycle contract: explicit entrypoints and permissions, validated content,
package-owned state, complete teardown, and reversible migrations.

## Audit coverage

The 2026-09-01 audit mechanically inventoried all 4,926 tracked repository
files, including 1,742 code-like files and 686 tests. It reviewed the functional
boundaries under `app/`, `desktop/`, `marketplace/`, `mobile/`, `server/`,
`tools/`, `world/`, `www/`, and the portable `.forkmesh/` skills and shortcuts.
Generated assets and test fixtures informed the inventory but are not proposed
as standalone plugins merely because they are separate files.

The strongest evidence is already in the source layout:

- Desktop product surfaces are split across `MainWindow*.cpp` files and focused
  stores/runtimes, but most still compile into the core application.
- App features are split into route modules such as `notes.py`, `calendar_api.py`,
  `activitypub.py`, `security_scan_ingest.py`, and `repository_migration.py`.
- World features already have separable backend modules, front-end modules, and
  self-contained element implementations.
- Mobile services and screens are separated, and the owned Git engine is
  already its own module in each native app (`FMGit` on iOS, the
  `nativeapp.git` package on Android).
- Headless operations are mostly one-purpose Python tools or focused Go server
  modules.
- The test suite names many coherent capabilities that are less obvious from
  navigation alone: direct messages, repository migrations, referral controls,
  reward scheduling, workshops, QA boards, satellites, private mirrors, and
  provider-specific agent transports.

Readiness labels used below:

- **Now** — an existing host socket or package adapter is close to sufficient.
- **Socket** — a good package boundary, but the host needs a neutral extension
  socket before extraction.
- **Content** — primarily declarative content, a theme, an agent/skill, or a
  World object; it should need little or no privileged runtime code.
- **Guarded** — plausible only as a signed first-party or administrator-approved
  package with narrow capabilities and fail-closed host policy.
- **Bundle** — a useful collection of smaller candidates; it intentionally
  overlaps atomic rows.

## Already represented by packages: finish and preserve the boundary

These are not future ideas: their manifests or content already live under
`marketplace/packages/`. They should be treated as reference extractions and
expanded without adding package-id branches back to the hosts.

| Package | Current package surface | Generous follow-on variants |
| --- | --- | --- |
| Calendar | Dashboard, desktop, Worker-backed calendar | CalDAV adapter, ICS import/export, team on-call calendar, release calendar, repository activity calendar |
| Capture | Native region capture and markup | OCR, redaction, scrolling capture, screen recording, issue/PR attachment workflow |
| Competitor Watch | Organization dashboard and Worker data | Pricing monitor, release monitor, positioning templates, public-site change alerts |
| CRM | Dashboard, native view, Worker data, Zoho Mail import | Provider adapters, deal pipelines, contributor relations, sponsor CRM, support inbox |
| Discord | Desktop chat connector | Organization OAuth setup, bug intake, webhook bridge, moderation console, World panel |
| ForkMesh for VS Code | VS Code issue-to-agent workflow | JetBrains, Zed, Neovim, Visual Studio, Eclipse, browser IDE variants |
| Gemma 4 | Ollama-backed local coding agent | Other Ollama model families, llama.cpp, MLX, local embedding and reranking models |
| Ledger | Desktop financial view | Expense export, tax lots, organization budgets, reward reconciliation, accounting-provider adapters |
| Marketing | Dashboard, desktop, Worker data | Editorial calendar, campaign templates, channel analytics, social publishing adapters |
| Notes | Dashboard and Worker-backed collaborative notes | Daily notes, meeting notes, wiki, ADRs, encrypted notebook, publish-to-docs |
| Reddit Banner Board | World object and public feed snapshot | Subreddit-specific boards, moderation board, topic packs |
| Sentry | Desktop log source and issue page | Datadog, Bugsnag, Rollbar, Grafana, OpenTelemetry, Cloudflare logs |
| Tasks | Dashboard and Worker-backed organization tasks | Personal tasks, kanban, incident tasks, support queue, release checklist |
| Winamp Classic | Declarative desktop theme | High contrast, solarized, terminal, branded organization themes, seasonal skins |
| X Banner Board | World object and public feed snapshot | Lists, hashtags, creator feeds, alternative social-network boards |

## Desktop and local-node candidates

These candidates are supported by the focused `desktop/src/MainWindow*.cpp`,
store, runtime, and integration classes. A row can become a standalone package
or be combined with adjacent rows as a suite.

| Candidate | What moves into the package | Readiness |
| --- | --- | --- |
| Repository explorer | Repository list, detail, README, metadata, and recent activity UI | Socket |
| File tree | Lazy tree browsing, file icons, binary handling, and file actions | Socket |
| Code viewer | Syntax-aware blob display, line links, blame affordances, and bounded reads | Socket |
| Code symbol navigator | Symbol extraction and jump-to-symbol UI from `CodeSymbols` | Socket |
| Global search | Repository/global search providers, result ranking, and `GlobalSearchMatch` presentation | Socket |
| Markdown workbench | `MarkdownEditor`, preview, task lists, mentions, and attachment affordances | Socket |
| Spell-check provider | `PromptSpellCheckHighlighter` and selectable language dictionaries | Now |
| Source-control workspace | Status, diff, stage, commit, outgoing changes, and worktree summary | Socket |
| Branch manager | Branch list, create/delete/rename, upstream state, and checkout | Socket |
| Commit graph | Commit graph rendering, commit detail, and comparison UI | Socket |
| Upstream sync | `UpstreamCheckoutSync`, fetch/rebase/pull policy, and conflict reporting | Socket |
| Native terminal | `TerminalWidget`, repository environment setup, and terminal profiles | Guarded |
| Issue tracker | `IssueStore`, issue list/detail/editor, filters, subissues, dates, and signed events | Socket |
| Issue analytics | `IssueBurnup`, aging, throughput, and reporter/assignee views | Socket |
| Discussion client | `DiscussionStore`, inbox, threads, reactions, and federation projections | Socket |
| Pull-request client | `PullStore`, pull list/detail, files, commits, checks, and conversation UI | Socket |
| Pull review workbench | `PullReviewModel`, viewed-file state, inline comments, and review submission | Socket |
| AI pull reviewer | `PullAiReview`, prompt policy, evidence display, and provider choice | Socket |
| Pull badges | `PullBadgeWidget` and embeddable merge/check/reward state | Now |
| Merge queue | `MergeQueue`, queue policy, update branch, merge controls, and conflict state | Socket |
| AI conflict resolver | Conflict explanation and guarded agent-assisted resolution workflow | Guarded |
| Project boards | `ProjectStore`, kanban views, milestones, fields, and project filters | Socket |
| Project Gantt | `ProjectGantt` scheduling and dependency visualization | Now |
| Release manager | Release list/editor, tags, artifacts, checksums, and publish workflow | Guarded |
| Actions dashboard | `ActionStore`, workflow/run lists, logs, checks, and artifacts | Socket |
| Local action runner | `ActionRunner`, sandbox policy, cancellation, artifact capture, and labels | Guarded |
| Action telemetry | `ActionTelemetry`, run duration, reliability, and resource reporting | Now |
| Repository statistics | `RepoStatsStore`, language mix, size maps, activity, and contributor stats | Now |
| Encrypted Coves | `CoveStore`, `CoveCrypto`, file editor, and sharing workflow | Guarded |
| Chat client | Rooms, history, threads, sidebar channels, composer, and transcript UI | Socket |
| Direct messages | Private conversation list, device membership, unread state, and transcript | Socket |
| Chat crypto provider | `RoomCrypto`, `RoomSeal`, key rotation, and device envelope handling | Guarded |
| Reactions and emoji pack | `ReactionCatalog`, custom emoji, reaction rendering, and organization packs | Content |
| Presence provider | Visitor presence, typing state, account status, and activity badges | Socket |
| Agent workspace | Agent list/detail, transcripts, steering, diffs, and completion actions | Socket |
| Agent provider: Codex | `CodexAppServerSession`, model discovery, usage limits, and resumable runs | Socket |
| Agent provider: Claude Code | `ClaudeStreamSession`, transcript parsing, usage, and resume identity | Socket |
| Agent provider: Cloudflare | `CloudflareAgentScript` and remote execution adapter | Socket |
| IDE handoff bridge | `ClaudeIdeBridge` plus generic issue/prompt handoff contracts | Socket |
| Agent worktree manager | `AgentWorktree`, isolation, cleanup, branch naming, and diff collection | Guarded |
| Agent sandbox profiles | `AgentJail`, VM-backed runtime, filesystem/network profiles, and policy UI | Guarded |
| Virtual-machine runner | `VirtualMachineRuntime` and provider-specific VM backends | Guarded |
| Agent prompt attachments | `AgentPromptImages`, image lifecycle, limits, and provider conversion | Now |
| Agent transcript themes | `CodexTranscriptStyle`, Claude transcript presentation, and custom renderers | Content |
| Teammate automation | `TeammateStore`, schedules, skills, model routing, activity, and permissions | Socket |
| Skill usage analytics | `SkillUsageChart`, invocation history, cost, and outcome summaries | Now |
| MCP connector | `McpConnector`, scoped tokens, capability selection, and server status | Guarded |
| Mirror control center | Node setup, service controls, health, routing, and recovery UI | Guarded |
| Public mirror runtime | `PublicMirrorRuntime`, inventory, setup, and serving status | Guarded |
| Private mirror runtime | `PrivateMirrorStore`/`PrivateMirrorRuntime`, routes, auth, and lifecycle | Guarded |
| Mirror Actions | `MirrorActionsConfiguration` and `MirrorActionsSummary` | Guarded |
| Local backup provider | `LocalBackupStore`, schedules, retention, verification, and restore | Guarded |
| Server-node provisioner | `ServerNode` flows for supported hosting providers | Guarded |
| Control-node dashboard | `ControlNode`, diagnostics, service state, and administrative actions | Guarded |
| Node diagnostics | `NodeDiagnostics`, `SystemStats`, runtime checks, and recommendations | Now |
| Crash diagnostics | `CrashHandler`, `CrashLogScan`, client error reports, and consent UI | Guarded |
| Log explorer | `LogSource`, network archive, filters, activity lights, and timeline chart | Socket |
| Startup profiler | `StartupTrace`, `RestartTiming`, splash timings, and regression display | Now |
| Organization manager | Organization, team, membership, invitation, and role UI | Socket |
| Membership provider UI | Paid membership state, prompts, and provider-specific management links | Socket |
| Rewards center | Reward pool, contributor estimates, payout state, and receipt views | Guarded |
| Tip workflow | `TipSigner`, notices, recipient selection, and public receipt display | Guarded |
| Wallet provider | Local wallet linkage, balance, signing policy, and chain adapters | Guarded |
| QR toolkit | `QrCode`, mobile handoff, clone, invite, and identity QR flows | Now |
| Onboarding tour pack | `OnboardingTour` steps for roles or organization templates | Content |
| Suggested-app catalog | `SuggestedApps` recommendations and provider install links | Content |
| Keyboard/shortcut pack | `MainWindowShortcuts`, role-specific bindings, and command palettes | Content |
| World speech bridge | `WorldSpeechBridge`, dictation, speech-to-text providers, and controls | Socket |
| Desktop notification providers | Native notifications, sounds, quiet hours, and notification actions | Socket |
| Accessibility pack | Alternate contrast, reduced motion, larger controls, screen-reader labels, and audits | Content |
| Developer diagnostics bundle | Logs, system stats, startup profiling, crash reports, and action telemetry | Bundle |
| Complete forge bundle | Explorer, issues, discussions, pulls, projects, releases, and Actions | Bundle |
| AI developer bundle | Agent workspace, providers, worktrees, AI review, and conflict assistance | Bundle |

## App, API, and organization candidates

The App already supports `dashboard.page` and `worker.http`. Many rows can use
those today; scheduled work, event delivery, Durable Objects, or repository
mutation need new host sockets before extraction.

| Candidate | Package-owned surface | Readiness |
| --- | --- | --- |
| ActivityPub federation | Actors, inbox/outbox, followers, comments, repository actors, and settings | Socket |
| Fediverse digest | Digest queues, organization settings, delivery, and poll summaries | Socket |
| Fediverse mention review | Mention collection, moderation, issue conversion, and World projection | Now |
| Lemmy/thread federation adapter | Threaded collaboration projection for Lemmy-compatible hosts | Socket |
| Portable identity | Export/import UI, portable sessions, device claims, and signed handoff | Guarded |
| Account event channel | Revocation, profile changes, device cleanup, and live account events | Guarded |
| Password recovery provider | Reset requests, rate limits, delivery adapter, and reset UI | Guarded |
| SSH key manager | Key registration, verification, revocation, and account display | Guarded |
| APNS push provider | Device tokens, push envelopes, deep links, and delivery telemetry | Socket |
| Web-push provider | Browser subscription, push delivery, actions, and expiry cleanup | Socket |
| Notification center | Notifications, subscriptions, digest detail, unread counts, and preferences | Socket |
| Channel webhooks | Incoming/outgoing repository chat webhooks and delivery history | Socket |
| Organization Discord connector | OAuth, setup tasks, channel mapping, and message bridge | Now |
| Slack connector | OAuth, channel mapping, issue actions, and notification bridge | Socket |
| Matrix connector | Room mapping, encrypted bridge, and collaboration actions | Socket |
| Email delivery adapter | Transactional templates, provider API, bounces, and delivery audit | Socket |
| Mailtrap event adapter | Authenticated event ingestion and package webhook fan-out | Now |
| ForkBot | Repository-room assistant, commands, safety policy, and response hooks | Socket |
| Agent guestbook | Public rate-limited agent greeting/message endpoint and moderation | Now |
| Organization agent bots | Agent sessions, job queue, bot tokens, usage, and organization UI | Socket |
| Agent provider gateway | Provider-neutral model/session routing for App and organization agents | Socket |
| Notes publishing | Public note views, snapshots, links, view counts, and docs export | Now |
| Calendar reminders | Reminder scheduler, invitations, attendee responses, and delivery adapters | Socket |
| Microjobs | Job listing, claims, completion, disputes, and dashboard | Now |
| Referral/affiliate system | Attributions, click dedupe, stats, burst quarantine, and admin controls | Now |
| Patreon membership adapter | OAuth, account links, webhook handling, and reconciliation | Now |
| Patreon membership adapter | Account linking, memberships, webhooks, and paid feature status | Now |
| Generic entitlement provider | Provider-neutral plan, seat, entitlement, and lifecycle API | Socket |
| Marketplace governance | Submissions, member approvals, final review, install counts, and ratings | Now |
| Marketplace storefront | Catalog browsing, package pages, ratings, install state, and review queue | Now |
| Creator analytics | Installs, active versions, ratings, revenue, and package health | Socket |
| Package data export | Organization-scoped plugin export, purge, and migration status | Now |
| Repository importer | Import jobs, contributor invitations, logos, progress, and retries | Socket |
| Repository migration service | Object migration, run history, source authority, and cutover | Guarded |
| Repository fork service | Fork jobs, provenance, borrowed objects, and dashboard flow | Guarded |
| GitHub migration adapter | GitHub-specific repository, issue, pull, and release import | Guarded |
| GitLab migration adapter | GitLab-specific repository and collaboration import | Guarded |
| Bitbucket migration adapter | Bitbucket-specific repository and collaboration import | Guarded |
| Repository logo service | Logo upload, suggestions, approval, storage, and native display | Now |
| Repository social cards | Open Graph image generation, banner metadata, and caching | Now |
| Repository stars/follows | Star state, follower lists, activity, and social projections | Now |
| Repository alerts | Alert rules, delivery targets, and operational transition notifications | Now |
| Repository terms/moderation | Terms flags, privacy memo, content moderation, and appeal flow | Guarded |
| Security scan ingest | Lease/queue, findings ingestion, review, and repository status | Socket |
| Security scanner provider | OSV/native scan runners, result normalization, and evidence | Guarded |
| Security report intake | Public report form, encryption, triage, status, and response workflow | Guarded |
| Abuse-control console | Block rules, quarantines, restrictions, appeals, and audit log | Guarded |
| Organization succession | Policies, cases, approvals, events, and continuity workflow | Guarded |
| Admin console | Usage reports, error actions, capacity, verification override, and controls | Guarded |
| Status monitor | Health sampling, deploy grace, alert transitions, and public status data | Socket |
| Public status page | Status history, incidents, component state, and subscriber links | Now |
| API metrics | Privacy-bounded minute metrics, capacity views, and exports | Now |
| Public API snapshots | Cached public datasets, republish policy, and mirror counters | Now |
| Leaderboards | Contributors, reporters, tips, mirror nodes, and partial-failure handling | Now |
| Badges/achievements | Award rules, badge catalog, account display, and notifications | Socket |
| Release service | Release metadata, downloads, signatures, appcast, and republish protection | Guarded |
| Release publisher | Artifact verification, signing, upload, and publication workflow | Guarded |
| Mirror payout policy | Contribution observations, rounds, intents, splits, and receipts | Guarded |
| Contributor rewards | Reward estimates, reminders, settlement, and repository records | Guarded |
| Ledger chain indexer | Public chain backfill, transactions, balances, and reconciliation | Guarded |
| Solana adapter | RPC reads, transaction verification, address validation, and displays | Guarded |
| Alternative chain adapter | Equivalent read/verify support for another network without changing core | Guarded |
| Operating-expense tracker | Expense records, budgets, reports, and organization access | Now |
| Blog feed service | RSS, post metadata, metrics, referrers, links, and social reach | Now |
| Changelog generator | Releases/commits to authored changelog entries and feeds | Now |
| Feedback inbox | Feedback collection, email response, triage state, and analytics | Now |
| Public contribution profile | Contribution receipts, projects, days, languages, and profile views | Now |
| Repository collaboration bundle | Issues, discussions, pulls, projects, releases, and notification API projections | Bundle |
| Community growth bundle | Referrals, badges, leaderboards, blog analytics, outreach, and guestbook | Bundle |
| Organization continuity bundle | Teams, tasks, notes, calendar, succession, audit, and exports | Bundle |

## World candidates

World is the richest content-plugin opportunity. `elements/*.js` already proves
the self-contained `manifest` plus `build` pattern, while other modules show
natural packages for buildings, panels, activities, data feeds, and avatar
customization.

| Candidate | Package-owned World surface | Readiness |
| --- | --- | --- |
| Status Obelisk | Existing self-contained element, moved into a catalog package | Content |
| Aurora Beacon | Existing self-contained element, moved into a catalog package | Content |
| Orbit Sculpture | Existing self-contained element, moved into a catalog package | Content |
| Element starter kit | Template, manifest validation, lifecycle, bounds, and teardown examples | Content |
| Element store | Purchases, inventory, placement, organization policy, and creator attribution | Socket |
| Build board | Board items, placement, edit workflow, and moderation | Now |
| Link kiosk | Curated links, lobby placement, access policy, and click handling | Now |
| QR kiosk | QR generation, repository/invite destinations, and branded object variants | Content |
| Repository graph | Repository/entity graph rendering and live data adapter | Socket |
| Mirror-node cabinet | Live mirror health, serve/commit activity, and cabinet visualization | Socket |
| Infrastructure console | Sanitized live status, node controls, diagnostics, and admin gating | Guarded |
| General chat board | Room transcript, composer, reactions, and proximity interactions | Socket |
| Issue box | Issue summaries, quick filing, assignment, and proximity effects | Socket |
| Pull-review room | Diff tree, viewed state, mergeability, comments, and merge request | Guarded |
| Task bulletin | Organization task cards, check-ins, agent runs, and QA state | Now |
| Notes wall | Shared/public note cards, links, snapshots, and access policy | Now |
| Discord panel | Connector setup, channel selection, messages, and OAuth outcome | Now |
| Mastodon wall | Profile/status normalization, public feed, links, and count display | Now |
| Fediverse directory | Instance/member directory, filters, profiles, and moderation | Now |
| Social banner framework | Generic bounded feed board used by X, Reddit, Mastodon, and future networks | Now |
| Community ads | Proposals, voting, moderation, placement, instance policy, and revenue | Now |
| Events venue | World events, schedules, attendance, and event objects | Socket |
| Office meeting room | Realtime meeting participants, movement queue, room UI, and teardown | Socket |
| QA board | QA items, reviews, moderation, and result display | Now |
| Office tower | Building geometry, floors, team access, elevator, and shell lifecycle | Content |
| Sky office | Island/building geometry, floor access, aquarium, lounge, and hyperloop | Content |
| Custom organization office | Layout presets, brand materials, access rules, and remote-office portal | Content |
| Office attendance | Visit tracking, team occupancy, privacy bounds, and aggregate display | Socket |
| Marketing command center | Initiatives, proofs, check-ins, tasks, and office visualization | Now |
| Repository district | District arrival, repository plots, followers, and navigation | Content |
| Instance booth | Relay-instance profile, health, member lifecycle, and travel link | Content |
| Social directory booth | Members, bots, organizations, filters, and profile launches | Now |
| World sky | Procedural stars, quality bounds, planets, colors, and day/night mix | Content |
| Live satellites | Snapshot backend, OMM propagation, rendering, and information panel | Socket |
| Astronomy pack | Alternative constellations, planets, observation events, and sky tours | Content |
| Landscape biome | Beach, void, city, paths, vegetation, water, and collision surfaces | Content |
| Marine aquarium | Aquarium geometry, animals, access, and interaction pack | Content |
| Campfire | Benches, seated activity, audio, reactions, and group interaction | Content |
| Swing set | Ride state, camera motion, animation, and interaction | Content |
| Jetpack gym | Movement mode, course, timing, and leaderboard | Content |
| Quadcopter ride | Vehicle object, camera, route, boarding, and effects | Content |
| Hyperloop transport | Stations, routes, vehicle motion, access, and travel UI | Content |
| World tours | `TOUR_STEPS`, landmark sets, role-specific tours, and completion state | Content |
| Landmark pack | Creator-owned landmark definitions, models, labels, and navigation | Content |
| Avatar face pack | Procedural face variants, image assets, and organization avatar styles | Content |
| Agent avatar pack | Model/provider-specific bot faces, status animations, and badges | Content |
| Outfit pack | Outfit styles, color palettes, accessories, and organization uniforms | Content |
| Avatar animation pack | Idle, wave, sit, ride, work, celebrate, and accessibility-safe variants | Content |
| World audio pack | Ambient loops, interaction sounds, event cues, and volume metadata | Content |
| ForkMesh song/radio | Licensed audio, playlist, spatial source, and playback controls | Content |
| Emoji/status pack | Emoji categories, status presets, activity badges, and moderation | Content |
| Speech/voice pack | Dictation, speech bubbles, provider adapter, and accessibility controls | Socket |
| Screenshot/annotation tool | World capture, markup, privacy crop, and attachment flow | Now |
| Presence backend | Spaces, visitors, inactive presence, saved profiles, and expiry | Socket |
| Visitor analytics | Visit stats, unique-count sketch, referrers, and privacy-bounded reports | Now |
| Community moderation | Manual blocks, element moderation, feed moderation, and admin tools | Guarded |
| Device quality profiles | Adaptive rendering tiers, workload budgets, and creator test harness | Content |
| Renderer diagnostics | Frame/input readings, GPU probe, memory estimates, and debug panel | Now |
| Socket recovery toolkit | Reconnect timers, backoff policy, failure UI, and diagnostics | Now |
| Creator world SDK | Geometry/material helpers, interaction API, budgets, teardown, and test fixtures | Bundle |
| Office suite | Tower, meetings, attendance, tasks, notes, marketing, and access control | Bundle |
| Community venue suite | Events, workshops, QA, ads, build board, and link kiosk | Bundle |
| Avatar studio suite | Faces, outfits, animations, status, speech, and audio | Bundle |

## Mobile candidates

Mobile needs host sockets before most Marketplace packages can run, but its
existing screen/service split makes the intended boundaries clear.

| Candidate | Package-owned mobile surface | Readiness |
| --- | --- | --- |
| Owned Git engine | `native/ios/ForkMeshCore/Sources/FMGit` and `native/android/core/src/main/kotlin/com/forkmesh/nativeapp/git` as a separately versioned development service/library | Guarded |
| Clone-to-device | Clone UI, progress, cancellation, quarantine, and local catalog registration | Guarded |
| Local repository browser | Local repositories, files, refs, commits, and storage reporting | Socket |
| Mobile source control | Worktree edits, patches, commit, publish, and failure recovery | Guarded |
| Mobile pull-request flow | Patch builder, fork remote, publish, and pull submission | Guarded |
| Repository import sheet | Mirror/source selection, validation, progress, and retry | Socket |
| Mobile chat | Rooms, DMs, attachments, crypto, presence, and deep links | Socket |
| Mobile issue client | Issue list/create/detail, offline drafts, images, and signed events | Socket |
| Mobile organization client | Organizations, teams, tasks, members, and role-gated actions | Socket |
| Mobile agent console | Agent list/session, steering, transcript, completion, and diffs | Socket |
| Mobile agent provider | `AgentProvider`, credentials, tools, and provider-specific sessions | Socket |
| Push notifications | APNS/Android registration, notification routing, preferences, and badges | Socket |
| Secure-storage adapter | Platform keychain/keystore implementation behind a bounded secrets API | Guarded |
| Portable identity wallet | Identity creation/import/export, device linking, and signing prompts | Guarded |
| Mobile crypto provider | Owner crypto, room crypto, room seal, and key lifecycle | Guarded |
| Node sync | Event channel, node sync service/applier, reconnect, and conflict policy | Socket |
| Inbox service | Notification aggregation, unread state, subscriptions, and offline cache | Socket |
| Performance monitor | Frame/resource overlay, production sampling, and diagnostics export | Now |
| Legal-link provider | Instance-specific privacy, terms, security, and support links | Now |
| Mobile theme/icon pack | Theme, `fm_icons`, organization branding, accessibility variants | Content |
| Share extension | Share text/images/URLs into issue, chat, note, or agent composer | Socket |
| Document-provider integration | Export/import repository and note data through platform document APIs | Guarded |
| Mobile developer bundle | Git engine, clone, local browser, source control, publish, and pull flow | Bundle |

## Mirror server, CLI, MCP, and operations candidates

Most `tools/` programs already behave like one-purpose service plugins. Go
server modules can also become signed service packages if the daemon gains
neutral supervisor, event, transport, and repository-hook sockets.

| Candidate | Package-owned service/tool | Readiness |
| --- | --- | --- |
| Mirror daemon | Go mirror-node service, configuration, lifecycle, and health | Guarded |
| Repository sync engine | Catalog fetch, clone/fetch cycles, integrity checks, and retry policy | Guarded |
| Direct HTTPS gateway | Smart HTTP gateway, routing contract, refs proof, and TLS integration | Guarded |
| SSH Git gateway | SSH command authorization, repository mapping, and audit | Guarded |
| SSH authorization broker | Account/key capability verification and bounded command handoff | Guarded |
| Cloudflare Tunnel adapter | Tunnel install, configuration, health, and fallback endpoint publication | Guarded |
| Generic tunnel adapter | Provider-neutral tunnel contract plus alternative implementations | Socket |
| Mirror identity signer | Identity creation/loading, health signing, manifest signing, and rotation | Guarded |
| Mirror refresh service | Headless refresh, post-receive notification, locking, and recovery | Guarded |
| Hosted repository sync | Hosted repository catalog synchronization and integrity policy | Guarded |
| Dashboard replica | Static dashboard replica refresh and event invalidation | Now |
| Node event client | Authenticated event socket, ping, invalidation, and reconnect | Socket |
| Collaboration intake | Issues/pulls/discussions materialization and acknowledgements | Guarded |
| Issue reconciler | Deterministic issue divergence merge and collision renumbering | Guarded |
| Pull mutation executor | Merge/update execution, review gates, conflicts, and reward settlement | Guarded |
| Repository fork materializer | Borrowed objects, provenance, safety cycle, and catalog publication | Guarded |
| Service supervisor | Process specs, reload/restart/invalidate, backoff, and snapshots | Guarded |
| Git conformance runner | Compatibility scenarios, evidence, matrix generation, and reports | Now |
| Quality gate | Repository policy checks and a provider-neutral gate result | Now |
| Critical-test runner | Curated component test groups, timeouts, and machine-readable report | Now |
| Native security scanner | Native scan runner, lease, normalized findings, and upload | Guarded |
| Security scan orchestrator | OSV queries, source checks, evidence, and policy output | Guarded |
| MCP server | Capability-scoped code, notes, and chat toolsets | Guarded |
| MCP code toolset | Repository list/read/search tools as an independently grantable capability | Guarded |
| MCP notes toolset | Signed repository/personal note tools as a separate capability | Guarded |
| MCP chat toolset | Bounded room read/post tools as a separate capability | Guarded |
| Coding-agent launcher | Provider-neutral CLI agent launch, prompt handoff, and worktree policy | Guarded |
| Branch/PR review agent | Review automation, identity guard, evidence, and output schema | Guarded |
| GitHub issue sync | External issue synchronization, identity mapping, and schedule | Guarded |
| Reward scheduler | Ready-reward selection, external signer handoff, retry, and receipts | Guarded |
| Reward mainnet validator | Configuration and chain-read validation without custody | Guarded |
| Release publish tool | Signed release manifest, artifacts, upload, and rollback checks | Guarded |
| Appcast generator | Release metadata to update feed with signature and checksum | Now |
| Package builders | macOS, Windows, AppImage, installer, and release bundle adapters | Guarded |
| Cloudflare bootstrap | Worker/D1/KV/DO setup, secrets, routes, and verification | Guarded |
| Direct-HTTPS bootstrap | TLS/gateway provisioning and recovery checks | Guarded |
| Deployment target planner | Changed-file analysis and responsibility-scoped deploy plan | Now |
| Startup measurement | Cold/warm startup sampling and regression report | Now |
| Unit-economics report | Infrastructure/usage inputs to bounded cost and margin reports | Now |
| Commit-message rewriter | Guarded, reviewed history-rewrite workflow | Guarded |
| Collaboration verifier | Portable record/schema/signature verification | Now |
| Marketplace CLI | Validate, pack, install, uninstall, migrate, and catalog sync | Now |
| Marketplace socket inspector | Discover and explain contributions supported by a host | Now |
| World package generator | Manifest-driven World module staging and validation | Now |
| Desktop package generator | Manifest-driven native source/resource/test generation | Now |
| Mirror operator bundle | Daemon, sync, gateways, tunnel, identity, refresh, and diagnostics | Bundle |
| Maintainer toolkit | Quality gates, conformance, review, release, packaging, and deployment | Bundle |

## Web, docs, and community-content candidates

These are lower-privilege packages that can let creators change communication
surfaces without receiving repository or organization secrets.

| Candidate | Package-owned surface | Readiness |
| --- | --- | --- |
| Documentation theme | Docs shell, typography, navigation, search presentation, and code styles | Content |
| Documentation collection | Versioned authored docs mounted under a package namespace | Content |
| Blog theme | Blog index/post templates, social cards, and feed presentation | Content |
| Blog collection | Creator-authored posts, assets, authors, tags, and RSS entries | Content |
| Changelog page | Release feed, filtering, subscription links, and static rendering | Now |
| API explorer | Route catalog, request examples, live public calls, and response viewers | Now |
| Market storefront theme | Catalog cards, category navigation, creator pages, and package details | Content |
| Landing-page section | Bounded marketing section with assets and experiment metadata | Content |
| Press kit | Press page, logos, screenshots, facts, and downloadable assets | Content |
| Outreach toolkit | Share links, campaign parameters, attribution-safe analytics, and templates | Now |
| Social-sharing adapter | Per-network share URLs, preview rules, and reach reporting | Now |
| Analytics (first-party, shipped as the `analytics` package) | Consent, capture SDK, insights, replay, flags, surveys, and disable/teardown | Package |
| Privacy-friendly analytics provider | Aggregate page/referrer measurements with no cross-site identity | Socket |
| Auth-mesh widget | Cross-surface sign-in state and safe origin handoff | Guarded |
| Agent guestbook widget | Public visitor-agent interaction embedded on approved pages | Now |
| Security report widget | Encrypted submission, expectations, and status handoff | Guarded |
| Installer page | Platform detection, release verification instructions, and troubleshooting | Content |
| Career board | Static/remote openings, role pages, and application links | Content |
| Status widget | Current status summary embeddable in docs, App, desktop, and World | Now |
| Brand asset pack | Logos, icons, avatars, banners, presentation templates, and usage rules | Content |
| Launch campaign pack | Landing sections, press kit, outreach, social posts, and analytics dashboard | Bundle |

## Portable skills, shortcuts, policies, and templates

The checked-in `.forkmesh/skills` and `.forkmesh/shortcuts` are natural package
content. They should remain reviewable text with declared capabilities; a skill
must never silently acquire more authority than the installing user or agent.

| Candidate | Package content | Readiness |
| --- | --- | --- |
| CFO payout operations | Existing guarded payout skill, workflow references, and audit checklist | Content |
| Discord quality intake | Existing bug-intake skill, dedupe policy, reporter credit, and result schema | Content |
| Code-review skill pack | Review shortcut, security checks, severity rubric, and report template | Content |
| Bug-triage skill pack | Bug-report checks, reproduction template, dedupe, and issue creation | Content |
| UI-stall remediation | Stall evidence collection, Qt diagnostics, profiling, and safe fixes | Content |
| Qt performance pack | Performance budgets, measurement shortcut, and regression checklist | Content |
| Documentation maintainer | Docs refresh, link validation, changelog/blog generation, and ownership map | Content |
| Release-readiness pack | Critical tests, packaging, app-store checklist, signatures, and rollout | Content |
| Mobile release pack | Existing mobile advance shortcut plus Android/iOS validation workflows | Content |
| Security response pack | Report triage, disclosure workflow, scan evidence, and escalation policy | Content |
| Feature-removal pack | Usage evidence, dependency audit, migration, deprecation, and cleanup checklist | Content |
| Repository cleanup pack | Redundant code, stale comments, temporary files, and verification shortcuts | Content |
| YC/news launch pack | Existing launch-readiness shortcut, evidence checklist, and campaign templates | Content |
| Contributor reward pack | Reward validation, reconciliation, public receipt, and non-custody policy | Content |
| Mirror operator skill pack | Provision, health check, failover proof, recovery, and incident templates | Content |
| Project template pack | Portable issue, pull, discussion, project, release, and workflow templates | Content |
| Organization operating pack | Roles, team structure, recurring tasks, calendars, notes, and dashboards | Content |
| Agent persona pack | Teammate definition, model routing, skills, schedule, and collaboration policy | Content |

## Host sockets needed to unlock the inventory

Creating these neutral sockets is higher leverage than adding more
`builtin:<feature>` branches. Socket names are proposals, not schema commitments.

| Proposed socket | Capability exposed by the host | Unlocks |
| --- | --- | --- |
| `desktop.page` | Lifecycle-owned native page with declarative navigation | Most desktop feature pages |
| `desktop.repo-panel` | Bounded repository context and embedded panel | Stats, security, pulls, project and release panels |
| `desktop.repo-action` | Explicit user-triggered repository action with scoped inputs | Import, migration, sync, review, publish |
| `desktop.log-source` | Register category, poll/push records, filter page, teardown | Sentry-style observability providers |
| `desktop.chat-provider` | Channels, messages, sends, presence, credentials, teardown | Discord, Slack, Matrix, custom chat |
| `desktop.agent-provider` | Models, session start/resume/steer, usage, transcript events | Codex, Claude, Ollama, remote agents |
| `desktop.scm-provider` | Read status/diffs and execute approved SCM operations | Git workflows and future SCM backends |
| `desktop.composer-tool` | Add bounded attachments or transformations after user action | Capture, OCR, voice, templates |
| `desktop.status-widget` | Small status/health/usage surface | Mirrors, deployments, agents, budgets |
| `desktop.control-panel` | Administrator-gated service configuration and actions | Node, mirror, tunnel, server packages |
| `dashboard.repo-panel` | Repository-scoped web panel and safe API | Security, analytics, releases, imports |
| `dashboard.profile-panel` | Account/profile-scoped web panel | Badges, contributions, memberships |
| `worker.cron` | Named scheduled callback with time/resource budget | Digests, reminders, monitors, sync jobs |
| `worker.event` | Typed internal event subscription | Notifications, badges, analytics, bots |
| `worker.webhook` | Verified provider webhook with package-specific secret handle | Payments, chat, email, CI providers |
| `worker.queue` | Bounded asynchronous job consumer with retries and dead-letter state | Imports, migrations, scans, agent jobs |
| `worker.websocket` | Scoped realtime endpoint without arbitrary namespace access | Chat providers, meetings, live status |
| `worker.durable-object` | Explicit class/binding allocation and migration lifecycle | Rooms, sessions, rate limits, coordination |
| `worker.outbound` | Existing exact-origin outbound adapter made consistently available | Provider connectors and public feeds |
| `worker.notification` | Provider-neutral delivery request | APNS, web push, email, chat notifications |
| `worker.payment-provider` | Read/verify/entitlement contract without private-key custody | Patreon, chain indexers |
| `worker.audit-sink` | Redacted typed audit events | SIEM and compliance packages |
| `world.object` | Bounded scene object with placement and teardown | Existing elements and virtual goods |
| `world.panel` | Safe 2D/3D panel with data access and click handling | Chat, Discord, notes, status boards |
| `world.activity` | Enter/exit/update lifecycle for rides, games, meetings, workshops | Campfire, gym, swings, events |
| `world.building` | Bounded geometry, surfaces, portals, floors, and access rules | Offices, venues, repository districts |
| `world.avatar-part` | Faces, clothing, accessories, material and compatibility metadata | Avatar creator economy |
| `world.animation` | Named clips, state transitions, accessibility metadata | Avatar and object animations |
| `world.audio` | Licensed audio assets and bounded spatial playback | Ambience, radio, sound packs |
| `world.data-source` | Privacy-filtered snapshot subscription | Feeds, mirror state, repo graphs, events |
| `world.admin-tool` | Role-gated diagnostics/moderation action | Infrastructure and moderation consoles |
| `mobile.tab` | Lifecycle-owned tab/screen with route/deep-link contract | Chat, agents, orgs, local repos |
| `mobile.repo-action` | Explicit repository operation in an isolate with cancellation | Clone, commit, publish, PR |
| `mobile.share-target` | Approved external share payload to a selected composer | Issues, notes, chat, agents |
| `mobile.agent-provider` | Provider-neutral mobile session contract | Local and hosted agent adapters |
| `mirror.sync-adapter` | Fetch/catalog source and integrity proof contract | Alternative repository sources |
| `mirror.transport` | Direct endpoint lifecycle and health contract | HTTPS, SSH, tunnel transports |
| `mirror.repository-hook` | Bounded push/fetch/collaboration event | Refresh, indexing, notifications |
| `cli.command` | Namespaced subcommand with explicit files/network permissions | Maintainer and operator tools |
| `mcp.toolset` | Independently grantable MCP tools and resources | Code, notes, chat, issue, release tools |
| `release.publisher` | Artifact read, sign request, upload, and publication receipt | Hosting/provider release adapters |
| `content.skill` | Reviewable agent instructions plus required capability declarations | Skill packs and teammate behaviors |
| `content.shortcut` | Parameterized prompt/workflow/command with safe host substitution | Maintainer shortcuts |
| `content.template` | Repository/org/project templates with preview and merge policy | Project and organization packs |

## Boundaries that should stay in the trusted host

Being generous about candidates does not mean turning the application into an
unbounded native-code loader. The following enforcement points should remain
host-owned even when a package supplies UI, policy, or provider behavior:

- Manifest, signature, digest, provenance, permission, and dependency
  validation.
- Install/uninstall transactions, quarantine, migration ledger, export, purge,
  and rollback.
- Secret storage and opaque credential handles; packages should never enumerate
  unrelated secrets.
- Account authentication, session validation, CSRF/CORS/origin enforcement,
  role checks, and audit-record integrity.
- Identity private keys, owner encryption keys, wallet private keys, and the
  final user-presence/arming step for signatures or payments.
- Git path validation, object-integrity checks, repository locks, worktree
  quarantine, command allowlists, and process sandbox enforcement.
- Route prefix allocation, exact outbound-origin enforcement, request/body
  limits, rate limits, and Worker/DO resource budgets.
- Native widget ownership, destruction, callback revocation, thread/process
  cancellation, and crash containment.
- World triangle/draw-call budgets, asset limits, collision bounds, interaction
  ownership, accessibility rules, and complete scene teardown.
- Mobile isolate/resource limits, secure storage, platform permission prompts,
  background-execution policy, and app-store disclosure enforcement.

## Suggested extraction order

1. Finish converting existing `builtin:*` packages to neutral contribution
   sockets and use them as contract tests.
2. Add `desktop.page`, `desktop.repo-panel`, `desktop.repo-action`,
   `desktop.agent-provider`, and `desktop.chat-provider`; then extract low-risk
   pages before runners, signers, or control planes.
3. Add Worker cron, event, webhook, queue, and notification sockets; extract
   provider adapters such as Patreon, APNS, Discord, and analytics.
4. Generalize the existing World element loader into object, panel, activity,
   building, avatar, animation, and audio sockets; move the three checked-in
   elements first.
5. Add mobile tab/repository-action/provider sockets only after lifecycle,
   app-store disclosure, secure-storage, and background-task policies are
   enforceable by the host.
6. Package standalone tools and portable skills early, because their inputs,
   outputs, and permissions are easier to review than arbitrary in-process
   native extensions.
7. Leave identity, authentication, signature arming, migration enforcement,
   sandboxing, route allocation, and integrity checks in the host permanently.
