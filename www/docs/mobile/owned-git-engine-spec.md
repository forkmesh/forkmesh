# ForkMesh-Owned Git Engine Specification

The mobile apps carry their own Git engine: Swift in
`native/ios/ForkMeshCore/Sources/FMGit` and Kotlin in
`native/android/core/src/main/kotlin/com/forkmesh/nativeapp/git`. It treats all
repository and network data as attacker-controlled and is a security boundary.

## Dependency boundary

Runtime code is owned ForkMesh code plus platform standard libraries
(Foundation, system zlib and a pinned SHA1DC detector on iOS; the JDK on
Android). No libgit2, JGit, go-git, other Git library, Git executable, shell or
process fallback. System Git is used only in tests, as fixture generator,
disposable smart-HTTP server and interoperability oracle. A source and runtime
guard fails the build if production code can spawn a process.

## Supported subset

- Non-bare SHA-1 repositories, `core.repositoryformatversion = 0`, app-private
  worktree with `<worktree>/.git`. Bare repositories, linked worktrees,
  `core.worktree`, alternates, grafts, replace refs and extensions other than
  `objectFormat = sha1` are rejected; SHA-256 returns
  `unsupportedRepositoryFormat` without touching disk.
- Objects: blobs, trees, commits; annotated tags parsed. Modes `100644`,
  `100755`, `120000`; gitlinks rejected. Commits are written UTF-8 with no
  `encoding` header.
- Refs: loose refs under `refs/heads/`, `refs/remotes/`, `refs/tags/`, symbolic
  or detached `HEAD`, packed refs read-only. Writes use lock files and atomic
  rename; reflogs only for `HEAD` and `refs/heads/*`.
- Config: only `.git/config`, and only `core.repositoryformatversion`,
  `core.filemode`, `core.bare`, `extensions.objectformat`, `remote.<n>.url`
  (HTTPS, no credentials), `remote.<n>.fetch`, `branch.<n>.remote`,
  `branch.<n>.merge`. Includes, aliases, hooks paths, helpers and external
  commands are rejected.
- Index version 2 only, no extensions written. Pack v2 and index v2 read
  (including `OFS_DELTA` and `REF_DELTA`); non-delta packs written. Thin packs
  rejected.
- Local operations: status, add, remove, branch, checkout, commit, log, read
  tree and blob.

## Transport

HTTPS smart HTTP only; no `git://`, `ssh://`, `http://`, `file://` or URL
user-info. Fetch is protocol v2 only (`Git-Protocol: version=2`) with stateless
negotiation (32 `have` lines per round, at most 8 rounds), no thin packs. Push
uses the receive-pack advertisement with expected-old-OID commands,
`report-status-v2` when offered, and a typed result per ref. Non-fast-forward
and force updates are refused locally before any body is sent; ref deletion is
not expressible. Redirects are never followed. Authentication headers come
from a caller-supplied provider that sees the final method, URL and body; the
engine stores no credentials and sends no cookies. Downloaded packs land in a
quarantine directory and are installed atomically only after every hash and
limit check passes.

## Not supported

SHA-256 repositories, shallow and partial clone, LFS, SSH, submodules, linked
worktrees, merge and rebase, hooks, credential helpers, signature verification,
and server-side hosting. Each fails with a typed error instead of a lossy
approximation.

## Resource limits

Limits are part of the protocol boundary. Callers may lower defaults but never
raise hard caps.

| Resource | Default | Hard cap |
| --- | ---: | ---: |
| pkt-line length | 65,520 B | 65,520 B |
| pkt-lines per response | 50,000 | 200,000 |
| refs per advertisement | 25,000 | 100,000 |
| object size | 64 MiB | 256 MiB |
| compressed pack per operation | 512 MiB | 1 GiB |
| inflated pack per operation | 768 MiB | 1 GiB |
| delta chain depth | 32 | 50 |
| delta expansion ratio | 32:1 | 64:1 |
| tree depth / entries per tree | 64 / 25,000 | 128 / 100,000 |
| checkout files / bytes | 100,000 / 512 MiB | 250,000 / 1 GiB |
| path component / path length | 255 B / 1,024 B | 255 B / 4,096 B |
| connect or first byte / idle read | 15 s / 30 s | 30 s / 60 s |
| operation deadline | 5 min | 15 min (30 min if approved) |

