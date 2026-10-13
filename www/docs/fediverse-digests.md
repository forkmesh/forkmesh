# Fediverse daily digests

ForkMesh does not publish a separate automated ActivityPub post for every
repository event. Public issue, pull-request, discussion, commit, and release
hooks are reduced to a small metadata record and placed in an encrypted,
bounded queue.

Each queued record contains only:

- an event category;
- a short public title;
- a stable public ForkMesh link with its query string removed;
- a UTC timestamp; and
- a repository or organization-alias scope.

Bodies, source excerpts, form values, private paths, request metadata, and
credentials are not retained in the digest queue. Private repositories are
rejected before enqueueing. If a public repository becomes private or its owner
turns automatic federation off, its queued automatic posts are deleted.

## Cadence and delivery

A new queue waits 24 hours before its first eligible post so events can be
combined. After publication, the same repository actor cannot publish another
automatic digest for at least 24 hours. Empty queues do not create posts.
Duplicate events receive deterministic IDs, and each digest contains at most 12
updates from a queue capped at 80 records.

The scheduled Worker processes at most 12 due scopes per run. It creates a
deterministic ActivityPub Note and retryable delivery rows in the same D1 batch
that removes the published event IDs. Delivery then uses the existing
ActivityPub outbox, including remote `Retry-After` handling, exponential
backoff, domain blocking, and capped attempts.

Every generated post starts with “ForkMesh automated daily update” and ends
with an automated-post label. Links point to the relevant public repository
resource.

## Repository controls and preview

Repository owners manage the existing “Post updates” switch in the repository
About settings. The switch now means “include meaningful public updates in at
most one automated daily digest.” The same form shows the exact currently
queued digest text. The owner-only API is:

```text
GET /api/repo/{owner}/{repo}/fediverse-digest
```

It requires the owner’s authenticated session (or the existing owner-key
proof), returns `Cache-Control: no-store`, and returns `404` for private
repositories.

Explicit owner-authenticated announcements sent to `ap-publish` with
`"manual": true` retain the immediate publication behavior. All calls without
that explicit flag enter the daily queue.

## Organization controls

Organization owners and administrators can enable or disable organization-alias
digests and preview every eligible linked public repository at:

```text
GET  /api/orgs/{org}/fediverse
POST /api/orgs/{org}/fediverse
```

The POST body is `{ "enabled": true|false }`. This is a suppress-only control:
the backing repository owner’s `federate` and `broadcastEvents` settings remain
authoritative. An organization administrator cannot enable a repository the
owner disabled, and organization membership does not grant visibility into a
private repository.
