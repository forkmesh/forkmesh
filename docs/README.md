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

```text
   your desktop node        independent mirrors          phones · browser
   holds and serves         carry copies, serve          read, review, commit,
   your repositories        clone and fetch              steer agents
          │                        ▲                           │
          │ signed push            │ signed refs events        │ HTTPS
          ▼                        │                           ▼
  ┌──────────────────────────────────────────────────────────────────────┐
  │  three equal core nodes · DNS round robin · no proxy in front        │
  │  accounts · web app · Git over HTTPS · chat · routing catalog        │
  └──────────────────────────────────────────────────────────────────────┘
            every record is signed, so any copy can be checked
```

## The six pillars

Everything ForkMesh promises rests on six short pages. Read them in order the
first time; each one links to the full detail.

```text
    ┌─────────────────────────────────────────────────────────────────────┐
    │       Code and releases stay reachable and provably authentic,      │
    │           whichever host, domain, account or network fails          │
    └─────────────────────────────────────────────────────────────────────┘
    ┌─────────┐ ┌─────────┐ ┌─────────┐ ┌─────────┐ ┌─────────┐ ┌─────────┐
    │    1    │ │    2    │ │    3    │ │    4    │ │    5    │ │    6    │
    │         │ │         │ │         │ │         │ │         │ │         │
    │  Archi- │ │ Encryp- │ │ Mirror- │ │  Self-  │ │ Censor- │ │  Threat │
    │ tecture │ │   tion  │ │   ing   │ │ hosting │ │   ship  │ │  model  │
    │         │ │         │ │         │ │         │ │  resil- │ │         │
    │         │ │         │ │         │ │         │ │  ience  │ │         │
    └─────────┘ └─────────┘ └─────────┘ └─────────┘ └─────────┘ └─────────┘
    ═══════════════════════════════════════════════════════════════════════
           Git, signed .forkmesh/ records and an open protocol suite
```

| | Pillar | Read it to learn | In one line |
| --- | --- | --- | --- |
| 🏗️ | **[Architecture and availability](architecture.md)** | How the network is built, and why no single server has to stay up | Three equal cores behind DNS round robin; with two down the third serves read-only |
| 🔐 | **[Encryption and signatures](encryption.md)** | What is signed and encrypted, with which algorithms, and the known gaps | Ed25519 everywhere; chat, calls and private repositories use X25519 + ML-KEM-768 |
| 🪞 | **[Mirroring and software distribution](mirroring.md)** | How copies stay in step, how clones are verified, how releases reach users | Clones go only to a mirror whose refs match the signed pin, and fail closed otherwise |
| 🏠 | **[Self-hosting](self-hosting.md)** | Run a node, a mirror, an instance or a fleet yourself | Four levels, from a desktop node to a fleet; an instance takes about 15 minutes |
| 🛡️ | **[Censorship resilience](censorship-resilience.md)** | What still works when a host, domain, account or network is blocked | Mirrors, Tor and I2P transports, USB bundles and portable keys, with the limits stated |
| 🎯 | **[Threat model](protocol/threat-model.md)** | What the protocol defends against, and what is still open | A numbered register of threats, each with its severity, its status and the controls that answer it |

## Find your way

| I want to… | Start here |
| --- | --- |
| 🚀 Install ForkMesh and make my first repository | [Getting started](getting-started.md) |
| 💾 Keep a safe copy of a GitHub project | [Back up a GitHub repository](back-up-a-github-repository.md) |
| 📱 Use ForkMesh on my phone or watch | [Phone apps](mobile/README.md) |
| 🧩 Add ForkMesh to Chrome | [Browser extension](browser-extension.md) |
| 🤖 Work with coding agents | [Working with coding agents](coding-agents.md) |
| 🖥️ Run my own server | [Self-hosting](self-hosting.md) |
| 🆘 Save a project that is under threat right now | [Crisis preservation](crisis-preservation.md) |
| 🛠️ Change ForkMesh itself | [How the codebase fits together](codebase.md) |

## Use ForkMesh