Cancellation is checked before every network read and write, every 64 KiB of
body, every inflate append, delta instruction, file write and rename. A
cancelled operation leaves the repository in its prior state.

## SHA-1 policy

Plain SHA-1 is not collision-safe. Complete object frames are screened through
the pinned SHA1DC detector; for untrusted transport the engine fails closed
(`hashScreeningUnavailable`) whenever a clear detector result is unavailable.
Network SHA-1 operations stay gated on collision-corpus evidence and
independent cryptography review.

## Worktree and filesystem policy

Checkout happens only inside an app-private root, one validated path component
at a time. Absolute paths, empty, `.` or `..` components, NUL, backslash,
control characters, trailing dot or space, reserved device names and
platform-ambiguous colons are rejected, as are case or normalization
collisions. A checkout is built in a sibling staging directory, verified to
stay below its root, then promoted atomically. Repository-controlled symlinks
are never followed; a symlink is written only if its target cannot escape.

## API and errors

`GitRepository` exposes open/init, `status`, `add`, `remove`, `commit`,
`createBranch`, `checkout`, `clone`, `fetch`, `push`, `readTree`, `readBlob`
and `log`. Network operations report bounded, redacted progress to a
`GitProgressListener` and honour a `GitCancellationToken`. Errors are typed:
`invalidRepository`, `unsupportedRepositoryFormat`, `invalidObject`,
`objectHashMismatch`, `invalidTree`, `invalidRef`, `refLocked`, `indexCorrupt`,
`packCorrupt`, `packChecksumMismatch`, `deltaDepthExceeded`,
`resourceLimitExceeded`, `pathEscapesWorktree`, `symlinkEscape`,
`networkUnavailable`, `tlsRejected`, `authenticationFailed`,
`protocolUnsupported`, `remoteRejected`, `nonFastForward`, `cancelled`,
`operationInterrupted`. Callers never parse message text.

## Atomicity and recovery

Every mutation writes a bounded journal in `.git/forkmesh-journal/` (operation,
staging paths, expected old and new refs) with no credentials or content.
Objects and packs are written to private temporary paths, verified, then
renamed; index, config, reflog and ref changes go through lock files in a fixed
order. On open, recovery removes uncommitted staging or completes an idempotent
rename; if preconditions cannot be proven, the repository opens read-only with
`operationInterrupted`. A partial clone never produces a valid-looking `HEAD`.

## Threat model

| Threat | Mitigation |
| --- | --- |
| Path traversal, symlink escape, case-folding overwrite | Component validation, staging-root containment, no symlink traversal |
| Malformed objects, zlib or delta bombs | Checked cursors, declared-size checks, streamed inflate, depth and ratio caps, hash verification |
| Pack or index corruption | Signature, count, CRC, checksum and ordering checks before use |
| Ref injection, lock races, stale push | Ref grammar, lock files, expected-old-OID commands, no force |
| Credential disclosure | No URL user-info, redaction, request-scoped memory only |
| TLS downgrade or redirect to another host | HTTPS only, strict validation, zero redirects, no trust-all API |
| Protocol desync | Exact pkt-line framing, MIME checks, capability allowlist, count caps |
| Crash or cancellation mid-operation | Quarantine, journal, idempotent recovery, read-only fail-closed |
| SHA-1 collision | Full-frame screening, fail closed without a detector result |
| Executable fallback | No third-party runtime dependencies, process guard |

Interrupted operations surface `operationInterrupted` rather than inferred
success, and recovery preserves a diagnostic copy instead of deleting a
repository.

## Testing

Published vectors for SHA-1, SHA-256, zlib, framing, tree order and refs;
bidirectional fixtures against system Git for local authoring; packs from
several Git versions plus malformed corpora, delta bombs and cancellation;
disposable smart-HTTP remotes for credentials, TLS, rejection and stale push;
fuzz and property tests for every parser.
