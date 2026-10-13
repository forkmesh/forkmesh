# Offline and low-bandwidth sync

Use this guide when a repository has to keep moving without a dependable
connection: a link that drops every few minutes, a metered data plan, a
community network with no internet uplink, or an air gap crossed on a USB
stick.

`tools/forkmesh_offline_sync.py` does four things:

1. **Export** refs and collaboration history to a directory, signed with your
   Ed25519 key. The export can be full or limited: some branches, some paths,
   metadata only, or leaving large files for later.
2. **Fetch** that directory from removable media or any static HTTP host. The
   fetch works in verified chunks, resumes after disconnects, crashes, and
   cancellation, and respects a data budget, a rate limit, and time windows.
3. **Import** it into a repository. Import checks the signature, the digests,
   and any missing objects before it changes anything. Refs then update in one
   transaction; diverged work is kept aside for review, never overwritten.
4. **Roll back** any import.

Issues, pull requests, discussions, and releases live as signed records under
`.forkmesh/` in the repository's branches (for example `forkmesh/pulls`).
Exporting the refs therefore carries the collaboration history with them.

The manifest is `forkmesh-offline-media-v1`, wrapped in the signed envelope
`forkmesh-offline-media-signed-v1`. A transfer checkpoint is
`forkmesh-offline-transfer-checkpoint-v1`, an import journal is
`forkmesh-offline-import-journal-v1`, and a replica advertisement is
`forkmesh-replica-scope-v1`.

## Start here: carry a repository on a USB stick

On the connected machine:

```sh
tools/forkmesh_offline_sync.py keygen ~/.forkmesh-offline.key   # prints your public key
tools/forkmesh_offline_sync.py export ~/src/project /media/usb/project-2026-09-14 \
  --signing-key ~/.forkmesh-offline.key --repository alice/project
```

Send the printed public key to the other side over any channel you trust
(read it aloud, put it on paper, or use a signed message). On the disconnected
machine:

```sh
tools/forkmesh_offline_sync.py verify /media/usb/project-2026-09-14 --trusted-key <public key>
tools/forkmesh_offline_sync.py import /media/usb/project-2026-09-14 ~/src/project \
  --trusted-key <public key>
```

Carry changes back the same way with that machine's own key. Next time, export
only what is new:

```sh
tools/forkmesh_offline_sync.py export ~/src/project /media/usb/project-2026-09-21 \
  --signing-key ~/.forkmesh-offline.key --since /media/usb/project-2026-09-14
```

An incremental export lists its `prerequisites` and the digest of the export
it builds on (`previousExport`). Import refuses it, and reports the missing
prerequisite commits, until the earlier export has been imported. If nothing
changed, `export --since` writes nothing.

## Fetch over a link that keeps dropping

Put the export directory on any static HTTP host that honours `Range`
requests, then fetch it:

```sh
tools/forkmesh_offline_sync.py fetch https://files.example.org/project-2026-09-14/ ./incoming \
  --rate 64k --budget 20M --schedule 22:00-06:00 --trusted-key <public key>
tools/forkmesh_offline_sync.py status ./incoming
tools/forkmesh_offline_sync.py import ./incoming ~/src/project --trusted-key <public key>
```

`SOURCE` can also be a local directory, such as a mounted share or a flaky
card reader. Read errors on local media are treated as disconnects.

How a fetch stays correct:

- **The manifest is fetched and verified first,** exactly once per transfer.
  It is stored next to the checkpoint and never requested again, so a metered
  link does not pay for it every session.
- **Chunks resume from the byte they stopped at.** Bytes are appended to
  `.forkmesh-transfer/chunks/NNNNNNNN.part` as they arrive. After a disconnect
  the next request starts at the first missing byte, so no byte is transferred
  twice.
- **A chunk counts only after its SHA-256 matches the signed manifest.** Only
  then is it renamed to `.done` and recorded in `checkpoint.json` (written
  atomically). If a digest is wrong, the whole chunk is fetched again; after 3
  failures on one chunk the fetch stops with `failed` / `integrity`. Every
  resume re-checks finished chunks on disk, so a checkpoint lost in a crash, or
  a chunk damaged on disk, costs at most that one chunk.
- **The bundle is assembled only when every chunk is verified,** and its
  whole-file digest must match before `repository.bundle` and the manifest
  appear in `DEST`. A partial directory can never be imported.
- **Retries back off** (0.5 s doubling to 30 s). Progress resets the retry
  count. `--retries N` consecutive failures with no progress pause the fetch
  with `link_down`; run the same command again later.
- **Cancel with Ctrl-C.** The current block is written, the checkpoint is saved,
  and the fetch exits paused with `cancelled`.
