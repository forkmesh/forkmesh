# ForkMesh documentation

ForkMesh is a decentralized Git hosting network: signed repositories replicate
across independent mirror nodes, and the desktop, web and mobile clients read
and write them through a shared protocol. Every Markdown file and JSON schema in
this directory is rendered at [forkmesh.com/docs/library](https://forkmesh.com/docs/library/)
by `app/tools/docs_site.py`, which also builds the page history at
[forkmesh.com/docs/history](https://forkmesh.com/docs/history/) and the
document index at [forkmesh.com/docs/documents](https://forkmesh.com/docs/documents/).
Developer documentation per subsystem is in the [developer docs index](../../docs/README.md).

## Getting started

| Goal | Guide |
| --- | --- |
| Install ForkMesh and complete first-run setup | [Getting started](getting-started.md) |
| Supported platforms, signing, upgrade, rollback, uninstall | [Installation support matrix](install-support-matrix.md) |
| Start an instance from a settings file | [Start from a settings file](start-config.md) |
| Build, run and use the desktop application | [ForkMesh desktop app guide](qt-client.md) |
| Read and clone public projects without an account | [Reading and cloning without an account](accountless-reading.md) |
| Follow issues by Atom feed or email, or read them without JavaScript | [Subscribing by feed or email](feeds-and-email.md) |
| Create a repository from the web and push to it | [Hosted repositories](hosted-repositories.md) |
| Fork a repository, push a branch and open a pull request with git only | [Contribute with git only](contributing-with-git.md) |
| Import repositories from another forge | [Repository imports](repository-imports.md) |
| Diagnose a failed clone, pull or push | [Git errors](git-errors.md) |
| Change the project and open a pull request | [Contributing](../../CONTRIBUTING.md) |

## Product and policy

| Topic | Page |
| --- | --- |
| The product promise and how it is measured | [Product promise](product-promise.md) |
| Fit compared with other hosting categories | [Category comparisons](category-comparisons.md) |
| Terminology and voice | [Terminology and voice guide](terminology.md) |
| Free protocol rights and paid services | [Business model](business-model.md) |
| Tokens, rewards and payments stay optional | [Incentive independence](incentive-independence.md) |
| Weekly activation-to-revenue metrics | [Activation-to-revenue scorecard](activation-revenue-scorecard.md) |
| Monthly outcome review | [Monthly outcome review](outcome-review.md) |
| Product analytics dashboard | [Admin dashboard](admin-dashboard.md) |
| Features per surface and device-only secrets | [Surface capability matrix](capability-matrix.md) |
| Hackathon campaigns | [Hackathon](hackathon.md) |
| Protocol and ecosystem governance | [Governance](governance/README.md) and its [decision register](governance/decision-register.md) |
| Public-interest grants and matching funds | [Public-interest funding](funding/README.md) and its [funding register](funding/funding-register.md) |

## Repositories, sync and availability

| Topic | Page |
| --- | --- |
| Stock Git operations and known limits | [Git compatibility matrix](git-compatibility.md) |
| Provider-independent dependencies for Git, pip, npm, Go and Cargo | [Dependency workflows](dependency-workflows.md) |
| Versioned HTTP, event, CLI and webhook contract | [Integration contract](integrations.md) |
| An About box and logo for a folder (monorepo projects) | [Folder About boxes](folder-about.md) |
| Public repository search, ranking and moderation | [Searchable repository index](repository-index.md) |
| One synchronization status model for every client | [Synchronization status](sync-status.md) |
| Availability proof and repository risk | [Availability proof](availability-proof.md) |
| Signed availability contracts backed by mirror capacity | [Repository availability contracts](availability-contract.md) |
| Fast preservation during shutdowns and takedowns | [Crisis preservation](crisis-preservation.md) |
| A goal of healthy mirrors and the copies people run | [Mirror goals](mirror-quota.md) |
| Removable-media exports and resumable low-bandwidth fetches | [Offline and low-bandwidth sync](offline-sync.md) |
| Clearnet, Tor, I2P, overlay and LAN transports | [Transports and anonymity postures](transports.md) |
| Recovery from origin, DNS, key, region and backup loss | [Disaster recovery](disaster-recovery/README.md) and its [printable runbook](disaster-recovery/runbook.md) |
| Sync with GitHub | [Connecting to GitHub](github-sync.md) |
| Mirror a GitHub repository and deploy its Cloudflare Worker, from desktop or phone | [Mirror and deploy a Cloudflare Worker](cloudflare-workers.md) |
| Two-way issue sync with GitHub, Gitea or Forgejo | [Two-way forge sync](forge-sync.md) |

## Identity, trust and social

| Topic | Page |
| --- | --- |
| Identity export, recovery and portability | [Portable identities and recovery](portable-identities.md) |
| Scoped, signed trust lists | [Trust lists](trust-lists.md) |
| Portable stars, follows, endorsements and reviews | [Portable social signals](social-signals.md) |
| Fediverse digests and mention review | [Fediverse digests](fediverse-digests.md), [mention review](fediverse-mention-review.md) |
| Threaded collaboration from Lemmy | [Lemmy threaded collaboration](lemmy-threaded-collaboration.md) |

## Security

| Topic | Page |
| --- | --- |
| Chat encryption | [Chat encryption and integrations](security/sealed-chat.md) |
| Private repositories, backups and key recovery | [Private repository confidentiality](security/private-repository-confidentiality.md) |
| Organization roles | [Organization roles and the authorization matrix](security/organization-roles.md) |
| Secret scanning before replication | [Secret scanning](security/secret-scanning.md) |
| Third-party dependency review | [Dependency security audit](security/dependency-security-audit.md) |
| Local speech bridge | [World speech bridge](security/world-local-speech-bridge.md) |
| Outside services and what they observe | [External dependency map](external-dependency-map.md) |
| What the relay stores and how long | [Relay data retention and deletion](relay-data-retention.md) |

## Engineering, protocol and operations

| Topic | Page |
| --- | --- |
| What "done" requires | [Definition of done](engineering/definition-of-done.md) |
| Responsive interface budgets | [Daily interface validation](engineering/daily-interface-validation.md) |
| Normative protocol suite | [Protocol suite](protocol/README.md) |
| Coding agent on Workers AI | [Workers AI coding agent](cloudflare-workers-ai-coding-agent.md) |
| World community services | [Community services](world-community-services.md), [soundtrack licence](world-soundtrack-license.md), [community ad governance](community-ad-governance.md) |

## Directory layout

- `design/`: protocol and federation design decisions.
- `protocol/`: the versioned protocol suite, bound to `conformance/protocol/v1`.
- `governance/`, `funding/`: charters and generated registers, bound to `conformance/governance/v1` and `conformance/funding/v1`.
- `engineering/`: quality gates, performance budgets and completion policy.
- `security/`: security boundaries and local integration contracts.
- `operations/`: deployment, recovery, gateways, headless nodes and runbooks.
- `mobile/`: mobile node contract and the mobile Git engine.
- `plugins/`, `launch/`, `strategy/`, `research/`: planned plugins, launch material and research.
- `*.schema.json`: machine-readable configuration and protocol schemas.

When reporting a problem, include the version, the action taken and the
relevant log excerpt; see the [ForkMesh desktop app guide](qt-client.md) for log locations.
