# ForkMesh-Owned Git Engine Threat Model

Status: `IMPLEMENTED, AWAITING REVIEW` as Phase G0 design evidence.

## Scope

This threat model covers the ForkMesh-owned Git engine (Swift `FMGit` on iOS, Kotlin `nativeapp.git` on Android), its app-private repository storage, and its HTTPS client transport.

It does not claim to secure later Worker authorization, node identity, key storage, document-provider adapters, agent tools, or server hosting beyond the interfaces and gates they require.

The engine processes attacker-controlled repository data and must be designed as a security boundary.

## Trust boundaries

| Component or input | Trust level | Required treatment |
| --- | --- | --- |
| Owned engine source, reviewed release artifact, and resource policy | Trusted after review | Keep dependency-free, reproducibly tested, and versioned. |
| Platform app-private root selected by the adapter | Trusted boundary input | Verify once, retain as immutable operation context, and never accept a repository-controlled replacement. |
| Local repository objects, refs, index, config, packs, and journal | Potentially corrupted or attacker-controlled | Parse with the same bounds and checks as remote data. |
| Remote URL, smart-HTTP advertisements, pkt-lines, packs, sideband, ref statuses, and HTTP headers | Untrusted | Validate, bound, redact, and authenticate before use. |
| HTTPS certificate chain and redirect target | Untrusted until strict validation | Reject invalid TLS and never follow redirects automatically. |
| Credential provider response | Sensitive trusted input | Keep in memory only for one request and never place it in URLs, logs, config, progress, or journal data. |
| System Git test fixture and smart-HTTP server | Test-only oracle | Keep outside production source and prove no production process fallback. |
| Operating-system filesystem and TLS APIs | Platform dependency | Use through bounded owned adapters and test their failure paths on every supported platform. |

## Assets at risk

| Asset | Security property |
| --- | --- |
| Repository object graph and history | Integrity and recoverability. |
| Canonical ref intent and remote-tracking refs | Compare-and-swap integrity and stale-update safety. |
| Worktree files | Root confinement, no overwrite outside selected repository, and no silent corruption. |
| Credentials, bearer sessions, private keys, and certificate decisions | Confidentiality and scoped use. |
| Device storage, memory, battery, and network quota | Bounded consumption under hostile inputs. |
| Operation state and recovery journal | Crash consistency without secret or source leakage. |
| Capability advertisement truthfulness | No claim of clone, commit, fetch, or push before its gate passes. |

## Attacker goals and mitigations

| Threat | Attack surface | Required mitigation | Residual risk | Release-blocking condition |
| --- | --- | --- | --- | --- |
| Path traversal or absolute write | Tree paths, index paths, checkout request, config worktree. | Component-by-component portable path validation and staging-root containment. | Platform canonicalization behavior. | Any traversal or root-escape corpus failure. |
| Symlink escape | Existing worktree entries, tree mode `120000`, staging directory. | Never traverse repository-controlled symlinks and verify parents before each write. | Platform race between checks and writes. | A demonstrable time-of-check/time-of-use escape. |
| Windows or case-folding overwrite | Unicode names, reserved names, case or normalization collisions. | Portable profile, platform collision check in a staging root, and abort before promotion. | New filesystem normalization edge cases. | Any unexplained platform-collision result. |
| Malformed object or compressed bomb | Loose object header, zlib stream, pack entry. | Checked cursor, declared-size limit, streamed inflate, object hash verification, and total-operation caps. | Denial of service within configured cap. | Allocation from unchecked length or cap bypass. |
| Delta bomb or recursive resolution | OFS_DELTA and REF_DELTA. | Hard depth, output-size, expansion-ratio, base-offset, and cycle checks. | CPU spent below cap. | Unbounded recursion, excessive allocation, or non-termination. |
| Pack or index corruption | Pack header, trailer, CRC, index fanout, offsets. | Verify signatures, counts, bounds, CRC, pack checksum, index checksum, and OID ordering. | Unknown future format variants. | Corrupt corpus accepted or valid object exposed before verification. |
| Ref injection or lock race | Ref names, packed refs, remote status, concurrent local operation. | Git ref grammar, ForkMesh length cap, lock files, expected-old-OID checks, and fixed transaction order. | External process modifying same app-private root. | A ref changes without matching expected-old-OID or atomic lock behavior. |
| Stale or forced push | receive-pack advertisement and command result. | Expected-old-OID commands, no force push by default, per-ref report-status model, and unchanged-OID test. | Server may ignore unsupported atomic request. | Stale rejection cannot be distinguished from success. |
| Credential disclosure | URL construction, HTTP headers, exceptions, progress, journal, fixtures. | Disallow URL user-info, redact before emission, memory-only request scope, and zero credential logging. | Process memory capture outside app security model. | A secret appears in a persisted or reported artifact. |
| TLS downgrade or MITM | URL scheme, certificate callback, redirect, proxy response. | HTTPS-only URL policy, strict default validation, host-and-certificate-scoped test override, zero redirects, and no trust-all API. | Platform TLS stack defects. | Invalid certificate accepted by default or credential sent to another host. |
| Protocol desynchronization | pkt-line, sideband, HTTP response parsing. | Exact framing, service MIME validation, command capability allowlist, packet count and output caps. | Novel server interoperability differences. | Parser accepts invalid control framing or violates cap. |
| Cancellation leaves valid partial state | Clone, fetch, pack install, index/ref write, checkout, push. | Checkpoints throughout pipeline, quarantine, journal, rollback, and reopen validation. | Disk-full or power loss at OS flush boundary. | Cancelled operation leaves usable partial HEAD, ref, or worktree state. |
| Crash recovery corruption | Journal parsing, lock files, temporary paths, restart. | Bounded journal schema, idempotent recovery only, read-only fail-closed state when uncertain. | Manual recovery UX after repeated damage. | Recovery invents a ref or deletes validated state. |
| SHA-1 collision attack | Any SHA-1 object from repository or remote. | Full-frame typed screening uses the optional pinned SHA1DC detector and fails closed with `hashScreeningUnavailable` for untrusted transport without a clear detector result. | Detector coverage, corpus rights, and integration review remain incomplete. | Any G5-G7 enablement before corpus evidence, independent cryptography approval, and Android validation. |
| Package or executable fallback | Imports, package manifest, process APIs, platform channel. | No third-party runtime dependencies and source plus runtime no-process guard. | Test harness contamination. | Production source can spawn or load Git implementation. |

