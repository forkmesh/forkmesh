# Stock Git conformance operations

The public [compatibility matrix](../git-compatibility.md) is generated from
two sources: the claims in `conformance/git/v1/matrix.json` and the retained run
reports in `conformance/git/v1/evidence/`, indexed by
`conformance/git/v1/results.json`. Do not hand-edit the Markdown, hosted HTML,
hosted JSON, the results index, or an evidence file. Change the versioned
source, add or update an executable probe, then run:

```sh
python3 tools/git_conformance.py validate
python3 tools/git_conformance.py render
python3 tools/git_conformance.py render --check
```

## Published status

No claim is published on its own authority. Each operation/path cell whose
matrix status is `supported`, `local`, or `conditional` is published with that
status only when a retained run covers the cell and passed within
`suite.freshnessDays` (30 days) of the attestation date. Otherwise the cell is
published as:

- `failing` — any retained run covering the cell failed or did not run it;
- `stale` — the newest passing run is older than the freshness window; or
- `unverified` — no retained run covers the cell.

Only smart-HTTP runs count for the relay and mirror-failover columns; a
file-transport oracle run verifies the direct column only. A hand-recorded
client certification (`"status": "tested"`) degrades to `stale` the same way.
The hosted page is static, so it re-checks every row's last-run date against
the reader's clock and shows `stale` even before the page is regenerated.
`/docs/git-compatibility/results.json` carries every published cell with its
covering run IDs, and `/docs/git-compatibility/evidence/<run>.json` is the raw
report.

## Attesting evidence

`attest` accepts runner reports, passing or failing, validates them (suite
version, operation inventory, summary consistency, privacy flags, and a scan for
unredacted paths, file URLs, or loopback addresses), stores them under content
addresses, and regenerates every public output:

```sh
python3 tools/git_conformance.py attest \
  artifacts/git-conformance-v0.json artifacts/git-conformance-v2.json
```

The newest report per client configuration (label, Git version, platform,
machine, protocol, and transport) replaces older ones, and an older report never
replaces a newer one. The results index is recomputed from the evidence during
`validate`, so editing either file fails CI. Bumping `suite.version` makes
existing evidence invalid until the suite is re-run and attested.

## Release gate

```sh
python3 tools/git_conformance.py gate
python3 tools/git_conformance.py gate --operations-only \
  --report artifacts/git-conformance-v2.json
```

`gate` exits non-zero and names each failing, stale, or unverified support claim
as of today. `--report` also counts a fresh, unattested run, which is how the
public workflow proves its own run backs every claim. `--operations-only` skips
hand-recorded client rows that a suite run cannot refresh; the `release` quality
gate profile runs the full gate, so a stale desktop certification blocks a
release until it is re-tested or its row is changed to `pending`.

## Evidence chain

The `Stock Git conformance` workflow runs Git protocol v0 and v2 on Ubuntu,
macOS, and Windows. Protocol v2 proves the supported partial-clone and lazy
fetch flow; protocol v0 proves the documented safe rejection without relaxing
hidden-object policy. Every matrix cell maps to one stable probe ID. Each job
retains a JSON report with the tested repository commit, exact Git/Python/OS
versions, status and elapsed time for every operation, generalized origin/proxy
counts, the signed fixture commit ID, and a digest of the final refs.

The offline smart-HTTP fixture exercises direct read/write, a masked relay
proxy, and a failed origin followed by a matching mirror. Focused production
gateway tests separately prove that:

- HTTPS receive-pack returns the actionable 501 before consuming a body;
- SSH receive-pack checks authorization, old ref IDs, incoming objects, quotas,
  hidden internal refs, and read-only configuration;
- upload-pack supports shallow clone and protocol-v2 partial clone while
  arbitrary hidden-object wants and protocol-v0 lazy fetches remain rejected;
- signature bytes and object IDs survive each read path; and
- damaged refs and packs fail strict fsck instead of becoming passing evidence.

