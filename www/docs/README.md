# ForkMesh documentation

> **Serving stack note.** Since 2026-09-24 the App runs on three
> self-hosted core nodes instead of Cloudflare Workers, D1 and Durable Objects
> (see `docs/README.md` in the repository for the developer docs). Pages that
> describe the earlier Cloudflare deployment and do **not** apply as written:
>
> - Cloudflare Worker deployment, limits and bootstrap:
>   [operations/worker-split.md](operations/worker-split.md),
>   [operations/cloudflare-free-tier-audit.md](operations/cloudflare-free-tier-audit.md),
>   [operations/automation-and-cloudflare-bootstrap.md](operations/automation-and-cloudflare-bootstrap.md),
>   [cloudflare-workers-ai-coding-agent.md](cloudflare-workers-ai-coding-agent.md).
> - The production D1 database: [operations/d1-database-cleanup-plan.md](operations/d1-database-cleanup-plan.md)
>   (the database is now rqlite; see `docs/REKEY.md`).
> - The production desktop control node and its `wrangler tail` streams:
>   [operations/desktop-control-node.md](operations/desktop-control-node.md).
> - The production mirror fleet (`mirrorN.forkmesh.com`, Cloudflare Tunnel):
>   [operations/mirror-node.md](operations/mirror-node.md),
>   [operations/direct-https-mirror-gateway.md](operations/direct-https-mirror-gateway.md),
>   [operations/git-ssh-gateway.md](operations/git-ssh-gateway.md). The
>   core nodes and runners are in `docs/deploy/README.md` and `docs/RUNNERS.md`.
> - [external-dependency-map.md](external-dependency-map.md) lists
>   the Cloudflare-era outside services; the core nodes drop most of them.
> - Legacy custody and historical plans: [operations/legacy-solana-custody-migration.md](operations/legacy-solana-custody-migration.md),
>   `superpowers/`.
>
> Protocol, security and product pages apply to both.

Start with the guide that matches what you are trying to do:

