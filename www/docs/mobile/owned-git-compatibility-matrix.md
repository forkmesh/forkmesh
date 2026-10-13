# ForkMesh-Owned Git Engine Compatibility Matrix

Status: `IMPLEMENTED, AWAITING REVIEW` as Phase G0 design evidence.

This matrix is structured for direct conversion into machine-readable records without changing its meaning.

`Status` means the intended initial engine behavior, not a claim that the behavior already exists.

`Oracle` names the required external compatibility evidence and is test-only where it refers to system Git.

| Feature | First phase | Initial status | Interoperability oracle | Security gate | Explicit non-goal |
| --- | --- | --- | --- | --- | --- |
| Local non-bare SHA-1 init and open | G1 | Planned | System Git opens ForkMesh-created repository and ForkMesh opens system-Git fixture. | Repository layout, object ID, config, and ref validation. | Bare app repositories. |
| Loose blob objects | G1 | Planned | Bidirectional blob hash and body comparison. | Declared-size, zlib, object-size, and SHA-1 screen policy. | Unbounded blobs. |
| Loose tree objects | G1 read, G2 write | Planned | System Git `cat-file` and ForkMesh tree traversal agree. | Raw-byte ordering, duplicate, traversal, and mode rejection. | Gitlinks and submodule checkout. |
| Loose commit objects | G1 read, G2 write | Planned | Commit OID, parent, tree, author, and message agree. | Header injection and size limits. | Signed commit verification. |
| Annotated tags | G1 parse | Planned | System Git object inspection agrees. | Header and object-type validation. | Tag creation and signature verification. |
| SHA-1 repositories | G1 trusted local, G4.5 security gate, G5 network | Blocked for untrusted input | Published vectors, system-Git object IDs, and pinned SHA1DC adapter tests. | Full-frame fail-closed screening, corpus evidence, independent cryptography review, and Android validation before untrusted input. | Collision-safe claim from ordinary SHA-1 alone. |
| SHA-256 repositories | Later | Rejected | Future full SHA-256 fixture suite. | Complete object, index, pack, and protocol review. | Partial SHA-256 support. |
| Loose refs and symbolic `HEAD` | G1 | Planned | System Git sees created branch and detached HEAD state. | Ref grammar, lock, expected-old-OID, and atomic rename. | Arbitrary ref namespaces. |
| Packed refs read | G1 | Planned | System Git packed fixture resolves same refs and peeled tags. | Checksum-free format bounds, sorting, and duplicate rejection. | Packed-refs rewrite. |
| Reflogs | G2 | Planned | System Git reads local branch and HEAD log entries. | Append-after-commit transaction order and bounded messages. | Remote-tracking reflogs. |
| Repository-local config subset | G1 | Planned | System Git recognizes written remote and branch settings. | No includes, helpers, hooks, or global configuration. | Full Git config language. |
| Index v2 | G2 | Planned | System Git status and ForkMesh status agree on shared fixtures. | DIRC checksum, sort order, stage, path, and extension validation. | Index v3, v4, split, sparse, fsmonitor, and untracked cache. |
| Status and add | G2 | Planned | System Git index and working tree result match. | Worktree confinement and resource limits. | Rename heuristic parity. |
| Remove and delete | G2 | Planned | System Git sees expected deletion commit. | Path validation and atomic index/ref update. | Recursive destructive public API without caller confirmation. |
| Branch and checkout | G2 | Planned | System Git and ForkMesh resolve identical branch trees. | Symlink, traversal, platform collision, and staging-root checks. | Linked worktrees. |
| Commit creation | G2 | Planned | System Git verifies tree and commit OID. | UTF-8 and header controls, atomic index/ref/reflog transaction. | GPG or SSH signing. |
| Pack v2 read | G3 | Planned | System Git fixtures from multiple versions resolve identical objects. | Header, trailer, CRC, checksum, zlib, and total-resource limits. | Pack v3 and thin packs. |
| Pack index v2 read | G3 | Planned | OID lookup matches system-Git pack index. | Fanout, offset, large-offset, and checksum bounds. | Multi-pack index and reverse index. |
| OFS_DELTA and REF_DELTA | G3 | Planned | Reconstructed object IDs match system Git. | Base availability, depth, ratio, range, and allocation limits. | Unbounded delta chains. |
| Pack v2 write | G4 | Planned | System Git verifies and unpacks produced pack. | Streaming hash, cancellation, quarantine, and atomic install. | Delta writing before non-delta gate passes. |
| Pack index v2 write | G4 | Planned | System Git opens generated index. | Sorted OIDs, CRC, offset, and checksum verification. | Reverse index or multi-pack index. |
| pkt-line | G5 | Planned | Disposable smart-HTTP fixture exchanges exact packets. | Four-hex length, control packet, count, and output limits. | Text-line parsing. |
| Protocol v2 `ls-refs` and fetch | G5 | Planned | Disposable Git HTTP server and selected read-only fixture. | Capability allowlist, ref count, sideband, TLS, and pack quarantine. | Unsupported v2 commands. |
| Protocol v0 or v1 fallback | G5 | Opt-in only | Server without v2 and exact expected advertisement. | Same-origin HTTPS, no redirect, explicit caller opt-in, and valid MIME type. | Silent downgrade. |
| HTTPS clone and fetch | G5 | Planned | Disposable smart-HTTP server and mobile platform run in G7. | Strict TLS, host-scoped test trust, credential redaction, cancellation, and resource limits. | HTTP, `file`, Git native, SSH, and process transports. |
| Credential challenge | G5 | Planned | Wrong disposable credential fails typed and correct one succeeds. | No URL user-info, logging, persistence, or redirect forwarding of secrets. | Credential helpers. |
| Certificate challenge | G5 | Planned | Self-signed fixture is rejected by default and scoped test trust succeeds. | Host and certificate pin scope, TLS downgrade rejection, and no global trust mutation. | Broad trust-all policy. |
| Shallow clone | After normal G5 | Deferred | Future system-Git shallow fixture. | Shallow boundary and object reachability review. | Implicit depth handling. |
| Partial or promisor clone | Later | Rejected | Future protocol and promisor corpus. | Missing-object and server-trust review. | Filter negotiation. |
| Smart HTTP push | G6 | Planned | Disposable receive-pack server and system-Git verification. | Expected-old-OID, report-status, per-ref result, authentication, and cancellation. | Implicit force push. |
| Non-fast-forward rejection | G6 | Planned | Competing writer leaves remote OID unchanged after rejection. | Compare-and-swap ref commands and no force capability. | Last-writer-wins. |
| Ref deletion | G6 | Deferred within phase | Disposable receive-pack server. | Separate destructive API, caller confirmation, and explicit capability. | Empty refspec convenience behavior. |
| SSH transport | Later | Rejected | Future SSH fixture. | Host-key, key storage, and agent policy review. | Shell SSH fallback. |
| Git LFS | Later | Rejected | Future LFS server fixture. | Pointer, authorization, and large-object policy review. | Filter or pre-push emulation. |
| Submodules | Later | Rejected | Future nested-repository fixture. | Gitlink, URL, recursion, and trust review. | Automatic recursive update. |
| Linked worktrees | Later | Rejected | Future worktree interoperability fixture. | Cross-directory lock and path policy review. | Partial linked-worktree support. |
| Merge and rebase | G8 | Deferred | System-Git merge and conflict corpus. | Conflict index, recovery, and data-loss review. | Silent conflict resolution. |
| Hooks | Never | Rejected | Not applicable. | No code execution from repository configuration. | Hook compatibility. |
| Credential helpers | Never | Rejected | Not applicable. | Secret confinement and no process execution. | Helper protocol compatibility. |
| Signed commits and tags | Later | Deferred | Future signature corpus and verified key policy. | Independent cryptography and trust review. | Treating signature text as verification. |
| Android and iOS core execution | G7 | Planned | iOS simulator/device and Android emulator/device complete flow. | App-private storage, lifecycle cancellation, secure credential bridge, and quota evidence. | Public repository storage. |
| Desktop test hosts | G1 onward | Planned test-only | macOS and Windows interoperability suites. | Same path, malformed input, and case-collision tests. | Desktop-specific runtime dependency. |

The Phase 0 catalog capability IDs `repo.clone`, `repo.store`, `git.branch`, `git.commit`, and `git.push` remain `planned` until their respective engine and authorization phases have passed their gates.

No matrix row grants a capability merely because a local implementation phase is complete.

The node registration, repository role, capability-grant, and truthful availability checks defined by the version 1 contracts remain independent authorization gates.