### Get started

[Getting started](getting-started.md) ·
[ForkMesh apps](apps.md) · [Desktop app guide](qt-client.md) ·
[Installation support matrix](install-support-matrix.md) ·
[Back up a GitHub repository](back-up-a-github-repository.md) ·
[Working with coding agents](coding-agents.md) ·
[Git errors](git-errors.md)

### Apps on every screen

| | App | Guide |
| --- | --- | --- |
| 🖥️ | Desktop (Linux, Windows, macOS) | [Desktop app guide](qt-client.md) |
| 📱 | iPhone, iPad and Android | [Phone apps](mobile/README.md) · [Getting started on a phone](mobile/getting-started.md) · [Android](mobile/android.md) · [Privacy and security on the phone](mobile/privacy-and-security.md) |
| ⌚ | Apple Watch | [Apple Watch](mobile/apple-watch.md) |
| 🧩 | Chrome and Chromium browsers | [Browser extension](browser-extension.md) |
| 🌐 | Web | [ForkMesh apps](apps.md) |

Phone guides: [Mobile node contract](mobile/mobile-node-contract.md) ·
[Owned Git engine](mobile/owned-git-engine-spec.md) · [Siri](mobile/siri.md) ·
[Apple Health](mobile/apple-health.md) · [File editing](mobile/mobile-file-editing.md) ·
[Agent access](mobile/mobile-agent-access.md)

### Repositories

[Reading and cloning without an account](accountless-reading.md) ·
[Hosted repositories](hosted-repositories.md) ·
[Contribute with git only](contributing-with-git.md) ·
[Repository imports](repository-imports.md) ·
[GitHub and forge sync](forge-sync.md) ·
[Folder About boxes](folder-about.md) ·
[Subscribing by feed or email](feeds-and-email.md) ·
[Mirror and deploy a Cloudflare Worker](cloudflare-workers.md) ·
[Workers AI coding agent](cloudflare-workers-ai-coding-agent.md)

### Availability

[Synchronization status](sync-status.md) ·
[Mirror goals](mirror-quota.md) ·
[Availability proof](availability-proof.md) ·
[Repository availability contracts](availability-contract.md) ·
[Crisis preservation](crisis-preservation.md) ·
[Offline and low-bandwidth sync](offline-sync.md) ·
[Transports and anonymity postures](transports.md) ·
[Dependency workflows](dependency-workflows.md)

### Chat and calls

[Voice and video calls](calls.md) ·
[Safety numbers and trusted devices](chat-safety-numbers.md) ·
[Mentions and username suggestions](mentions-and-autocomplete.md) ·
[DJ Studio jams](dj-studio.md)

### Identity, trust and social

[Portable identities and recovery](portable-identities.md) ·
[Trust lists](trust-lists.md) · [Portable social signals](social-signals.md) ·
[Fediverse digests, mention review and Lemmy threads](fediverse.md)

### World

[Community services, soundtrack licence and ad governance](world-community-services.md)

## Run ForkMesh

| | Area | Start with |
| --- | --- | --- |
| 🏠 | Your own node, mirror or instance | [Self-hosting](self-hosting.md), [Start your own instance](start-your-own-instance.md), [Start from a settings file](start-config.md), [mirror node](operations/mirror-node.md) |
| 🛰️ | The served stack and its runbook | [Operations](operations/README.md): [served stack](operations/served-stack.md), [runbook](operations/runbook.md), [vault](operations/vault.md), [runners](operations/runners.md), [instances](operations/instances.md) |
| 🧯 | Recovery | [Disaster recovery](disaster-recovery/README.md) and its [printable runbook](disaster-recovery/runbook.md) |
| 🔒 | Security | [Security](security/README.md): trust boundaries, keys, [private repositories](security/private-repository-confidentiality.md), [chat encryption](security/sealed-chat.md), [organization roles](security/organization-roles.md), [secret scanning](security/secret-scanning.md) |
| 🔎 | What outside services see, and what the service keeps | [External dependency map](external-dependency-map.md), [Relay data retention and deletion](relay-data-retention.md) |
| 📦 | Releases and installers | [Releases](release/README.md) |

