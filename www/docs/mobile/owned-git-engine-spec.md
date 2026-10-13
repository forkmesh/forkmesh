# ForkMesh-Owned Git Engine Specification

Status: `IMPLEMENTED, AWAITING REVIEW` as Phase G0 design evidence.

## Purpose and non-negotiable boundary

This specification defines the first compatible subset for the ForkMesh-owned Git engine.

The engine is written and maintained by ForkMesh in Swift for the iOS app (`native/ios/ForkMeshCore/Sources/FMGit`) and in Kotlin for the Android app (`native/android/core/src/main/kotlin/com/forkmesh/nativeapp/git`).

The runtime dependency boundary is owned ForkMesh code and the platform's standard libraries only: Foundation, the system zlib and the pinned SHA1DC detector on iOS, and the JDK on Android.

The engine must not depend on libgit2, git2dart, libgit2dart, JGit, go-git, Dulwich, isomorphic-git, gix, a Git executable, Termux, a shell, or a process fallback.

System Git is permitted only outside production-engine sources as a fixture generator, disposable smart-HTTP server, and interoperability oracle.

An earlier libgit2 experiment remains behavioral evidence only.

It is not a runtime dependency, an implementation template, or a production fallback.

This document uses **normative Git behavior** for byte-level Git interoperability requirements.

It uses **ForkMesh policy** for deliberate compatibility limits, security limits, and release gates.

## Compatibility target

### Repository format

The first release opens and creates non-bare SHA-1 repositories with `core.repositoryformatversion = 0`.

The working tree is app-private and its Git directory is `<worktree>/.git`.

The engine rejects `core.bare = true`, linked worktrees, `core.worktree`, object alternates, grafts, replace refs, and repository extensions other than an absent or explicit `extensions.objectFormat = sha1`.

An encountered SHA-256 repository returns `unsupportedRepositoryFormat` without mutating disk.

The engine reads loose objects and pack files, but it creates loose objects in G1 and packs only after G4 passes its interoperability gate.

The engine recognizes only object IDs with 40 lowercase hexadecimal characters in the first release.

### Supported object and repository data

| Area | First support | Initial behavior |
| --- | --- | --- |
| Blob | G1 | Read and write verified loose objects. |
| Tree | G1 read, G2 write | Parse canonical entries and write only supported file modes. |
| Commit | G1 read, G2 write | Parse commit headers and create UTF-8 commits. |
| Annotated tag | G1 parse | Parse for object traversal, with tag creation deferred. |
| Loose refs and symbolic `HEAD` | G1 | Read and atomically update supported local refs. |
| Packed refs | G1 read | Read normal and peeled entries, with rewrite deferred. |
| Reflogs | G2 | Append and read local `HEAD` and `refs/heads/*` logs only. |
| Config | G1 | Read and write the restrictive repository-local subset defined below. |
| Index | G2 | Read and write version 2 only. |
| Pack and index | G3 read, G4 write | Read pack v2 and index v2, then write non-delta packs and index v2. |

