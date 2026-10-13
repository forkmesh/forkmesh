# Lemmy-compatible threaded collaboration

ForkMesh accepts public ActivityPub replies to public, federated issue,
pull-request, and discussion objects. Lemmy `Create` activities containing a
`Note`, `Page`, or `Article` use the same normalization boundary as compatible
Mastodon replies. Nested replies may target either the original ForkMesh
ActivityPub object or another verified remote reply.

## Trust boundary

Remote replies are stored in `ap_comments` using the
`forkmesh-federated-thread-v1` encrypted record schema and the
`federatedReplies` storage namespace. They are never written to:

- `.forkmesh/issues`;
- `pulls`;
- `.forkmesh/discussions`; or
- any ForkMesh Ed25519-signed event stream.

An ActivityPub HTTP signature authenticates delivery by a remote actor. It
does not make the reply a ForkMesh native event. API projections therefore always
include `federated: true`, `nativeEvent: false`, remote provenance, and a
separation notice. Clients show the remote instance and canonical HTTPS
backlink alongside that notice.

A remote object cannot select a ForkMesh context. A top-level reply inherits
the context of the local `/ap/o/{id}` object it targets. A nested reply inherits
the encrypted context of its already-stored parent. Unresolved targets are
acknowledged and discarded.

## Lifecycle and deduplication

The remote object’s HTTPS `id` receives a stable blind index and SHA-256
deduplication key.

- `Create` inserts once; repeated delivery is idempotent.
- `Update` replaces only the public projection body and records `edited`.
  The author must match the verified original author.
- `Delete` erases the projected body and leaves a `tombstoned` placeholder so
  nested thread structure and the remote backlink remain understandable.
- `Remove` erases the projected body and records `moderated`. It is accepted
  only from a verified actor on the same remote instance.
- `Undo(Remove)` makes a record await safe redelivery rather than resurrecting
  moderation-hidden content from a local cache.

Tombstone and moderation placeholders never expose removed bodies.

## Instance blocks and privacy

The existing ActivityPub instance block list is checked before signature
verification and persistence. The read projection checks it again so a newly
blocked instance disappears immediately without waiting for encrypted-row
cleanup. Subdomains of a blocked domain are blocked as well.

Only public repository threads federate. Private repository visibility checks
run before the comments endpoint, and a remote activity cannot discover or
attach itself to an encrypted private repository.

## Client API

All clients read:

```text
GET /api/repo/{owner}/{repo}/fedi-comments?kind={issue|pull|discussion}&number={n}
```

The ordered `comments` array contains nesting depth, lifecycle, author,
instance/software provenance, plain-text body, canonical backlink, and the
non-native marker. The Web dashboard, Qt client, iOS and Android apps, and World use
this projection and render it in a distinct “Fediverse thread” area below or
beside the native collaboration timeline.

Clients must render bodies as plain text, accept backlinks only when they are
HTTPS URLs, cap visible nesting depth, and label edited, tombstoned, and
moderated records. No client may deserialize these records into a native event
class.

## Fixtures

Interoperability fixtures live under:

```text
app/tests/fixtures/activitypub/lemmy/
```

They cover issue create/update, pull-comment tombstones, nested discussion
replies, moderation, duplicate delivery, author mismatch, and instance blocks.
