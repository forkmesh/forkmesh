# Synchronization status

All clients describe synchronization with one model, `forkmesh-sync-status-v1`.

| State | Current | Meaning |
| --- | --- | --- |
| `queued`, `syncing` | no | Waiting or in flight |
| `verified` | yes | Every copy holds the latest signed state, confirmed within an hour |
| `degraded` | yes | Enough copies verified, some offline, behind or rejected |
| `stale` | no | No recent confirmation |
| `conflicted` | no | Copies disagree |
| `paused`, `external-only`, `failed` | no | Paused, only at an external provider, or rejected |

Nothing reports success without a verified copy, and conflict outranks success.
A status holds counts, timestamps and fixed wording only (`verifiedCopies`,
`trustedMirrors`, `lastConfirmedAt`, `lagMs`, `error`, `action`, `summary`), never
node names or raw errors.

Implementations: `app/src/sync_status.py` (API `syncStatus` on
`GET /api/repo/<owner>/<repo>/mirrors`), `desktop/src/SyncStatusModel.h`,
`SyncStatus.swift`, `SyncStatus.kt`, `server/syncstatus.go` and
`tools/forkmesh_sync_status.py explain` (exit 0 current, 3 not, 2 unreadable).
All match `conformance/sync-status/v1/fixture.json`, generated from `model.json`
(`staleAfterMs` 1 hour, `syncDeadlineMs` 15 minutes).