| Goal | Guide |
| --- | --- |
| Install ForkMesh and complete the first-run setup | [Getting started](getting-started.md) |
| Check supported platforms, signing, upgrade, rollback, and uninstall | [Installation support matrix](install-support-matrix.md) |
| Start your own instance in one click from a settings file | [Start from a settings file](start-config.md) |
| Prove and measure the canonical product promise | [Product promise](product-promise.md) |
| Decide whether ForkMesh fits compared with centralized hosting, self-hosted forges, decentralized Git tools, backups, and basic Git servers | [Category comparisons](category-comparisons.md) |
| Use familiar developer terms and check copy for jargon | [Terminology and voice guide](terminology.md) |
| Understand free protocol rights, hosted limits, and paid-service gates | [Business model](business-model.md) |
| Confirm tokens, rewards, and payments stay optional | [Incentive independence](incentive-independence.md) |
| Run the weekly activation-to-revenue KPI review | [Activation-to-revenue scorecard](activation-revenue-scorecard.md) |
| Run the Never Lose Your Code maintainer challenge: rules, bounty verification, failover video, and kickoff posts | [Never Lose Your Code Challenge](launch/never-lose-your-code-challenge.md) |
| Record the monthly community, product, and revenue outcome review | [Monthly outcome review](outcome-review.md) |
| Check stock Git operations, clients, and known limitations | [Git compatibility matrix](git-compatibility.md) |
| Understand a failed clone, pull, or push, such as `Error in the HTTP2 framing layer`, and what to send when reporting it | [Git errors](git-errors.md) |
| Depend on a repository from Git, pip, npm, Go, or Cargo so builds survive a provider change or a lost origin | [Stable repository identity and dependency workflows](dependency-workflows.md) |
| See which features are planned to move into optional Marketplace plugins, from the stingy shortlist to the full opportunity inventory | [Planned plugins](plugins/README.md): [shortlist](plugins/shortlist.md) and [opportunity inventory](plugins/opportunities.md) |
| Build migration, automation, discovery, deployment, analytics, or moderation tools against versioned HTTP, event, CLI, and webhook contracts | [Integration contract](integrations.md) |
| Search the public repository index and read its ranking and moderation rules | [Searchable repository index](repository-index.md) |
| Read a repository's synchronization status the same way on every client | [Synchronization status](sync-status.md) |
| Check whether a repository is actually preserved, how independently, and what is at risk | [Availability proof and repository risk](availability-proof.md) |
| See what the hosted relay stores, how long each store and backup keeps it, and how it is deleted | [Relay data retention and deletion](relay-data-retention.md) |
| Preserve a project fast during a shutdown, takedown, lost account, censorship, or provider end-of-life | [Preservation-first crisis onboarding](crisis-preservation.md) |
| Keep a repository and its issues and pull requests moving over dropped links, data budgets, and USB sticks, with signed exports and resumable fetches | [Offline and low-bandwidth sync](offline-sync.md) |
| Reach repositories over clearnet, Tor or I2P, an overlay network, or only the local network, with failover and tested protection against DNS, address, log, and telemetry leaks | [Transports and anonymity postures](transports.md) |
| Prepare for and recover from origin, service, DNS, key, regional, and backup loss with a printable runbook, a signed offline bootstrap kit, key rotation by recovery custodians, and live drills | [Disaster recovery](disaster-recovery/README.md) and its [printable runbook](disaster-recovery/runbook.md) |
| Run the GitHub outage campaign: 60-second video script, post thread, and $5 mirror bounty terms | [GitHub outage survival kit](launch/github-outage-survival-kit.md) |
| Write, verify, and sign a repository preservation contract backed by demonstrated mirror capacity | [Repository availability contracts](availability-contract.md) |
| Build, run, and use the desktop application | [Qt client guide](qt-client.md) |
| Change the project and open a pull request | [Contributing](../../CONTRIBUTING.md) |
| Understand what “done” requires | [Definition of done](engineering/definition-of-done.md) |
| Implement or verify signed collaboration objects | [Conflict-safe collaboration protocol](design/conflict-safe-collaboration.md) |
| Understand how pull request review threads stay on the right lines, or say they are outdated, after a re-push | [Review threads across patchsets](design/pull-review-threads.md) |
| Implement ForkMesh independently: normative encodings, signatures, sync, versioning, vectors, and threat model | [Protocol suite](protocol/README.md) |
| Prove an independent client, relay, index, or provider is compatible and earn an expiring certification badge | [Protocol certification program](protocol/certification.md) |
| Propose a protocol change, appeal a decision, join as an independent implementer or operator, and read decisions, dissent, and funding disclosures | [Protocol and ecosystem governance](governance/README.md) and its [decision register](governance/decision-register.md) |
| Apply for a grant or matching funds for a public repository, volunteer mirror, accessibility, documentation, education, or tooling work, and read funders, selections, payments, and outcome reports | [Public-interest funding](funding/README.md) and its [funding register](funding/funding-register.md) |
| Read and clone public projects without an account, and see where sign-in is required | [Reading and cloning without an account](accountless-reading.md) |
| Recover an identity or move it between clients | [Portable identities and recovery](portable-identities.md) |
| Follow scoped trust lists from maintainers, reviewers, security researchers, and moderation groups, and recover when one is compromised | [Trust lists](trust-lists.md) |
| Carry stars, follows, usage endorsements, security reviews, and mirror commitments between services as signed records; revoke them; see and turn off recommendations | [Portable social signals](social-signals.md) |
| Start an empty repository from the web and push to it | [Hosted repositories](hosted-repositories.md) |
| Import repositories from another forge | [Repository imports](repository-imports.md) |
| Keep a repository in sync with GitHub, including issues, discussions, and pull requests | [Connecting to GitHub](github-sync.md) |
| Keep issues and comments in sync both ways with GitHub or a self-hosted Gitea or Forgejo server | [Two-way forge sync](forge-sync.md) |
| Understand public and private chat encryption | [Chat encryption and integrations](security/sealed-chat.md) |
| Audit private repositories, backups, and key recovery | [Private repository confidentiality](security/private-repository-confidentiality.md) |
| Check what each organization role may do and how the matrix denies by default | [Organization roles and the authorization matrix](security/organization-roles.md) |
| See every outside service ForkMesh contacts, what it observes, and what works when it is gone | [External dependency and observability map](external-dependency-map.md) |
| Review every third-party dependency, its publisher, freshness and security score | [Dependency security audit](security/dependency-security-audit.md) |
| Block or warn on secrets before they replicate, and respond when one already has | [Secret scanning before replication](security/secret-scanning.md) |
| Operate a desktop control node | [Desktop control node](operations/desktop-control-node.md) |
| Back up, restore, drill, or move a node's key | [Node backup and recovery](operations/node-backup-and-recovery.md) |
| Self-host each network service role, check it with its contract, and see what still needs the hosted deployment | [Self-hostable service roles](operations/service-roles.md) |
| Operate and interpret stock Git conformance | [Git conformance operations](operations/git-conformance.md) |
| Require signed status checks before a pull request merges on a mirror | [Signed required checks](operations/required-checks.md) |
| Set rules every pull request must meet (tests, docs, size, secrets, a required AI model) | [Pull request rules](operations/pull-rules.md) |
| Operate the organization agent offline queue | [Organization agent offline queue](operations/organization-agent-offline-queue.md) |
| Configure and prove mobile push notifications | [Mobile push notifications](operations/mobile-push-notifications.md) |
| Place, review, or operate first-party text ads: policy, membership allowances, review states, and measurement | [ForkMesh Ads](operations/ad-network.md) |
| Work through the D1 database cleanup: dead tables, hot reads, retention, and key rotation, with a ready prompt per step | [D1 database cleanup plan](operations/d1-database-cleanup-plan.md) |
| Read the founders' product dashboard and its derivation rules | [Admin dashboard](admin-dashboard.md) |
| Run a coding agent on Cloudflare Workers AI | [Cloudflare Workers AI coding agent](cloudflare-workers-ai-coding-agent.md) |
| See which features each surface (Worker, web, World, desktop, mobile) offers, and which secrets never leave the device | [Surface capability matrix](capability-matrix.md) |
| Understand how releases are signed and why binaries never enter Git | [Release binary publishing](design/release-binary-publishing.md) |
| See what the iPhone app reads from and writes to Apple Health through HealthKit, tool by tool, and how to turn it off | [Apple Health on iPhone](mobile/apple-health.md) |
| Use ForkMesh from the phone: agent sessions, Siri, file editing, the node contract, and the owned Git engine | [Mobile docs](mobile/mobile-node-contract.md): [agent access](mobile/mobile-agent-access.md), [Siri on iPhone](mobile/siri.md), [file editing](mobile/mobile-file-editing.md), [feature-parity audit](mobile/feature-parity-audit.md), [Git engine decision](mobile/git-engine-decision.md), [engine spec](mobile/owned-git-engine-spec.md), [compatibility matrix](mobile/owned-git-compatibility-matrix.md), [threat model](mobile/owned-git-threat-model.md) |
| Follow ForkMesh from the fediverse and Lemmy | [Fediverse digests](fediverse-digests.md), [mention review](fediverse-mention-review.md), [Lemmy threaded collaboration](lemmy-threaded-collaboration.md) |
| Run World community services, and check World's visual acceptance | [Community services](world-community-services.md), [visual acceptance](world-visual-acceptance.md), [soundtrack licence](world-soundtrack-license.md) |
| Review community-placed contextual ads | [Community ad governance](community-ad-governance.md) |
| Run or join a hackathon | [Hackathon](hackathon.md) |
| Read the managed-services pricing interviews | [Pricing interview register](research/pricing-interviews.md) |
| Waive a quality gate, with the record it needs | [Quality-gate waivers](engineering/quality-gate-waivers/README.md) |
| Find developer documentation for one subsystem (desktop, server, app, www, world, mobile, native, marketplace, release, security) | [Developer docs index](../../docs/README.md) in the repository |

