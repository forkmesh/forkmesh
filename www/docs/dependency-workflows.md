# Stable repository identity and dependency workflows

A build that depends on source mirrored through ForkMesh should keep working,
and keep installing exactly the same bytes, when the hosting provider changes or
the original host disappears. This guide defines the identity and pinning rules
that make that true, and gives tested recipes for stock Git, pip, npm, Go, and
Cargo. No client is modified and no ForkMesh tool is needed at install time.

The contract is `conformance/dependencies/v1/contract.json`. The reference
rules are `app/src/stable_repository.py`, and
`tools/dependency_conformance.py` proves every recipe with unmodified clients.

## Identity: what a dependency really names

| Concept | Value | Changes when the repository moves? |
| --- | --- | --- |
| Logical identity | `urn:forkmesh:repository:sha256:<64 hex>` | No |
| Pinned content | A full 40- or 64-hex commit object ID | No |
| Routes | `https://HOST/OWNER/REPOSITORY`, `https://HOST/OWNER/REPOSITORY.git`, `ssh://git@HOST/OWNER/REPOSITORY.git` | Yes, and that is fine |

- The **logical identity** is protocol requirement FM-ID-001 applied to the
  verified root `git-root-commit:<root commit>`. The root commit comes from the
  account-key-signed catalog record. Every faithful mirror of a repository has
  the same root commit, so every mirror has the same identity.
- Hosts, owners, organization aliases, repository names, and URLs are routing
  hints only. Renaming, transferring, or re-hosting a repository never changes
  its identity.
- A record with no root commit (an empty repository, or a record from an old
  client) has no identity. ForkMesh returns an empty string rather than
  inventing one from a name, because a name-based identity would change when
  the repository moved.

The public [repository index](repository-index.md) returns `identity` on every
search result.

Both HTTPS spellings reach the same repository. The `.git` form matters because
Go module paths, Cargo, and the published SSH URL all use it.

## Pinning rule

A dependency is pinned by **identity plus full commit object ID**. Branch names,
tags, and abbreviated IDs are routing conveniences, not pins:

- The resolver rejects a branch, tag, or short ID with `mutable_pin`.
- It rejects a malformed identity with `identity_invalid`.
- It rejects a pinned ref outside `refs/heads/` or `refs/tags/` with `ref_invalid`.

Git object IDs are content hashes, so a client that holds a full commit ID
detects substituted content on its own. The route rules below make sure that
failure is visible and explained, not silent.

## Switching providers without editing a lock

Write manifests and locks against one **logical base URL**, for example the
URL you first depended on. Then point that base at whichever provider is active
with Git's standard URL rewrite:

```sh
git config --global url."https://ACTIVE-PROVIDER/".insteadOf "https://LOGICAL-HOST/"
```

Every recipe below runs Git under the hood, so this one line moves all of them
together. Lock files keep naming the logical URL and the same commit, so they
stay byte-for-byte identical. To roll back, restore the previous `insteadOf`
line; no manifest or lock changes. In CI, set the rewrite in the job
environment (`GIT_CONFIG_GLOBAL` or `git config --global`) rather than in the
repository.

## Recipes

Each recipe shows the manifest line and the lock evidence the probe compares
across providers. `LOGICAL_URL` is `https://LOGICAL-HOST/OWNER/REPOSITORY.git`,
and `COMMIT` is the full commit object ID.

### Git

```sh
git clone LOGICAL_URL dependency
git -C dependency checkout --detach COMMIT
```

Lock evidence: the checked-out commit (`git rev-parse HEAD`).

### Git submodule

```sh
git submodule add LOGICAL_URL deps/dependency
git -C deps/dependency checkout --detach COMMIT
git add deps/dependency && git commit -m "Pin dependency"
```

