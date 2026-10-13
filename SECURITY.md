# Security Policy

ForkMesh is an open-source desktop node, mirror daemon, relay and set of
mobile apps under active development. We take security seriously and
appreciate reports that help keep the network and its users safe.

## Supported Versions

ForkMesh ships from `main`; the desktop release line is 1.x (see
`.forkmesh/releases/latest/release.json`). Only the latest published build and
the current `main` branch receive security fixes. Please reproduce issues
against the most recent version before reporting.

| Version        | Supported          |
| -------------- | ------------------ |
| `main` (latest)| :white_check_mark: |
| Older builds   | :x:                |

## Reporting a Vulnerability

**Please do not report security vulnerabilities through public issues.**
ForkMesh issues are signed and published in-repo under
[`.forkmesh/issues/`](.forkmesh/issues/), so
a public report would disclose the problem before a fix is available.

Instead, report privately:

- Email the maintainers at **security@forkmesh.com**, or
- Submit the private form at https://forkmesh.com/security-report (also in
  **Desktop → Settings → Security**). The report is stored encrypted and
  emailed to the security team; it is never published in the public issue
  tracker.

Please include:

- a description of the vulnerability and its impact,
- the affected component (desktop node, relay/App, mirror daemon, hosted
  service, fleet tooling, mobile or native app, Marketplace package,
  identity/crypto, or website). The form
  lists fewer components; if yours is missing, choose **Other** and name it
  in the description,
- steps to reproduce or a proof of concept,
- the version or commit you tested against.

We acknowledge reports within 3 days. Once a fix is available we will
coordinate disclosure and credit reporters who wish to be named.

Security decisions follow the emergency process in the
[governance charter](www/docs/governance/README.md#security-decisions).
An embargo lasts at most 90 days. A public `FMS` record naming who decided,
when, and any dissent is published in the
[decision register](www/docs/governance/decision-register.md) within 7 days
of disclosure.

## Scope

Security-relevant areas of ForkMesh include:

- **Identity and signing** — local Ed25519 keys used to sign profile,
  repository, issue, and pull-request metadata.
- **Relay chat** — payloads are encrypted client-side with AES-256-GCM.
  Default rooms use a relay-derived authenticated shared key: clients apply
  PBKDF2-HMAC-SHA256 with 210,000 rounds, while the relay can derive the same
  key. Public channels intentionally remain controller-readable so ForkBot,
  MCP, and webhooks can participate. Private channels and direct messages add
  an automatic device-local X25519 layer that wraps a fresh content key for
  each known device, so the controller key opens only recipient ciphertext.
  The relay retains at most 500 persisted frames per room for 7 days. See
  [chat encryption and public integrations](www/docs/security/sealed-chat.md)
  for the wire format and unauthenticated-key-discovery boundary. Coves and
  encrypted mirrors use separate per-object encryption boundaries.

- **Mirroring** — bare Git mirrors fetched from remotes and served on demand.
  The complete private-repository leakage envelope, key lifecycle, keyless
  backup inspection command, and commercial-GA evidence requirements are in
  [private repository confidentiality](www/docs/security/private-repository-confidentiality.md).
- **Marketplace packages** — package Worker modules run with a
  least-privilege runtime; the Password Vault is end-to-end encrypted and its
  server holds no key ([`marketplace/packages/vault/README.md`](marketplace/packages/vault/README.md)).
- **Releases and installers** — Ed25519-signed release metadata and
  content-addressed binaries ([`docs/release/README.md`](docs/release/README.md)).
- **Donations and rewards** — Solana addresses published on profiles and
  repositories. Legacy custodial payouts are frozen; current rewards use
  externally owned wallets.
- **Hosted service** — the core nodes that run app.forkmesh.com and the
  runners that execute agent and Actions jobs
  ([docs/RUNNERS.md](docs/RUNNERS.md)).

The developer overview of trust boundaries is
[`docs/security/README.md`](docs/security/README.md).

Issues that are not directly exploitable (for example, missing best-practice
hardening with no demonstrated impact) are still welcome but may be triaged at
lower priority.

## Out of Scope

- Vulnerabilities in third-party dependencies should be reported upstream,
  though we appreciate a heads-up if ForkMesh is affected.
- Social-engineering, physical attacks, and denial-of-service against a single
  self-hosted node.

Thank you for helping keep ForkMesh and its community secure.