The native ForkMesh Actions workflow runs the same suite with the runner's
system Git under `--transport auto`. Selecting HTTP requires a completed
loopback round trip, not merely a successful bind: a runner can accept a bind
on `127.0.0.1` and then drop the traffic, which would hang every Git command
until its timeout. IPv4 is probed first, then IPv6, so a runner that only
routes `::1` still produces real relay and failover evidence. Only when no
loopback family answers does the suite fall back to the clearly labeled
file-transport oracle for ref/object semantics; that report is not counted as
relay or failover network evidence. The public cross-platform workflow requires
`--transport http` and fails instead of silently falling back.

The Actions step runs each suite in its own network namespace
(`unshare -rn`, loopback brought up) whenever the sandbox allows it. A node
whose Actions policy enables network access runs steps in the host namespace,
where a host application firewall that queues new connections for a verdict
(OpenSnitch does this with NFQUEUE) can drop one loopback SYN for good: the
Git command then waits out its 90-second timeout and a probe fails while every
fresh connection still works. Run 784 lost `submodules` this way. A private
loopback keeps the smart-HTTP evidence and never meets the host rules. When a
run fails, the job log names each failed probe and its redacted reason, because
the report itself stays inside the disposable sandbox.

## Promoting a client row

Do not infer support from a brand name. Run the suite with the exact executable
shipped by the client:

```sh
python3 tools/git_conformance.py run \
  --git /path/to/client/bundled/git \
  --client-label "Client name and version" \
  --transport http \
  --protocol-version 0 \
  --output artifacts/git-conformance-client-v0.json
python3 tools/git_conformance.py run \
  --git /path/to/client/bundled/git \
  --client-label "Client name and version" \
  --transport http \
  --protocol-version 2 \
  --output artifacts/git-conformance-client-v2.json
```

Attest both reports so they become retained public evidence, record the
application and bundled-Git versions in the source matrix, and link the
evidence in review. A desktop UI remains pending until its authentication, clone/fetch,
push, branch, and recovery flows are exercised in the application itself; a
passing bundled-Git probe establishes protocol compatibility but does not
certify the UI or credential-store integration.

## Failure and recovery

A failed probe leaves all production repositories untouched: every origin,
key, bundle, hook, and corruption fixture lives under a temporary directory.
The report replaces that directory and loopback URLs with generic markers.
Keep a failed report and job log, reproduce with the same Git and protocol
version, and inspect the first failed probe.

Attest the failed report before a release rather than discarding it: the
affected cells are published as `failing` and `gate` blocks the release. Once
the fix lands, re-run the same client configuration and attest the new report;
it supersedes the failure. To retire a configuration that is no longer tested
(for example, an old Git version), run
`python3 tools/git_conformance.py attest --drop <run-id>`; the cells it covered
fall back to whatever other retained runs prove, or to `unverified`.

For a failed or interrupted production fetch, retry before deleting the clone;
Git reuses valid objects. Run `git fsck --full --strict` after any corruption
diagnostic. Before force-updating a production ref, create and verify a bundle.
The rollback probe restores the exact old object ID from that bundle and proves
the recovered ref can be cloned through every read path.

## Telemetry and privacy

Conformance reports are retained for 30 days. They contain no repository
contents, filesystem paths, URLs, credentials, SSH keys, client addresses, or
raw Git stderr. Generalized request and timing counts are sufficient to
calculate pass rate by operation/client/transport and locate a failed stage.
Production mirror counters remain aggregate clone/website totals and bounded
client classes; the conformance runner does not widen that collection.

## Rollout and rollback

The filter capability and bounded protocol-v2 forwarding are command-line and
transport policies on the HTTPS and SSH gateways. They require no repository
migration and do not change object bytes or refs. Roll them out through the
normal mirror-node and Worker packages, install the scoped
`AcceptEnv GIT_PROTOCOL` SSH drop-in after validating the combined
configuration with `sshd -t`, run the protocol-v0 safe-rejection and
protocol-v2 partial-clone hidden-ref tests, then the complete conformance
workflow. Rollback removes `uploadpack.allowFilter=true`, protocol-v2
forwarding, and the scoped SSH environment hint, then redeploys the prior
packages; existing full clones and repositories are unchanged, while new
partial-clone requests safely fail or fall back only when the stock client
permits it. Do not publish that rollback as partial-clone support—the generated
matrix and suite version must change with the contract.