Consumers run `git clone --recurse-submodules`. Lock evidence: the gitlink
object ID in the superproject tree. Each submodule URL is its own repository;
see the [Git compatibility matrix](git-compatibility.md#submodule-authorization).

### pip

```text
dependency @ git+LOGICAL_URL@COMMIT
```

Put this line in `requirements.txt` or in `dependencies` in `pyproject.toml`.
Lock evidence: `vcs_info.commit_id` in the installed package's
`direct_url.json`.

### npm

```json
{ "dependencies": { "dependency": "git+LOGICAL_URL#COMMIT" } }
```

Lock evidence: `resolved` for the package in `package-lock.json`, which names
the logical URL and the commit.

### Go modules

```sh
GOPROXY=direct GOPRIVATE=LOGICAL-HOST go get LOGICAL-HOST/OWNER/REPOSITORY.git@COMMIT
```

Lock evidence: the `h1:` module and `go.mod` hashes in `go.sum`. The module
path keeps the `.git` suffix, so Go talks to Git directly and needs no HTML
discovery page. `GOPRIVATE` (or `GONOSUMDB`) is required because the public
checksum database cannot see this host; `go.sum` still verifies every later
download. A tag re-pointed after the lock was written fails with a
`checksum mismatch`.

### Cargo

```toml
[dependencies]
dependency = { git = "LOGICAL_URL", rev = "COMMIT" }
```

Set `CARGO_NET_GIT_FETCH_WITH_CLI=true` so Cargo uses the Git CLI and therefore
the `insteadOf` rewrite. Lock evidence: the `source = "git+LOGICAL_URL?rev=COMMIT#COMMIT"`
line in `Cargo.lock`.

## Fallback: deterministic, visible, verified

The stable HTTPS URL is served by the ForkMesh relay. Before it sends any
response bytes, it checks candidate hosts **in their declared order**, never by
load or latency:

1. The host must answer (`unreachable` otherwise).
2. It must serve the same logical identity (`identity_mismatch` otherwise).
3. Its refs must hash (FM-ID-005) to a repository state the source signed
   (`state_unverified` otherwise).

For a dependency pin, two more checks apply:

4. The host must hold the pinned commit (`commit_missing` otherwise).
5. A pinned ref must still point at that commit (`ref_moved` otherwise).

Every resolution reports a status:

| Status | Meaning |
| --- | --- |
| `resolved` | The first declared route proved the content. |
| `failed_over` | A later route proved it; each skipped route is listed with its reason. |
| `unavailable` | No route proved it (`no_verified_route`). Nothing is served in its place. |
| `rejected` | The pin itself is unusable (for example `mutable_pin`). |

A route that fails any check is never selected. When every route fails, the
build fails with a normal transport error. It never silently falls back to
mutable or unverified content.

## Tested scenarios

`tools/dependency_conformance.py run` builds disposable loopback providers:
an origin, a faithful mirror, a tampered copy whose `main` was rewritten
without being signed, and a retagged copy. It runs each available client
through these scenarios:

| Scenario | Active route | Expected |
| --- | --- | --- |
| `install` | Origin | Installs the pinned commit; records lock and content digests. |
| `provider-switch` | Mirror, through `insteadOf` | Installs; lock and content identical to `install`. |
| `origin-lost` | Relay: lost origin, then mirror | Installs; resolution `failed_over`; lock identical. |
| `unverified-fallback` | Relay: lost origin, then tampered copy | Refused before any bytes; resolution `unavailable` with `state_unverified`. |
| `missing-commit` | Origin, pinned to an absent commit | Every client fails; the resolver reports `commit_missing`. |
| `moved-tag` | Retagged copy | Go fails with a `go.sum` checksum mismatch; the resolver reports `ref_moved`. |
| `rollback-recovery` | Relay: restored origin, then mirror | Installs; resolution `resolved`; lock identical to `install`. |

A client that exits successfully but records a different commit counts as
`substituted`, which always fails the suite.

## Report and telemetry

Each run writes a `forkmesh-dependency-conformance-report-v1` JSON report:
exact client versions, platform, per-scenario outcomes, lock and content
digests, and every relay resolution with its attempts. The summary carries the
primary measures:

| Field | Meaning |
| --- | --- |
| `resolutionSuccess` / `resolutionAttempts` | Relay requests that selected a verified route, out of all relay requests. |
| `failoverSuccess` / `failoverAttempts` | Requests whose first route was unreachable and that still resolved, out of all such requests. |
| `reproducibleLocks` / `lockChecks` | Installs after a provider switch, origin loss, or recovery whose lock and content digests matched the original install. |
| `verificationFailures` | Routes or pins rejected for `state_unverified`, `identity_mismatch`, `commit_missing`, or `ref_moved`, plus Go checksum refusals. |
| `unexpectedResults` | Scenario outcomes that did not match the contract. Must be 0. |
| `unavailableClients` | Selected clients not installed or below the minimum version. |

Reports never contain temporary paths, loopback URLs or ports, file contents,
credentials, or client addresses.

## Supported clients and platforms

| Client | Minimum | Continuous evidence |
| --- | --- | --- |
| Git and Git submodules | 2.34.0 | `dependency-conformance.yml` on Ubuntu, macOS, and Windows |
| pip | 23.0 | same |
| npm | 9.0.0 | same |
| Go modules | 1.21 | same |
| Cargo | 1.70.0 | same; optional locally, required in CI |

Every CI run uploads its report as an artifact. The workflow passes `--require`
for every client, so a runner image that drops a client fails the run instead
of skipping it.

## Run the suite

```sh
python3 tools/dependency_conformance.py validate
python3 tools/dependency_conformance.py run --output artifacts/dependency-conformance.json
python3 tools/dependency_conformance.py run --clients git,pip --require git,pip
python3 -m pytest -q app/tests/test_dependency_workflows.py
```

`run` exits 0 when every scenario matched, 1 on an unexpected result or a
missing required client, and 2 when the probe could not run at all.

## Limits

- HTTPS routes are read-only. Publish changes over SSH; see the
  [Git compatibility matrix](git-compatibility.md#https-receive-pack).
- Public repositories only. Private repositories use owner-sealed replicas and
  have no public route.
- Packages served from a registry (PyPI, the npm registry, crates.io, a Go
  module proxy) are outside this contract. These recipes install directly from
  Git.
- A relay can prove only the states the source signed. The client-side pin (a
  commit ID, `go.sum`, or a lock file) is what protects content once it has
  been accepted.

## Rollout and rollback

The `.git` route spelling and the index `identity` field are additive, and
older clients ignore them. Rolling back the Worker removes both. Existing
`https://HOST/OWNER/REPOSITORY` URLs are unaffected, but a `.git`-suffixed HTTPS
URL would again look up a repository literally named `REPOSITORY.git` and fail.
To change the contract, edit `contract.json`, the reference, and this guide
together until `validate` and the test suite pass.
