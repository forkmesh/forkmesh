# Design decisions

Specifications for features that cross the protocol, the app and the clients.
Conflict-safe collaboration is normative and part of the
[protocol suite](../protocol/README.md); scoped trust lists are a candidate
protocol with shared vectors; the others record how a feature works and why.

| Page | Covers |
| --- | --- |
| [Conflict-safe collaboration](conflict-safe-collaboration.md) | Signed collaboration events, identifiers, causality, deterministic fold and conflict rules, moderation lineage |
| [Scoped trust lists](scoped-trust-lists.md) | Signed, scoped community trust lists: documents, verification order, revocation, appeals, merge rules |
| [Pull review threads](pull-review-threads.md) | Signed anchors that keep review threads on the right lines across patchsets and branches |
| [Release binary publishing](release-binary-publishing.md) | Signed release metadata in Git, payloads in a content-addressed store, verification order |
| [ActivityPub federation](activitypub-federation.md) | Followable fediverse actors, HTTP signatures, inbox actor binding, delivery |

Related core pages: [encryption and signatures](../encryption.md),
[mirroring and distribution](../mirroring.md) and the
[threat model](../protocol/threat-model.md). Back to the
[documentation index](../README.md).
