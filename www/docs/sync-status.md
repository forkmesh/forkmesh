# Synchronization status

Every repository and collaboration object explains where it has reached, how
many copies were verified, and whether what you see is current or conflicted.
The web app, desktop, mobile, the mirror node, the API, and the CLI all use one
state model, `forkmesh-sync-status-v1`, so the same evidence reads the same way
everywhere.

## States

| State | Severity | Current | Meaning |
| --- | --- | --- | --- |
| `queued` | info | no | A synchronization is waiting to start. |
| `syncing` | info | no | A synchronization is in flight and within its deadline. |
| `verified` | ok | yes | Every copy holds the latest state with a verified signature, confirmed within the last hour. |
| `degraded` | warning | yes | Enough copies are verified, but at least one is offline, behind, unverified, or rejected. |
| `stale` | warning | no | Nothing confirms the latest state recently: copies are offline, the sync missed its deadline, or the last confirmation is over an hour old. |
| `conflicted` | error | no | Copies disagree about the latest state and someone has to choose. |
| `paused` | info | no | An owner or operator paused synchronization. |
| `external-only` | info | no | The object so far exists only at an external provider (for example, a GitHub import that has not been mirrored yet). |
| `failed` | error | no | A copy rejected the update or synchronization reported an error. |

Only `verified` and `degraded` count as current. Nothing reports success
without a verified copy of the latest state, and a conflict always outranks
success.

## What each status contains

```json
{
  "schema": "forkmesh-sync-status-v1",
  "state": "degraded",
  "label": "Degraded",
  "severity": "warning",
  "current": true,
  "totalCopies": 3,
  "nodesReached": 2,
  "verifiedCopies": 2,
  "trustedMirrors": 1,
  "lastConfirmedAt": 1799999970000,
  "lagMs": 180000,
  "error": "unreachable",
  "action": "One or more copies could not be reached; check that the mirrors are online.",
  "summary": "Verified, but some copies are not current. 2 of 3 copies verified, 1 trusted mirror, 2 nodes reached. Last confirmed just now. 3 min behind the source. One or more copies could not be reached; check that the mirrors are online."
}
```

- **Nodes reached**: copies that answered, including ones that rejected the update.
- **Verified copies**: copies holding the latest state with a verified signed attestation.
- **Trusted mirrors**: verified copies admitted to serve clones.
- **Last confirmation**: the newest confirmation from a verified copy.
- **Lag**: how long ago the source changed, while not every copy has the change.
- **Error and action**: a code from a closed list, plus what to do about it.

## Privacy

A status is built only from counts, timestamps, and fixed wording. Node names,
peer addresses, private object names, and raw error messages in the evidence
are never repeated. An error code a client does not recognize is reported as
`unknown`, so the status is safe to show to anyone who can see that the object
exists. The shared fixture plants private strings in its evidence, and every
client's test fails if one of them appears in the output.

## Where it appears

| Client | Where | Implementation |
| --- | --- | --- |
| API | `syncStatus` on `GET /api/repo/<owner>/<repo>/mirrors` | `app/src/sync_status.py` |
| Web | Mirrors tab status line on the repository page | renders the API's `syncStatus` |
| Desktop | Mirror nodes summary (hover for the full explanation) | `desktop/src/SyncStatusModel.h` |
| iOS | Repository Mirrors tab | `native/ios/ForkMeshCore/Sources/FMAccount/SyncStatus.swift` |
| Android | Repository Mirrors tab | `native/android/core/src/main/kotlin/com/forkmesh/nativeapp/api/SyncStatus.kt` |
| Mirror node | `syncStatus` in the loopback `/v1/status` and `/readyz` responses | `server/syncstatus.go` |
| CLI | `tools/forkmesh_sync_status.py` | reuses the API implementation |

The mirrors payload maps each row to one copy: an offline row is unreachable;
an online row with integrity `rejected` is rejected; `behind` or integrity
`healing` is behind; otherwise the row is current. It counts as verified when
integrity is `ok`, and as trusted when `cloneAvailable` or `endpointHealthy` is
true.

## CLI

```sh
tools/forkmesh_sync_status.py explain evidence.json           # Degraded: Verified, but ...
tools/forkmesh_sync_status.py explain --json evidence.json    # machine-readable status
curl -s https://app.forkmesh.com/api/repo/forkmesh/forkmesh/mirrors \
  | tools/forkmesh_sync_status.py explain --mirrors -
tools/forkmesh_sync_status.py fixture                         # verify the shared fixture
```

`explain` exits 0 when the state is current, 3 when it is not, and 2 when the
input is unreadable. `fixture` exits 1 on any mismatch.

## Conformance

- `conformance/sync-status/v1/model.json` sets out the states, precedence, wording, and policy (`staleAfterMs` 1 hour, `syncDeadlineMs` 15 minutes).
- `conformance/sync-status/v1/fixture.json` holds evidence cases, offline, delayed, rejected, and conflict sequences that each end recovered, and mirrors-payload cases, all with their exact expected output.
- `conformance/sync-status/v1/generate_fixture.py` regenerates the expected output from the reference implementation. Every entry also records a hand-written expected state, so a reference change that moves a state fails loudly. Run it with `--check` in CI.

Every implementation compares its full output with the fixture:

| Suite | Command |
| --- | --- |
| API, CLI, web | `python -m pytest app/tests/test_sync_status.py` |
| Mirror node | `cd server && go test -run SyncStatus .` |
| Desktop | `forkmesh-control-tests` |
| iOS | `cd native/ios/ForkMeshCore && swift test --filter SyncStatusTests` |
| Android | `cd native/android && ./gradlew :core:coreTest -Pfilter=SyncStatus` |

To change the model, edit `model.json` and the reference implementation, run
`generate_fixture.py`, then update each client until its suite passes. Adding
`syncStatus` is additive, so older clients ignore it. Rolling back the Worker
removes the field, and the desktop and mobile clients keep deriving the status
from the mirror rows.
