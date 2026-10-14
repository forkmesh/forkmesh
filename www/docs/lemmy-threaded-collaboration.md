# Lemmy-compatible threaded collaboration

ForkMesh accepts public ActivityPub replies (Lemmy or Mastodon `Create` of a
`Note`, `Page` or `Article`) to public federated issues, pull requests and
discussions, including replies to other verified remote replies.

## Trust boundary

Remote replies are stored encrypted in `ap_comments`
(`forkmesh-federated-thread-v1`), never in `.forkmesh` records or any signed
event stream. An HTTP signature authenticates the remote actor but does not make the reply a ForkMesh native event;
projections carry `federated: true` and `nativeEvent: false`.

A remote object cannot select a ForkMesh context: a top-level reply inherits the
context of the `/ap/o/{id}` object it targets, a nested reply that of its
stored parent, and unresolved targets are discarded.

## Lifecycle

- `Create` is idempotent per remote `id`.
- `Update` replaces the body only when the author matches; marked `edited`.
- `Delete` leaves a `tombstoned` placeholder; `Remove` (same instance only)
  leaves `moderated`.
- `Undo(Remove)` waits for redelivery instead of restoring cached content.

Blocked instances (and their subdomains) are rejected before verification and
hidden on read. Private repository threads never federate.

## Client API

```text
GET /api/repo/{owner}/{repo}/fedi-comments?kind={issue|pull|discussion}&number={n}
```

Clients render bodies as plain text, accept only HTTPS backlinks, cap nesting
depth, label edited and removed records, and never parse them as native
events. Fixtures are in `app/tests/fixtures/activitypub/lemmy/`.