- `--restart` discards the checkpoint, for example when the source directory
  was replaced by a different export.

### Data budgets, rate, and schedules

| Option | Effect |
| --- | --- |
| `--budget SIZE` | Transfer at most SIZE bytes this run, then pause with `budget_exhausted`. The request is trimmed to the budget, so it never overshoots. |
| `--rate SIZE` | Average at most SIZE bytes per second. |
| `--schedule HH:MM-HH:MM` | Transfer only inside local-time windows. Windows may cross midnight, and the option may repeat. Outside a window the fetch pauses with `outside_schedule` before any request, and reports `nextWindowAt`. |
| `--wait` | Sleep until the next window instead of exiting. |

Sizes accept bytes or binary `k`, `M`, `G` suffixes (`64k`, `20M`, `1GiB`).

### Estimates and checkpoints

`status DEST` reads only the local checkpoint. It never contacts the source.
It reports `totalBytes`, `bytesOnDisk`, `remainingBytes`, chunk counts, percent
complete, and `etaSeconds` at the last session's measured rate. `fetch --json`
also returns `counters`:

| Counter | Meaning |
| --- | --- |
| `sessions`, `resumes` | Runs of this transfer, and runs that continued earlier progress. |
| `disconnects` | Reads that ended early. |
| `bytesTransferred` | Bundle bytes received over the link. With no integrity failures this equals the bundle size, however many disconnects happened. |
| `bytesResumed` | Bytes already on disk when a session started, which did not need fetching again. |
| `integrityFailures`, `retransferredBytes` | Chunks that failed their digest, and the bytes discarded with them. |
| `manifestBytes` | The one-time manifest download. |

## Limited exports and what they may claim

| Export option | Limitation code | What is left out |
| --- | --- | --- |
| `--metadata-only` | `metadata_only` | File contents, except collaboration records under `.forkmesh/`. Commits, trees, and refs travel. |
| `--path PATH` (repeatable) | `path_filter` | File contents outside PATH. `.forkmesh/` is always kept. |
| `--defer-blobs-over SIZE` | `deferred_large_objects` | File contents larger than SIZE. |
| `--ref GLOB` (repeatable) | `branch_filter` | Refs that match no glob. |
| _(detected on import)_ | `missing_objects` | Objects the repository references but does not hold. |

Every manifest carries a `replica` advertisement, derived only from the
selection:

- **`full`** — no limitation. Only a full replica sets `claimsFullAvailability: true`.
- **`filtered`** — at least one limitation, but not metadata-only.
- **`metadata-only`** — metadata only, possibly with other limitations.

The advertisement is part of the signed manifest, and `verify` recomputes it
from the selection. An export that claims more than its selection allows fails
verification, even when it is correctly signed.

Import refuses a limited export unless you pass `--allow-limited`. Before
recording any ref, it checks that no commit or tree object is missing. Left-out
file contents are stored as a Git partial clone: the pack is marked
`.promisor`, and `extensions.partialClone` is set to `forkmesh-offline`. Reading
a left-out file fails with Git's missing-object error rather than showing wrong
content.

```sh
tools/forkmesh_offline_sync.py replica ~/src/project --json
```

`replica REPO` prints what the repository may advertise. It combines the
recorded limitations with a live count of missing objects. A limited
repository keeps advertising its limitations until a complete full export (one
with no `--since`) is imported and no objects are missing.

When a copy reports that scope as `replicaScope` in the mirrors payload, the
[availability proof](availability-proof.md) classifies it as `observed` with
`limited_replica`, however fresh its evidence. It never counts toward a
quorum.

## Import, reconciliation, and rollback

Import verifies, in order: the envelope signature and the trusted key, the
manifest shape and replica advertisement, every chunk digest and the bundle
digest, the scope (`--allow-limited`), the object format, and the
prerequisites. Any failure exits `3` before any ref is written. It then
unpacks the objects with `git bundle unbundle` and checks that every commit and
tree the refs need is present. For a full export, every file's contents must be
present too. Only then does it decide each ref:

| Decision | When | What happens |
| --- | --- | --- |
| `create` | The ref does not exist locally. | Created. |
| `fast_forward` | The local ref is an ancestor of the incoming one. | Moved forward. |
| `unchanged` | Same object. | Nothing. |
| `local_ahead` | The incoming ref is an ancestor of the local one. | Local kept. |
| `conflict` | History diverged, or a tag points elsewhere. | Local kept; incoming saved at `refs/forkmesh-offline/conflicts/<exportId>/…`. |
| `checked_out` | A fast-forward of the branch checked out in a working tree. | Local kept; incoming saved aside so you can `git merge --ff-only` it. |