The hosted documentation at [forkmesh.com/docs](https://forkmesh.com/docs)
contains the complete protocol reference, release notes, and build notes for
every client. Files in this directory cover repository-specific development,
security, design, and operations; every Markdown file and JSON schema here is
also rendered on the website at
[forkmesh.com/docs/library](https://forkmesh.com/docs/library/) by
`app/tools/docs_site.py` during the `www` deploy stage, with relative links,
heading anchors, and schema references resolved. The same build reads the Git
history of this directory to date every page, list recent changes at
[forkmesh.com/docs/history](https://forkmesh.com/docs/history/), and publish a
sortable index of every document at
[forkmesh.com/docs/documents](https://forkmesh.com/docs/documents/).

## Reference by area

- `design/` — protocol and federation design decisions.
- `protocol/` — the versioned normative protocol suite, bound to `conformance/protocol/v1` vectors.
- `governance/` — the governance charter and the generated decision register, bound to `conformance/governance/v1` records.
- `funding/` — the public-interest funding program guide and the generated funding register, bound to `conformance/funding/v1` rounds.
- `social-signals.md` — signed, revocable, exportable social signals and explainable recommendations, bound to `conformance/social-signals/v1` vectors.
- `engineering/` — quality gates, performance budgets, and completion policy.
- `git-compatibility.md` — generated stock-Git support and limitation matrix.
- `category-comparisons.md` — evidence-backed fit and not-for-you comparison, generated from `conformance/category-comparisons/v1/ledger.json`.
- `dependency-workflows.md` — provider-independent repository identity, pin and fallback rules, and tested dependency-manager recipes.
- `integrations.md` — versioned HTTP, event-stream, CLI, and webhook contract.
- `trust-lists.md` — scoped, signed trust lists; follows, overrides, appeals, and the compromised-list drill. Protocol in `design/scoped-trust-lists.md`.
- `sync-status.md` — shared synchronization state model, fixture, and client parity.
- `forge-sync.md` — two-way issue sync with external forges: provider contract, conflicts, and controls.
- `availability-proof.md` — availability proof, trusted quorum, and risk score model.
- `crisis-preservation.md` — crisis scenarios, time- and access-bound preservation plans, evidence-only coverage, and drills.
- `offline-sync.md` — signed removable-media exports, resumable bounded fetches, limited-replica advertisements, reconciliation, and rollback.
- `transports.md` — transport adapters, anonymity postures, leak tracing, failover, health states, and address advertisement, bound to `conformance/transports/v1` cases.
- `availability-contract.md` — availability contract terms, SLO misses, remedies, demonstrated capacity, and failure simulation.
- `plugins/` — planned Marketplace plugins: the stingy shortlist and the generous opportunity inventory from the #677 audit.
- `launch/` — launch readiness evidence and campaign kits.
- `operations/` — deployment, recovery, gateways, headless nodes, and service
  runbooks.
- `operations/service-roles.md` — self-hostable role manifest, substitutes, and probe verdicts.
- `security/` — security boundaries and local integration contracts.
- `strategy/` — phased product integration plans and machine-readable scope ledgers.
- `*.schema.json` — machine-readable configuration and protocol schemas.

If the application is not behaving as expected, start with
[Desktop app diagnostics](qt-client.md#logs-and-diagnostics). Include the version,
the action you took, and the relevant log excerpt when opening an issue.