## Reference

| | Reference | What it holds |
| --- | --- | --- |
| 📜 | [Protocol suite](protocol/README.md) | The normative wire protocol, signatures and routing |
| 🧠 | [Design decisions](design/README.md) | Collaboration, trust lists, review threads, release publishing and federation |
| 🔌 | [Integration contract](integrations.md) | What outside tools can rely on |
| 🧪 | [Git compatibility matrix](git-compatibility.md) · [Conformance suites](conformance/README.md) | Evidence that stock Git and other implementations work |
| 🗺️ | [Surface capability matrix](capability-matrix.md) · [Searchable repository index](repository-index.md) | Which app does what, and where each repository lives |
| ✍️ | [Terminology and voice guide](terminology.md) | The words ForkMesh uses, and how it writes |

Machine-readable `*.schema.json` files sit in this folder.

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

| | Part | Code | Home page |
| --- | --- | --- | --- |
| 🖥️ | Desktop app (Qt) | `desktop/` | [desktop/README.md](desktop/README.md) |
| 🤖 | Agent runs | `desktop/src/*Agent*`, `server/`, `app/src` | [agents/README.md](agents/README.md) |
| 🐍 | App (Python on the core nodes) | `app/` | [app/README.md](app/README.md) |
| 🧱 | CPython host for the app | `app/host/` | [host/README.md](host/README.md) |
| 🐹 | Go daemon (mirror, core, runner roles), calls | `server/` | [server/README.md](server/README.md) |
| 🌐 | Website, docs site and the Cloud Dragon mascot | `www/`, `app/tools/docs_site.py` | [www/README.md](www/README.md), [Cloud Dragon](www/cloud-dragon.md) |
| 📱 | Phone apps (iOS, watchOS, Android) | `native/` | [native/README.md](native/README.md) |
| 🛍️ | Marketplace packages and plugins | `marketplace/` | [marketplace/README.md](marketplace/README.md) |
| 🧩 | Browser extension | `marketplace/packages/browser-extension/` | [marketplace/browser-extension.md](marketplace/browser-extension.md) |
| 🌍 | World (3D) and Office (2D) | `world/`, `office/` | [world/README.md](world/README.md), [office/README.md](office/README.md) |
| 🛰️ | Fleet, deploys and operator tools | `deploy/`, `tools/next_*.py` | [operations/README.md](operations/README.md) |
| 📦 | Releases and installers | `tools/package/`, `.forkmesh/releases/` | [release/README.md](release/README.md) |
| 🧰 | Repository tools | `tools/` | [tools/README.md](tools/README.md) |
| 📏 | Engineering rules | cross-cutting | [engineering/README.md](engineering/README.md) |

Special agents are workflows in `.forkmesh/workflows/`
([app/workflows.md](app/workflows.md#desktop-workflows-and-special-agents)):
[CFO](finance/cfo.md), [Hearth](community/hearth.md),
[Reddit scout](community/reddit.md), [site crawler](www/site-crawler.md) and
[Sentinel](security/sentinel.md). The
guide for outside coding agents is `app/public/forkmesh-agent.md`, served at
`https://app.forkmesh.com/forkmesh-agent.md`. Keep it in step with
[instances](operations/instances.md), [start-config.md](start-config.md) and the
pull request section of `CONTRIBUTING.md`; `app/tests/test_start_instance_page.py`
checks the parts that must match.

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
- **Look.** Open a page with one sentence on what it covers. Draw a diagram as
  a `text` code block of box-drawing characters (no emoji inside it, since
  they break the column widths); it renders the same on the site, in the
  desktop and in the phone apps. An icon is one emoji in a table's first
  column or at the start of a list item, never in a heading, so heading
  anchors stay the same in every renderer. Tables beat long paragraphs for
  lists of facts.
- **Style.** Short sentences, present tense, facts over stories. Say what still
  depends on one provider; never claim more than the design delivers.
