# Stock Git conformance operations

The public [compatibility matrix](../git-compatibility.md) is generated from
the claims in `conformance/git/v1/matrix.json` and the retained run reports in
`conformance/git/v1/evidence/` (indexed by `conformance/git/v1/results.json`).
Never hand-edit the generated Markdown, HTML, JSON, results index, or evidence;
change the source or a probe, then run:

```sh
python3 tools/git_conformance.py validate
python3 tools/git_conformance.py render
python3 tools/git_conformance.py render --check
```

## Published status

A cell marked `supported`, `local`, or `conditional` keeps that status only
when a retained run covering it passed within `suite.freshnessDays` (30 days).
Otherwise it is published as `failing` (a covering run failed), `stale` (the
newest pass is too old), or `unverified` (no covering run). Only smart-HTTP
runs count for the relay and mirror-failover columns. The hosted page re-checks
run dates against the reader's clock, and
`/docs/git-compatibility/evidence/<run>.json` holds each raw report.

## Attesting evidence

```sh
python3 tools/git_conformance.py attest \
  artifacts/git-conformance-v0.json artifacts/git-conformance-v2.json
```

`attest` validates reports (suite version, operation inventory, summary
consistency, privacy flags, and a scan for unredacted paths, file URLs, or
loopback addresses), stores them by content address, and regenerates outputs.
The newest report per client configuration replaces older ones. Bumping
`suite.version` invalidates existing evidence. `attest --drop <run-id>` retires
a configuration.

## Release gate

```sh
python3 tools/git_conformance.py gate
python3 tools/git_conformance.py gate --operations-only \
  --report artifacts/git-conformance-v2.json
```

`gate` exits non-zero and names every failing, stale, or unverified claim.
`--report` also counts a fresh unattested run; `--operations-only` skips
hand-recorded client rows. The `release` quality-gate profile runs the full
gate.

## Evidence chain

The conformance workflow runs Git protocol v0 and v2 on Linux, macOS, and
Windows. Protocol v2 proves partial clone and lazy fetch; protocol v0 proves the
safe rejection without relaxing hidden-object policy. Each matrix cell maps to a
stable probe ID, and each report records the repository commit, exact
Git/Python/OS versions, per-operation status and time, generalized
origin/proxy counts, the signed fixture commit, and a digest of the final refs.

The offline smart-HTTP fixture exercises direct read/write, a masked relay
proxy, and failover from a failed origin to a matching mirror. Gateway tests
separately prove that HTTPS receive-pack returns an actionable 501, SSH
receive-pack enforces authorization and quotas, upload-pack serves shallow and
partial clones while rejecting hidden-object wants, object bytes survive every
read path, and damaged refs or packs fail strict fsck.

With `--transport auto` the suite selects HTTP only after a completed loopback
round trip (IPv4, then IPv6) and otherwise falls back to a clearly labelled
file-transport run that does not count as relay evidence. The public workflow
requires `--transport http`. CI runs each suite in a private network namespace
(`unshare -rn`) when possible so host firewalls cannot drop loopback traffic.

## Promoting a client row

Run the suite with the exact Git executable a client ships, for both protocol
versions, then attest the reports:

```sh
python3 tools/git_conformance.py run \
  --git /path/to/client/bundled/git \
  --client-label "Client name and version" \
  --transport http --protocol-version 2 \
  --output artifacts/git-conformance-client-v2.json
```

A passing bundled-Git probe proves protocol compatibility only; a desktop UI
stays pending until its own authentication, clone, push, and recovery flows are
exercised.

## Failure and recovery

Probes run entirely in temporary directories and never touch production
repositories. Keep a failed report and log, reproduce with the same Git and
protocol version, and attest the failure before a release so the gate blocks
it; a later passing run supersedes it. For a failed production fetch, retry
rather than delete the clone, run `git fsck --full --strict` after corruption,
and create and verify a bundle before force-updating a ref.

## Telemetry and privacy

Reports are retained for 30 days and contain no repository contents, paths,
URLs, credentials, SSH keys, client addresses, or raw Git stderr: only
generalized request and timing counts.

## Rollout and rollback

Partial-clone filtering and protocol-v2 forwarding are gateway transport
policies requiring no repository migration. Deploy them through the normal
packages, validate the scoped `AcceptEnv GIT_PROTOCOL` SSH drop-in with
`sshd -t`, then run the conformance workflow. Rollback removes
`uploadpack.allowFilter=true`, protocol-v2 forwarding, and the SSH hint;
existing clones are unaffected, and the matrix must change with the contract.
