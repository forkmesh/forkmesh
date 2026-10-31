# ForkMesh documentation

ForkMesh keeps the source code and releases of a project reachable and
verifiable when a hosting provider, domain or network fails it. Git stays the
source of truth; signed repositories replicate to independent mirrors, and the
desktop, web and phone apps work on them through one protocol.

This folder is the one source of truth for every ForkMesh document. It is
published at [forkmesh.com/docs](https://forkmesh.com/docs/library/), in the
desktop app's Docs tab and in the phone apps, with
[page history](https://forkmesh.com/docs/history/) and a
[document index](https://forkmesh.com/docs/documents/).

## Core documentation

Start here. Each page is short and links to the detail.

| | |
| --- | --- |
| [Architecture and availability](architecture.md) | How the network is built, and why no single server has to stay up |
| [Encryption and signatures](encryption.md) | What is signed and encrypted, with which algorithms, and the known gaps |
| [Mirroring and software distribution](mirroring.md) | How copies stay in step, how clones are verified, how releases reach users |
| [Self-hosting](self-hosting.md) | Run a node, a mirror, an instance or a fleet yourself |
| [Censorship resilience](censorship-resilience.md) | What still works when a host, domain, account or network is blocked |
| [Threat model](protocol/threat-model.md) | What the protocol defends against, and what is still open |

## Use ForkMesh

**Get started.** [Getting started](getting-started.md) ·
[ForkMesh apps](apps.md) · [Desktop app guide](qt-client.md) ·
[Installation support matrix](install-support-matrix.md) ·
[Back up a GitHub repository](back-up-a-github-repository.md) ·
[Working with coding agents](coding-agents.md) ·
[Git errors](git-errors.md)

**Repositories.** [Reading and cloning without an account](accountless-reading.md) ·
[Hosted repositories](hosted-repositories.md) ·
[Contribute with git only](contributing-with-git.md) ·
[Repository imports](repository-imports.md) ·
[Connecting to GitHub](github-sync.md) ·
[Two-way forge sync](forge-sync.md) ·
[Folder About boxes](folder-about.md) ·
[Subscribing by feed or email](feeds-and-email.md) ·
[Mirror and deploy a Cloudflare Worker](cloudflare-workers.md) ·
[Workers AI coding agent](cloudflare-workers-ai-coding-agent.md)

**Availability.** [Synchronization status](sync-status.md) ·
[Mirror goals](mirror-quota.md) ·
[Availability proof](availability-proof.md) ·
[Repository availability contracts](availability-contract.md) ·
[Crisis preservation](crisis-preservation.md) ·
[Offline and low-bandwidth sync](offline-sync.md) ·
[Transports and anonymity postures](transports.md) ·
[Dependency workflows](dependency-workflows.md)

**Chat and calls.** [Voice and video calls](calls.md) ·
[Safety numbers and trusted devices](chat-safety-numbers.md) ·
[Mentions and username suggestions](mentions-and-autocomplete.md) ·
[DJ Studio jams](dj-studio.md)

**Identity, trust and social.** [Portable identities and recovery](portable-identities.md) ·
[Trust lists](trust-lists.md) · [Portable social signals](social-signals.md) ·
[Fediverse digests](fediverse-digests.md) and [mention review](fediverse-mention-review.md) ·
[Lemmy threaded collaboration](lemmy-threaded-collaboration.md)

**World.** [Community services](world-community-services.md) ·
[Soundtrack licence](world-soundtrack-license.md) ·
[Community ad governance](community-ad-governance.md)

**Phones.** [Mobile node contract](mobile/mobile-node-contract.md) ·
[Owned Git engine](mobile/owned-git-engine-spec.md) · [Siri](mobile/siri.md) ·
[Apple Health](mobile/apple-health.md) · [File editing](mobile/mobile-file-editing.md) ·
[Agent access](mobile/mobile-agent-access.md)

## Run ForkMesh

| Area | Start with |
| --- | --- |
| Your own node, mirror or instance | [Self-hosting](self-hosting.md), [Start your own instance](start-your-own-instance.md), [Start from a settings file](start-config.md), [mirror node](operations/mirror-node.md) |
| The served stack and its runbook | [Operations](operations/README.md): [served stack](operations/served-stack.md), [vault](operations/vault.md), [runners](operations/runners.md), [instances](operations/instances.md) |
| Recovery | [Disaster recovery](disaster-recovery/README.md) and its [printable runbook](disaster-recovery/runbook.md) |
| Security | [Security](security/README.md): trust boundaries, keys, [private repositories](security/private-repository-confidentiality.md), [chat encryption](security/sealed-chat.md), [organization roles](security/organization-roles.md), [secret scanning](security/secret-scanning.md) |
| What outside services see, and what the service keeps | [External dependency map](external-dependency-map.md), [Relay data retention and deletion](relay-data-retention.md) |
| Releases and installers | [Releases](release/README.md) |

## Reference

[Protocol suite](protocol/README.md) ·
design decisions on [conflict-safe collaboration](design/conflict-safe-collaboration.md), [ActivityPub federation](design/activitypub-federation.md), [pull review threads](design/pull-review-threads.md), [release binary publishing](design/release-binary-publishing.md) and [scoped trust lists](design/scoped-trust-lists.md) ·
[Integration contract](integrations.md) ·
[Git compatibility matrix](git-compatibility.md) ·
[Surface capability matrix](capability-matrix.md) ·
[Searchable repository index](repository-index.md) ·
[Terminology and voice guide](terminology.md) ·
[Conformance suites](conformance/README.md) ·
machine-readable `*.schema.json` files in this folder.

## Product and project

[Product promise](product-promise.md) ·
[Category comparisons](category-comparisons.md) ·
[Business model](business-model.md) ·
[Incentive independence](incentive-independence.md) ·
[Activation-to-revenue scorecard](activation-revenue-scorecard.md) ·
[Monthly outcome review](outcome-review.md) ·
[Admin dashboard](admin-dashboard.md) ·
[Hackathon](hackathon.md) ·
[Governance](governance/README.md) and its [decision register](governance/decision-register.md) ·
[Public-interest funding](funding/README.md) and its [funding register](funding/funding-register.md) ·
[Definition of done](engineering/definition-of-done.md) ·
[Contributing](../CONTRIBUTING.md)

## Build ForkMesh

[How the codebase fits together](codebase.md) is the developer entry page.
Each part has one home page; its topic pages link back to it.

| Part | Code | Home page |
| --- | --- | --- |
| Desktop app (Qt) | `desktop/` | [desktop/README.md](desktop/README.md) |
| Agent runs | `desktop/src/*Agent*`, `server/`, `app/src` | [agents/README.md](agents/README.md) |
| App (Python on the core nodes) | `app/` | [app/README.md](app/README.md) |
| CPython host for the app | `app/host/` | [host/README.md](host/README.md) |
| Go daemon (mirror, core, runner roles), calls | `server/` | [server/README.md](server/README.md) |
| Website and docs site | `www/`, `app/tools/docs_site.py` | [www/README.md](www/README.md) |
| Phone apps (iOS, watchOS, Android) | `native/` | [native/README.md](native/README.md) |
| Marketplace packages and plugins | `marketplace/` | [marketplace/README.md](marketplace/README.md) |
| World (3D) and Office (2D) | `world/`, `office/` | [world/README.md](world/README.md), [office/README.md](office/README.md) |
| Fleet, deploys and operator tools | `deploy/`, `tools/next_*.py` | [operations/README.md](operations/README.md) |
| Releases and installers | `tools/package/`, `.forkmesh/releases/` | [release/README.md](release/README.md) |
| Repository tools | `tools/` | [tools/README.md](tools/README.md) |
| Engineering rules | cross-cutting | [destructive actions](engineering/destructive-actions.md), [GitHub Actions](engineering/github-actions.md), [third-party dependencies](engineering/third-party-dependencies.md), [changelog](engineering/changelog.md), [daily interface validation](engineering/daily-interface-validation.md) |

Special agents are workflows in `.forkmesh/workflows/`
([app/workflows.md](app/workflows.md#desktop-workflows-and-special-agents)):
[CFO](finance/cfo.md), [Hearth](community/hearth.md),
[Reddit scout](community/reddit.md), [site crawler](www/site-crawler.md) and
[Sentinel](security/sentinel.md). The
guide for outside coding agents is `app/public/forkmesh-agent.md`, served at
`https://app.forkmesh.com/forkmesh-agent.md`.

## Writing docs

- **One home per topic.** Before adding a page, find the page that already
  covers the topic and extend it; link to it from elsewhere instead of
  repeating it. Summaries belong on the core pages above, detail on the area
  page.
- **Where a page goes.** User guides sit at the top of this folder (their
  addresses on forkmesh.com/docs stay fixed); everything else goes in the
  area folder of the code it describes. Source files hold no comments, so the
  reason behind a change goes on the page for that code (`AGENTS.md`).
- **Publishing.** `app/tools/docs_site.py` renders every page here except the
  internal `finance/`, `marketing/` and `community/` folders; the phone apps
  carry the user-facing areas. Code and tests read many pages by path: move or
  rename a page only together with every reference, and add a redirect in
  `www/public/_redirects` for a public address that goes away.
- **Style.** Short sentences, present tense, facts over stories. Say what still
  depends on one provider; never claim more than the design delivers.
