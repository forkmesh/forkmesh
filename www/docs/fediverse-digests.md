# Fediverse daily digests

Public repository events (issues, pulls, discussions, commits, releases) are not
posted one by one. Each becomes a small record (category, title, public link
without query string, UTC time, scope) in an encrypted, bounded queue. Private
repositories are never queued, and a queue is dropped when its repository turns
private or federation off.

## Cadence

A scope posts at most one automated digest per 24 hours, after a 24-hour first
wait. Empty queues post nothing; a digest holds at most 12 updates from a queue
of at most 80. The scheduled job handles at most 12 scopes per run and delivers
through the ActivityPub outbox with backoff. Posts start with "ForkMesh
automated daily update".

## Controls

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