The normative repository layout is [gitrepository-layout](https://git-scm.com/docs/gitrepository-layout).

### Refs, reflogs, and config subset

The engine accepts direct and symbolic loose refs under `refs/heads/`, `refs/remotes/`, and `refs/tags/`.

It accepts `HEAD` only as a symbolic reference to `refs/heads/<name>` or as a detached 40-hex object ID after checkout.

It validates every accepted ref using the restrictions in [git-check-ref-format](https://git-scm.com/docs/git-check-ref-format) plus the ForkMesh path limits in this specification.

Packed refs are read as immutable snapshot data and are never rewritten in the first release.

The engine writes a loose ref when a local branch changes and leaves a packed copy shadowed according to normal Git ref precedence.

Local ref writes use lock files and same-directory atomic rename semantics aligned with [git-update-ref](https://git-scm.com/docs/git-update-ref).

The engine appends reflogs only after an index and ref transaction has committed.

It does not read or write reflogs outside `logs/HEAD` and `logs/refs/heads/`.

The repository-local config parser accepts only these keys:

| Section and key | Policy |
| --- | --- |
| `core.repositoryformatversion` | Must be `0`. |
| `core.filemode` | Boolean, defaulting to the platform policy. |
| `core.bare` | Must be false or absent. |
| `extensions.objectformat` | Must be absent or `sha1`. |
| `remote "<name>".url` | HTTPS URL without embedded credentials. |
| `remote "<name>".fetch` | One validated fetch refspec. |
| `branch "<name>".remote` | One validated remote name. |
| `branch "<name>".merge` | One validated `refs/heads/` reference. |

The engine ignores unknown non-security config keys without interpreting them.

The engine rejects config includes, conditional includes, aliases, hooks paths, credential helpers, external diff commands, external merge commands, `core.worktree`, and keys with malformed quoting or continuation syntax.

The parser uses only `.git/config` and never reads global, system, environment, or command-line configuration.

The surrounding Git configuration model is documented by [git-config](https://git-scm.com/docs/git-config).

### Local authoring behavior

G2 supports status, add, remove, branch creation, checkout, and commit for regular files, executable files, symlinks, nested trees, deletions, and valid UTF-8 path names.

Rename detection is intentionally not part of status because Git represents a rename as deletion plus addition in the index.

The engine writes tree modes `100644`, `100755`, and `120000`.

It rejects gitlink mode `160000` because submodules are deferred.

The engine creates commits with UTF-8 author, committer, and message fields and no `encoding` header.

It reads commits containing an `encoding` header as opaque byte messages unless the encoding is UTF-8, in which case it exposes the decoded text.

It rejects NUL bytes and header-injection line breaks in fields it writes.

### Transport behavior

The first network transport is HTTPS smart HTTP only.

The engine never supports `git://`, `file://`, `ssh://`, `http://`, custom schemes, or URL user-info in this release.

The engine requests protocol v2 by sending `Git-Protocol: version=2` with the smart `info/refs` request.

The protocol v2 framing, command model, `ls-refs`, and HTTP negotiation rules are normative as described in [gitprotocol-v2](https://git-scm.com/docs/gitprotocol-v2) and [gitprotocol-http](https://git-scm.com/docs/gitprotocol-http).

The implemented fetch client is v2-only: a v0 or v1 upload-pack advertisement fails with `protocolUnsupported`, and the `allowLegacyProtocolFallback` escape hatch remains reserved and unimplemented.

If that fallback is ever implemented, its default must stay false for a new remote, the decision must be surfaced in the operation result, and fallback stays prohibited after a redirect, after a certificate challenge rejection, or after any response with an invalid Git service MIME type or malformed pkt-line.

With `Git-Protocol: version=2`, `git http-backend` omits the `# service=` banner from the smart `info/refs` body, so the parser accepts the advertisement with or without the banner wrapping.

Fetch negotiation is stateless: bounded rounds of `have` batches (32 per round, at most 8 rounds by default) that re-send acknowledged common commits, then `done`. Thin packs are never requested; `ofs-delta` and `no-progress` are.

Request authentication is injected through a caller-supplied header provider that observes the final method, URL, and body bytes of each request (the ForkMesh app signs these as device headers); the engine itself embeds no credential or signing logic.

Clone and fetch install downloaded packs into a quarantine directory, validate all hashes and limits, then atomically install the pack, index, refs, and clone-state transition.

Push uses the v0 `receive-pack` advertisement, expected-old-OID ref commands, `report-status-v2` when advertised (`report-status` otherwise, plus `side-band-64k`, `quiet`, and `atomic` when advertised), and a typed result for every requested ref.

Push refuses non-fast-forward and force updates: the client walks local ancestry and rejects with `nonFastForward` before any request body is sent, including when the advertised remote tip is not known locally.

Ref deletion is not expressible through the push API; the caller must explicitly request a destructive ref deletion through a separate API and capability gate after G6.

### Deliberately deferred compatibility

The following features are not silently approximated.

| Feature | Status and behavior |
| --- | --- |
| SHA-256 repositories | Rejected as unsupported until a separately reviewed roadmap phase. |
| Shallow clone | Deferred; a requested clone depth fails with a typed `unsupportedRepositoryFormat` error. |
| Partial or promisor clone | Rejected. |
| Git LFS | Rejected as unsupported. |
| SSH transport and SSH host keys | Rejected as unsupported. |
| Submodule update and gitlinks | Rejected as unsupported. |
| Linked worktrees | Rejected as unsupported. |
| Merge, rebase, cherry-pick, and conflict index stages | Deferred to G8. |
| Hooks and credential helpers | Never executed by the engine. |
| Signed commit and tag verification | Deferred behind a separate cryptographic review. |
| Server-side upload-pack and receive-pack hosting | Deferred until client behavior is stable. |
| Multi-pack index, commit graph, sparse index, split index, fsmonitor, and untracked cache | Ignored only where Git permits, otherwise rejected as unsupported. |

## Canonical bytes and object identities

### Object framing and OID calculation

For a supported loose or reconstructed packed object, the canonical bytes are the exact ASCII header `<type> <decimal-size>`, one NUL byte, and the raw body bytes.

The decimal size is the unsigned decimal body-byte count with no sign, whitespace, or leading zero except the single digit `0`.

The SHA-1 object ID is the lowercase hexadecimal encoding of the 20-byte digest of those complete canonical bytes.

The parser verifies the declared size before allocating the body and verifies the object ID before exposing a remote or packed object to callers.

This is normative Git object behavior and is compatible with the format described by [gitformat-pack](https://git-scm.com/docs/gitformat-pack).

### Tree encoding and ordering

A tree entry is encoded as ASCII decimal mode bytes, one space, raw filename bytes, one NUL byte, and the 20-byte object ID.

The parser rejects empty names, NUL in a name, slash in one tree-name component, unsupported modes, duplicate raw names, and an entry order that is not Git tree order.

ForkMesh compares names as unsigned raw bytes, with a directory tree name compared as if a slash byte followed its name.

The writer emits entries in that order and never relies on locale or platform string collation.

The G2 checkout interface accepts only names that decode as valid UTF-8 and meet portable path policy.

It can retain valid but non-materializable tree entries as raw bytes for read-only inspection, but it refuses checkout rather than coercing a filename.

### Commit and tag encoding

The writer emits `tree`, zero or more `parent`, `author`, and `committer` headers in that order, followed by one blank line and a UTF-8 message.

It writes timestamps as integer seconds and offsets as signed four-digit `HHMM` values.

The reader accepts repeated `parent` headers and continuation lines only when the Git header grammar permits them.

It treats unknown headers as opaque bounded bytes and rejects an object that exceeds object or header limits.

Annotated tag parsing accepts `object`, `type`, `tag`, and `tagger` headers plus opaque message and signature bytes.

Tag creation and signature verification are deferred.

### Index version 2

The engine reads and writes only index version 2, whose `DIRC` header, network-byte-order fields, sorted entry order, extension records, and trailing object-format checksum are specified by [gitformat-index](https://git-scm.com/docs/gitformat-index).

It writes no index extensions.

It skips an unknown optional extension only when its first signature byte is uppercase ASCII, the declared extension length is bounded, and the extension lies entirely before the checksum.

It rejects an unknown mandatory extension, an index version other than 2, untrusted stat cache values, invalid stage values, path traversal, duplicate index paths, unsorted entries, and any checksum mismatch.

### Pack and pack-index behavior

G3 accepts only pack version 2 with the `PACK` signature and a bounded network-byte-order object count.

It supports base objects, `OFS_DELTA`, and `REF_DELTA` after validating their header encodings, offsets, base resolution, zlib stream boundaries, and output sizes.

It rejects thin packs until a later dedicated phase provides a bounded fix-up implementation.

It verifies the pack trailer checksum before a pack enters the repository.

It reads only pack index version 2 with the `\377tOc` magic, a monotonic 256-entry fanout table, sorted OIDs, CRC table, offset table, optional large-offset table, matching pack checksum, and matching index checksum.

The normative pack header, delta encoding, trailer, CRC, and index v2 layout are specified by [gitformat-pack](https://git-scm.com/docs/gitformat-pack).

G4 writes non-delta pack version 2 and matching index version 2 before optional bounded delta writing is considered.

### pkt-line and smart HTTP bytes

Every packet begins with exactly four lowercase or uppercase ASCII hexadecimal digits representing its total byte length including the four-byte prefix.

`0000`, `0001`, and `0002` are parsed only as flush, delimiter, and response-end control packets respectively.

All other packets have a total length from 4 through `0xfff0` inclusive.

The engine treats command, capability, ref, and sideband payloads as raw bytes until the specific protocol field requires ASCII or UTF-8 validation.

It does not use line-oriented text readers for packet parsing.

The normative packet-line controls and v2 command sequencing are described by [gitprotocol-v2](https://git-scm.com/docs/gitprotocol-v2).

## Security and resource limits

Limits are part of the protocol boundary, not tuning suggestions.

The `GitResourceLimits` public value may lower configurable defaults but cannot raise hard caps.

| Resource | Configurable default | Hard cap | Enforcement point |
| --- | ---: | ---: | --- |
| pkt-line total length | 65,520 bytes | 65,520 bytes | Before packet allocation. |
| pkt-lines per response | 50,000 | 200,000 | During protocol parsing. |
| ref name length | 255 bytes | 1,024 bytes | Before ref parsing or writing. |
| refs in one advertisement | 25,000 | 100,000 | During `ls-refs` or advertisement parsing. |
| loose or reconstructed object bytes | 64 MiB | 256 MiB | Before inflate and after object header parse. |
| compressed pack bytes per operation | 512 MiB | 1 GiB | While streaming HTTP response to quarantine. |
| total inflated pack bytes per operation | 768 MiB | 1 GiB | Across every inflate and delta result. |
| delta chain depth | 32 | 50 | Before resolving each delta base. |
| delta expansion ratio | 32:1 | 64:1 | Before and after delta application. |
| tree depth | 64 | 128 | During tree traversal and checkout. |
| entries in one tree | 25,000 | 100,000 | While parsing a tree. |
| total checkout files | 100,000 | 250,000 | Before materialization. |
| total checkout bytes | 512 MiB | 1 GiB | While materializing staging worktree. |
| path component length | 255 UTF-8 bytes | 255 UTF-8 bytes | Before filesystem access. |
| repository-relative path length | 1,024 UTF-8 bytes | 4,096 UTF-8 bytes | Before filesystem access. |
| sideband or progress retention | 1 MiB | 4 MiB | Before event buffering or logging. |
| HTTP advertisement body | 1 MiB | 4 MiB | Before parsing service advertisement. |
| HTTP response headers | 64 KiB | 128 KiB | Before header processing. |
| connect, TLS, or first-byte wait | 15 seconds | 30 seconds | Per HTTP request phase. |
| idle read interval | 30 seconds | 60 seconds | During HTTP body streaming. |
| ordinary operation deadline | 5 minutes | 15 minutes | End-to-end operation clock. |
| user-approved large operation deadline | 15 minutes | 30 minutes | End-to-end operation clock. |

The engine follows zero HTTP redirects automatically.

An HTTP 3xx response is a typed `remoteRejected` result that contains a redacted origin and destination host comparison, never a followed request.

The HTTP client sends no cookies and does not retain an ambient credential cache.

Cancellation is checked before each network read and write, after at most 64 KiB of streamed body data, before each zlib output append, before every delta instruction, before each object hash update, before every checkout file write, and immediately before every transactional rename.

Cancellation produces `cancelled`, deletes transient request buffers, and preserves or restores the pre-operation repository state.

## SHA-1 compatibility and collision policy

SHA-1 object IDs are required for first-release interoperability with ordinary Git repositories.

Plain SHA-1 correctness does not detect chosen-prefix collisions and must never be described as collision-safe.

ForkMesh selects the following phased policy.

1. G1 implements an owned SHA-1 digest and SHA-256 digest abstraction with published test vectors, streaming tests, and cross-checks against test-only system-Git fixture object IDs.
2. G1 exposes an internal `Sha1CollisionScreen` boundary and records whether an object has passed the screening policy.
3. Before G5 accepts a pack or object from an untrusted HTTPS remote, ForkMesh must implement an owned collision-screening algorithm, pass the published SHAttered corpus and a maintained adversarial corpus, and obtain focused cryptography review.
4. Before that gate, local repository creation and trusted deterministic fixture interoperability may exercise SHA-1, but the production application must not advertise clone, fetch, push, canonical ref update, or hosting capability.
5. SHA-256 repository support remains rejected until an independently scoped format and interoperability phase supplies full object, index, pack, protocol, and ref behavior.

G4.5 screens complete canonical Git object frames with a typed bounded contract and optional pinned SHA1DC detector integration, but untrusted transport returns `screeningUnavailable` whenever a clear detector is absent.

G4.5 is blocked pending legally verified collision corpus data, independent cryptography review, and Android runtime evidence before G5-G7 can proceed.

This policy retains owned hashing and reserves generalized collision screening for a separately reviewed implementation rather than relying on an operating-system digest provider.

An OS digest can improve implementation reuse but does not itself supply collision detection and would add platform-specific behavioral variance.

The G5 collision-screening and security-review gate is release-blocking for networked SHA-1 repository operations.

## Worktree and filesystem policy

The engine materializes a checkout only inside an app-private repository root supplied by the platform adapter.

It operates on one validated path component at a time and never passes an unvalidated repository-controlled path directly to filesystem APIs.

It rejects absolute paths, empty components, `.` components, `..` components, NUL, backslash, ASCII control characters, trailing dot or space, Windows reserved device names, and names containing colon on platforms where that is ambiguous.

It rejects a tree whose checkout paths collide after the platform adapter's canonicalization and case-sensitivity checks.

The adapter stages a checkout in a new sibling directory on the same filesystem, verifies that every materialized entry remains below the staging root, then atomically promotes the completed tree.

The engine never follows a repository-controlled symlink while traversing or writing a worktree.

A symlink is materialized as a symlink only after its link payload has passed a no-escape policy, and the parent directory is verified not to be a symlink.

Android, iOS, macOS, and Windows test hosts use the same portable path profile.

The G2 interoperability corpus includes valid non-ASCII UTF-8 names and platform collision cases.

If the platform adapter cannot establish a safe result for a normalization or case-folding collision, checkout fails closed with `pathEscapesWorktree` or `unsupportedRepositoryFormat` and does not promote the staging tree.

## Public API and module boundaries

The public API is intentionally small: `GitRepository` in
`native/ios/ForkMeshCore/Sources/FMGit/Engine/GitRepository.swift` (with
`RemoteOperations.swift`) and
`native/android/core/src/main/kotlin/com/forkmesh/nativeapp/git/repository/GitRepository.kt`:
open or initialize a repository, `status`, `add`, `remove`, `commit`,
`createBranch`, `checkout`, `clone`, `fetch`, `push`, `readTree`, `readBlob`
and `log`.

Clone, fetch and push report progress to a `GitProgressListener`, can be
cancelled through a `GitCancellationToken`, and end in one terminal result or
typed error.

Progress values contain bounded typed counters and redacted host or ref labels only.

`GitCredentialRequest` exposes host, scheme, supported credential kinds, and operation purpose without a URL password or persisted secret.

`GitCertificateChallenge` exposes host, certificate chain bytes subject to the resource cap, and default-validation result.

Callers must explicitly return an accept or reject decision for a certificate challenge.

The public error code taxonomy is exactly `invalidRepository`, `unsupportedRepositoryFormat`, `invalidObject`, `objectHashMismatch`, `invalidTree`, `invalidRef`, `refLocked`, `indexCorrupt`, `packCorrupt`, `packChecksumMismatch`, `deltaDepthExceeded`, `resourceLimitExceeded`, `pathEscapesWorktree`, `symlinkEscape`, `networkUnavailable`, `tlsRejected`, `authenticationFailed`, `protocolUnsupported`, `remoteRejected`, `nonFastForward`, `cancelled`, and `operationInterrupted`.

Callers must not parse message text for control flow.

| Module | Responsibility | Allowed direct dependencies |
| --- | --- | --- |
| `bytes` | Checked cursors, writers, bounded byte utilities, hex, and pkt-length primitives. | Platform standard library only. |
| `crypto` | Owned SHA-1 and SHA-256 streaming digests, collision-screen boundary, and constant-time comparison. | `bytes`, platform standard library only. |
| `objects` | Loose object framing, blob, tree, commit, tag, and object-ID verification. | `bytes`, `crypto`, platform standard library only. |
| `refs` | Ref validation, symbolic refs, packed-refs reading, locks, reflogs, and transactions. | `bytes`, `crypto`, platform standard library only. |
| `index` | Index v2 parse, write, checksum, and index transaction. | `bytes`, `crypto`, `objects`, platform standard library only. |
| `worktree` | Portable path policy, status, staging, checkout, and filesystem confinement. | `bytes`, `objects`, `index`, `refs`, platform standard library only. |
| `pack` | Pack and index parse, zlib stream limits, delta resolution, pack write, and quarantine validation. | `bytes`, `crypto`, `objects`, platform standard library only. |
| `protocol` | pkt-line, v2 and approved legacy negotiation, advertisements, and report-status parsing. | `bytes`, `objects`, `refs`, platform standard library only. |
| `transport` | HTTPS request streaming, TLS policy, credential challenge, certificate challenge, and redaction. | `bytes`, `protocol`, platform standard library only. |
| `repository` | Public orchestration, operation lifecycle, recovery journal, and cross-module transactions. | All owned modules only. |
| `security` | Limits, path policy, redaction, cancellation, operation journal, and policy decisions. | `bytes`, platform standard library only. |

Binary parsers, FFI handles, native symbols, and platform package APIs are never public engine APIs.

The core engine has no UI-framework dependency.

Mobile secure storage and document-provider adapters belong to G7 outside the core package.

## Atomicity, cleanup, and recovery

Every mutating operation writes a bounded journal record in `.git/forkmesh-journal/` before installing state.

The journal contains operation kind, random operation ID, staging paths relative to the Git directory, expected old refs, intended new refs, and a commit state.

It contains no credential, URL user-info, authorization header, certificate body, source content, or unbounded remote output.

New loose objects and packs are written to private temporary paths, flushed, hash-verified, and renamed only after complete validation.

Index, config, reflog, and ref changes are written to same-directory lock files, flushed, and renamed in a fixed order recorded by the journal.

On open, recovery validates the journal and either removes an uncommitted staging area or completes only an idempotent rename whose preconditions still hold.

If preconditions cannot be proved, the repository opens read-only with `operationInterrupted` until an explicit recovery action preserves a diagnostic copy.

Partial clone and fetch downloads never create a valid-looking `HEAD` or advertised remote-tracking ref.

## Test and review strategy

Critical parser, crypto, and protocol tests are required because they establish data integrity and security boundaries rather than exercising boilerplate.

G1 uses published SHA-1, SHA-256, zlib, hex, object-framing, tree-order, and ref-validation vectors.

G2 uses bidirectional system-Git fixtures for text, binary, executable, symlink, deletion, empty tree, nested tree, and non-ASCII paths.

G3 and G4 use packs produced by multiple system-Git versions, malformed pack and index corpora, delta bombs, checksum corruption, and cancellation during stream processing.

G5 and G6 use disposable smart-HTTP remotes with wrong credentials, correct disposable credentials, invalid TLS, host-scoped test trust, redacted errors, server rejection, stale push, and cancellation.

Every remote result is compared with the independently observed system-Git oracle only in test harnesses.

Fuzz and property tests target byte cursors, object headers, tree entries, refs, index entries, pack headers, delta instructions, pkt-lines, config values, and recovery journals.

The test harness has a source and runtime guard that fails if any production engine source imports process-spawning APIs or invokes a shell, Git executable, Termux, or a Git implementation package.

No phase advances on passing happy-path tests alone.

Each phase requires its listed interoperability gate, malformed-input corpus result, cancellation evidence, resource-limit evidence, and focused security review.

## G1 entry criteria

G1 may start only after this specification, the compatibility matrix, and the threat model contain no unresolved placeholders or contradictory scope.

G1 must create a package with no third-party runtime dependencies.

G1 must implement no network transport, index, worktree materialization, or pack parsing beyond the exact G1 scope.

G1 must add owned SHA-1 and SHA-256 digest abstractions, published vectors, object-framing tests, ref transaction tests, malformed-input tests, and a no-process-fallback guard before claiming local repository interoperability.
