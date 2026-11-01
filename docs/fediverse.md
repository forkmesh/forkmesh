# Fediverse digests, mentions and replies

How public repositories talk to the fediverse: outbound daily digests,
inbound mentions that an owner can turn into issues, and threaded Lemmy or
Mastodon replies on issues, pull requests and discussions. Actors, handles and
signatures are in [ActivityPub federation](design/activitypub-federation.md).
Private repositories never federate.

## Daily digests

Public repository events (issues, pulls, discussions, commits, releases) are not
posted one by one. Each becomes a small record (category, title, public link
without query string, UTC time, scope) in an encrypted, bounded queue. Private
repositories are never queued, and a queue is dropped when its repository turns
private or federation off.

A scope posts at most one automated digest per 24 hours, after a 24-hour first
wait. Empty queues post nothing; a digest holds at most 12 updates from a queue
of at most 80. The scheduled job handles at most 12 scopes per run and delivers
through the ActivityPub outbox with backoff. Posts start with "ForkMesh
automated daily update".

The repository **Post updates** switch enables the digest and previews its
text. `"manual": true` on `ap-publish` keeps immediate owner announcements.

```text
GET  /api/repo/{owner}/{repo}/fediverse-digest
GET  /api/orgs/{org}/fediverse
POST /api/orgs/{org}/fediverse   {"enabled": true|false}
```

The repository endpoint is owner-only, `no-store`, and `404` for private
repositories. The organization switch only suppresses: a repository owner's
`federate` and `broadcastEvents` settings stay authoritative.

## Mention review

World lists an inbound ActivityPub Note only after HTTP-signature verification,
when it addresses a published public repository actor and the ActivityStreams
Public collection. Private or missing repositories are rejected before a
record is written; instance blocks and the repository's `federate` and
`acceptComments` controls apply.

Mentions never create issues automatically. States:

| State | Meaning |
| --- | --- |
| `review` | Available for owner review |
| `pending` | The owner confirmed a draft and the encrypted owner inbox accepted it |
| `created` | The owner node committed the issue and returned its number in a signed acknowledgement |

An item stays pending until the owner node confirms the committed issue
number.

Redeliveries are deduplicated by a blind index of the remote Note id. The
public feed exposes only a random id, sanitized author/post fields, repository
name, and generalized progress.

Follow-up consent is off by default. With consent, one idempotent public reply
with the issue link is queued only after owner-node confirmation.

API: `GET /api/world/fediverse-mentions`, and
`POST /api/world/fediverse-mentions/{id}/preview|create|moderate|materialized`.
The desktop inbox acknowledgement carries
`materialized={review-id}:{real-issue-number}` entries.

## Threaded replies (Lemmy-compatible)

ForkMesh accepts public ActivityPub replies (Lemmy or Mastodon `Create` of a
`Note`, `Page` or `Article`) to public federated issues, pull requests and
discussions, including replies to other verified remote replies.

### Trust boundary

Remote replies are stored encrypted in `ap_comments`
(`forkmesh-federated-thread-v1`), never in `.forkmesh` records or any signed
event stream. An HTTP signature authenticates the remote actor but does not make the reply a ForkMesh native event;
projections carry `federated: true` and `nativeEvent: false`.

A remote object cannot select a ForkMesh context: a top-level reply inherits the
context of the `/ap/o/{id}` object it targets, a nested reply that of its
stored parent, and unresolved targets are discarded.

### Lifecycle

- `Create` is idempotent per remote `id`.
- `Update` replaces the body only when the author matches; marked `edited`.
- `Delete` leaves a `tombstoned` placeholder; `Remove` (same instance only)
  leaves `moderated`.
- `Undo(Remove)` waits for redelivery instead of restoring cached content.

Blocked instances (and their subdomains) are rejected before verification and
hidden on read. Private repository threads never federate.

### Client API

```text
GET /api/repo/{owner}/{repo}/fedi-comments?kind={issue|pull|discussion}&number={n}
```

Clients render bodies as plain text, accept only HTTPS backlinks, cap nesting
depth, label edited and removed records, and never parse them as native
events. Fixtures are in `app/tests/fixtures/activitypub/lemmy/`.
