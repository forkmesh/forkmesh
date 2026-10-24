# Stable repository identity and dependency workflows

Builds that depend on Git sources must install the same bytes when the hosting
provider changes or the origin disappears. Contract:
`conformance/dependencies/v1/contract.json`; reference rules:
`app/src/stable_repository.py`; probe: `tools/dependency_conformance.py`.
Stock clients need no ForkMesh tooling.

## Identity and pinning

| Concept | Value | Changes when moved? |
| --- | --- | --- |
| Logical identity | `urn:forkmesh:repository:sha256:<64 hex>` (FM-ID-001 over `git-root-commit:<root>` from the signed catalog record) | No |
| Pinned content | Full 40- or 64-hex commit ID | No |
| Routes | `https://HOST/OWNER/REPOSITORY[.git]`, `ssh://git@HOST/OWNER/REPOSITORY.git` | Yes |

Hosts, owners, aliases, names and URLs are routing hints only. A record with no
root commit has no identity (empty string). The
[repository index](repository-index.md) returns `identity` on every result.

A pin is identity plus full commit ID. The resolver rejects branches, tags and
short IDs (`mutable_pin`), malformed identities (`identity_invalid`) and refs
outside `refs/heads/` or `refs/tags/` (`ref_invalid`).

## Switching providers without editing a lock

Write manifests and locks against one logical base URL and point it at the
active provider:

```sh
git config --global url."https://ACTIVE-PROVIDER/".insteadOf "https://LOGICAL-HOST/"
```

All recipes run Git underneath, so locks stay byte-identical; rollback restores
the previous line. `LOGICAL_URL` is `https://LOGICAL-HOST/OWNER/REPOSITORY.git`.

### Git

`git clone LOGICAL_URL dep && git -C dep checkout --detach COMMIT` (evidence:
`git rev-parse HEAD`).

### Git submodule

`git submodule add LOGICAL_URL deps/dep`, then check out `COMMIT` (evidence: the
gitlink in the superproject tree).

### pip

`dependency @ git+LOGICAL_URL@COMMIT` (evidence: `vcs_info.commit_id` in
`direct_url.json`).

### npm

`"dependency": "git+LOGICAL_URL#COMMIT"` (evidence: `resolved` in
`package-lock.json`).

### Go modules

`GOPROXY=direct GOPRIVATE=LOGICAL-HOST go get LOGICAL-HOST/OWNER/REPOSITORY.git@COMMIT`
(evidence: `go.sum` hashes; a moved tag fails with a checksum mismatch).

### Cargo

`dependency = { git = "LOGICAL_URL", rev = "COMMIT" }` with
`CARGO_NET_GIT_FETCH_WITH_CLI=true` (evidence: `source` line in `Cargo.lock`).

## Fallback

The relay checks candidate hosts in declared order before sending any bytes. A
route is skipped as `unreachable`, `identity_mismatch`, `state_unverified`
(refs do not hash to a signed state), `commit_missing` or `ref_moved`.
Resolution status:

| Status | Meaning |
| --- | --- |
| `resolved` | The first route proved the content. |
| `failed_over` | A later route proved it; skipped routes are listed with reasons. |
| `unavailable` | No route proved it; nothing is served. |
| `rejected` | The pin itself is unusable. |

There is never a silent fallback to unverified or mutable content.

## Scenarios

`tools/dependency_conformance.py run` builds loopback providers (origin, mirror,
tampered copy, retagged copy) and runs every client through:

| Scenario | Expected |
| --- | --- |
| `install` | Installs the pinned commit. |
| `provider-switch` | Installs via `insteadOf`; lock identical. |
| `origin-lost` | `failed_over` to the mirror; lock identical. |
| `unverified-fallback` | Refused with `state_unverified`. |
| `missing-commit` | Fails with `commit_missing`. |
| `moved-tag` | Fails with `ref_moved`. |
| `rollback-recovery` | `resolved` again; lock identical. |

The report (`forkmesh-dependency-conformance-report-v1`) summary carries
`resolutionSuccess`, `resolutionAttempts`, `failoverSuccess`,
`failoverAttempts`, `reproducibleLocks`, `lockChecks`, `verificationFailures`,
`unexpectedResults` (must be 0) and `unavailableClients`. Reports contain no
paths, loopback URLs, contents, credentials or addresses.

```sh
python3 tools/dependency_conformance.py validate
python3 tools/dependency_conformance.py run --output report.json [--clients git,pip --require git,pip]
```

`run` exits 0 on success, 1 on an unexpected result or missing required client,
2 when it cannot run. It only reads: pushes (over SSH, or over HTTPS with an
account credential) are out of scope, and only public repositories and direct
Git installs are covered.