## Parser and transport invariants

Untrusted lengths are checked before allocation, slicing, decoding, recursion, decompression, file creation, or multiplication.

All binary parsers use a checked cursor that carries remaining length and returns typed errors rather than a range exception.

All remote and local object IDs are recomputed before trusted use.

All decoder loops advance an input cursor or return an error, which fuzz tests assert through bounded execution.

All network reads honor cancellation, idle deadline, overall deadline, byte limits, and response MIME expectations.

All error text is redacted and bounded before it reaches a log, exception, journal, or progress stream.

All temporary data is rooted under a private operation directory that is deleted on cancellation or failed validation.

## Mobile lifecycle and storage risks

Mobile process death, storage pressure, app suspension, background-network revocation, and interrupted secure-storage access are expected conditions.

G7 must supply an owned platform adapter that chooses an app-private repository root, excludes repository contents from unsuitable backup paths, reports available storage before a large operation, and stops work when lifecycle policy requires it.

The engine must surface `operationInterrupted` rather than inferring success after a process restart.

Selected document-provider or share exports may copy explicit user-selected files, but they must never expose the mutable canonical repository directory as public storage.

Credential providers must be cancellable and unavailable while the application cannot safely display a required user challenge.

## Fuzzing, differential testing, and review requirements

Before G3, every byte parser introduced by G1 or G2 has deterministic malformed corpus cases and property tests for checked length behavior.

Before G5, fuzzing covers loose objects, trees, commits, tags, refs, config, index v2, packs, pack index v2, delta instructions, pkt-lines, sideband, and recovery journals.

Before G5 network enablement, the SHA-1 collision-screen implementation has published collision vectors, adversarial regression corpus, and independent cryptography review.

Before G6 push enablement, an independent review validates protocol command framing, expected-old-OID handling, report-status parsing, and cancellation cleanup.

Before G7 capability advertisement, iOS and Android evidence covers storage, TLS, credentials, cancellation, disk limits, and restart recovery.

Before general production release, G9 requires independent pack/protocol/security review and a software bill of materials demonstrating that no third-party Git runtime dependency is shipped.

## Residual risks and user-visible policy

The first release intentionally trades broad Git feature coverage for bounded behavior and explicit unsupported errors.

A repository requiring SHA-256, LFS, SSH, submodules, linked worktrees, partial clone, hooks, or advanced merge behavior must be rejected or capability-gated instead of silently receiving a lossy approximation.

The application must not claim that a mobile node can clone, author, or push until the corresponding G-phase, authorization, and mobile-runtime gates are all passed.

Any user-visible recovery action must preserve a diagnostic copy and never silently delete a repository after an interrupted operation.