Refs missing from the export are never deleted. Every change, including the
refs saved aside, is applied in one `git update-ref --stdin` transaction. A
journal is written first to
`<git-dir>/forkmesh-offline/imports/<importId>.json`. The journal records the
signer, the manifest digest, every decision, and each old and new value.
Import exits `5` when anything was saved aside.

```sh
tools/forkmesh_offline_sync.py rollback ~/src/project <importId>
```

Rollback reverses the transaction, verifying each ref still holds the value the
import wrote. If anything moved since, it changes nothing and exits `3`.
Rolling back twice is a no-op. Objects that were unpacked stay in the object
store; they are unreferenced and harmless.

## Provenance and trust

- Manifests are canonical JSON (sorted keys, no whitespace). The signature is
  Ed25519 over `forkmesh-offline-media-v1\n` followed by the manifest's SHA-256
  hex. Signing uses `cryptography` when it is installed, otherwise a pure-Python
  RFC 8032 implementation. Verification is pure Python, so it needs no
  dependencies on an air-gapped machine.
- `verify` and `import` report the signer's public key and whether it matched a
  `--trusted-key`. Import accepts an unpinned signature only with
  `--allow-unpinned`, which still requires a valid signature.
- `fetch` checks the signature before it downloads any bundle bytes. With
  `--trusted-key` it also refuses an untrusted signer.
- The signing key file is a base64url 32-byte seed, created with mode `0600`.
  Keep it off the removable media.

## Telemetry

`export`, `fetch`, `import`, and `rollback` take `--telemetry FILE` and append
one `forkmesh-offline-telemetry-v1` JSON line per run. The line holds the
outcome, the pause reason, the counters above, conflict and changed-ref counts,
omitted bytes, and scope. It never holds repository names, URLs, ref names, or
keys. Nothing is sent anywhere. These lines are the evidence for resume
success, bytes saved (`bytesResumed` plus export `omittedBytes`), integrity
failures, and reconciliation conflicts.

## Exit codes

| Code | Meaning |
| --- | --- |
| `0` | Done. |
| `2` | Usage error or unreadable input. |
| `3` | Verification, integrity, or missing-object failure; nothing was changed. |
| `4` | Paused with the checkpoint kept (`budget_exhausted`, `outside_schedule`, `cancelled`, `link_down`); run the same command to resume. |
| `5` | Imported, with conflicts saved aside for review. |

## Failure paths and recovery

| Situation | Result | Recovery |
| --- | --- | --- |
| Link drops mid-chunk, repeatedly | Resumes from the next missing byte | Automatic |
| Link down longer than `--retries` | `4` / `link_down` | Re-run `fetch` |
| Corrupted bytes on the link | Chunk re-fetched, `integrityFailures` counted | Automatic |
| Source export replaced or persistently corrupt | `failed` / `integrity` | `fetch --restart` |
| Crash or power loss during fetch | Checkpoint and chunks re-checked on the next run | Re-run `fetch` |
| Host ignores `Range` | `3`, no bundle bytes accepted | Use a host with `Range` support, or copy the directory |
| Tampered manifest, bundle, or wrong signer | `3`, repository untouched | Obtain a fresh export and confirm the key |
| Incremental export without its base | `3`, prerequisites listed | Import the earlier export first |
| Diverged branches or moved tags | `5`, local kept, incoming aside | Review and merge from `refs/forkmesh-offline/conflicts/…` |
| Wrong import | — | `rollback REPO IMPORT_ID` |

## Not covered yet

- Hosts do not yet publish `replicaScope` in their catalog records. The Worker
  passes it through and the availability proof honours it when present.
- Ref deletions are not propagated.
- Discovering other hosts on a local network is handled by the desktop app's
  LAN mesh. This tool moves data between them once you know where they are.
- The desktop and mobile apps do not yet expose these commands.

## Evidence

- `app/tests/test_offline_sync.py` runs against real Git repositories and a
  fault-injecting HTTP host. It covers repeated disconnects with byte-exact
  no-duplicate accounting, restarts under data budgets, lost checkpoints,
  corrupted link bytes, a link that stays down, hosts that ignore `Range`,
  cancellation, schedules, rate limits, all four limited modes and their
  advertisements, incremental prerequisites, conflicts, checked-out branches,
  rollback refusal, exit codes, and count-only telemetry.
- The suite runs in the `offline-sync-conformance` quality gate and in the
  `Collaboration conformance` workflow on Linux, macOS, and Windows.
- `conformance/availability-proof/v1/fixture.json` case
  `limited-replicas-never-verified` pins the availability-proof behaviour.
