# ForkMesh product promise: proof and measurement

> ForkMesh lets developers collaborate on, mirror, and preserve source-code repositories across independent hosts—without relying on any single hosting provider.

This is the canonical product promise. Keep the sentence unchanged on primary
product and commercial surfaces. `Your Code, Your World.` is the supporting
slogan, not a replacement promise.

## Four-step proof

Use a disposable **public** repository. The public failover proof does not apply
to a private repository, an unverified endpoint, or a repository with only one
eligible copy. Record the source and mirror node names, the stable clone URL,
the expected commit, and start/end times. Never delete the source repository as
part of this drill.

### 1. Collaborate

Open the repository in the [ForkMesh app](https://app.forkmesh.com/forkmesh/forkmesh/issues)
and create or review a signed issue, discussion, or pull request. Confirm the
record appears from the live repository view. Familiar Git terms—repository,
mirror, host, commit, issue, pull request, and clone—are intentional.

### 2. Add an independent mirror

Use the repository **Mirrors** tab to ask another node to mirror it, then accept
the request on that node. Operators may instead use **Network → Hosts** to add
and deploy a separate host. Wait until the repository profile reports that the
mirror is online, integrity is `ok`, its signed health is fresh, and its commit
matches the expected source commit. A configured or syncing host is not proof.

### 3. Take the source mirror offline

On the managed source host, stop `forkmesh-mirror-node` through its service
manager. The desktop is only a client and is not part of the serving path.
Wait until the source mirror is reported offline while the independent mirror
remains eligible. Do not stop the relay or the mirror under test.

### 4. Clone again

Clone the same stable URL into a new directory and compare the checked-out
commit with the commit recorded before the source went offline:

```sh
git clone https://forkmesh.com/OWNER/REPOSITORY forkmesh-failover-clone
git -C forkmesh-failover-clone rev-parse HEAD
```

The proof passes only when the fresh clone completes from an eligible mirror
and the commit matches. Keep the clone output and the source/mirror health
snapshot with the test-run record.

## Expected failures and recovery

- **No eligible mirror:** cloning must report repository or mirror unavailable;
  it must not claim preservation succeeded.
- **Stale, unhealthy, or integrity-mismatched mirror:** routing must exclude the
  mirror. Fix sync or integrity and repeat from step 2.
- **All serving hosts offline:** the stable URL is unavailable. Bring one
  integrity-matching host back before retrying.
- **Private repository:** do not use the public proof. Private reads require an
  authorized opaque-replica path and are outside this promise demonstration.

Rollback is explicit: restart `forkmesh-mirror-node` on the managed source host
and wait for fresh signed health. Preserve the failed clone directory and
sanitized logs until the result has been reviewed. The
[desktop control-node guide](operations/desktop-control-node.md) and
[mirror-node operations guide](operations/mirror-node.md) contain the service
and fleet recovery details.

## Supported proof surfaces

- Public browse, signed collaboration, and clone actions use the web app and a
  standard Git client.
- The signed `latest` release manifest is the authority for native package
  availability. At this review, it publishes Linux x86_64 only; macOS, Windows,
  Linux arm64, and repository build instructions must not be presented as
  published native packages. See the [desktop guide](qt-client.md) for source
  prerequisites.
- A managed independent server mirror uses the Linux headless mirror service.
- Mobile and editor integrations can participate in collaboration, but are not
  claimed as source-host controls for this drill.

## Measurement and closure gate

Optional analytics stays disabled by default. With explicit consent, ForkMesh
records only a coarse surface and a fixed action name for proof-link opens and
clone-command copies. It never records a repository, owner, URL, command, ref,
or form value. Report `home-proof-opened`, `proof-clone-copied`, and
`repository-clone-copied` as aggregate funnel counts, alongside an explicit
unsupported-claim count from the primary-surface audit.

Human comprehension remains a release gate and cannot be inferred from a
content test. For every five-person cohort, include maintainers, contributors,
and operators; ask each person to read one primary surface and then, without
prompting, answer:

1. What three jobs does ForkMesh do for source-code repositories?
2. What dependency does the independent-host design remove?
3. How would you demonstrate that benefit?

A person passes only by restating **collaborate**, **mirror**, **preserve**, and
the benefit of not relying on one hosting provider. A cohort passes at 4/5 or
better. Store role, surface, pass/fail, paraphrase, date, facilitator, and a link
to consented research notes. Do not mark this gate complete with blank,
synthetic, or inferred results.

## Linked automated evidence

- Supported release platforms: `app/tests/test_installer_release.py`
- Source-offline and mirror failure paths: `app/tests/test_mirror_serving.py`
  and `app/tests/test_https_mirror_routing_integration.py`
- Service restart and recovery: `app/tests/test_mirror_service_recovery.py`
- Product wording, action links, and documentation: `app/tests/test_product_promise.py`
- Privacy-safe telemetry: `app/tests/test_first_party_analytics.py`

Automated checks support the release gate; they do not replace a recorded
end-to-end drill, the five-person comprehension results, security review,
current documentation, or production telemetry evidence.
