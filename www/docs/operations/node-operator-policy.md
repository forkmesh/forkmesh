# Node operator policy and legal response

A ForkMesh node operator decides, for their own machine only, what the node
stores and serves, how much disk it may use, and how it answers an abuse report
or a legal request. None of these choices is a network-wide rule. When this
node refuses or quarantines a repository, every other mirror that carries it
keeps serving it. The gateway reports the repository as unavailable here, so
the router sends readers to another mirror.

The policy is opt-in. A gateway configured without `operatorPolicy` behaves
exactly as before.

## Enabling it

1. Create an owner-only state directory and policy file for the gateway's
   service user:

   ```sh
   install -d -m 0700 /var/lib/forkmesh-mirror/operator-policy
   install -m 0600 /dev/stdin /var/lib/forkmesh-mirror/operator-policy.json <<'JSON'
   {"schemaVersion": 1, "profile": "home", "jurisdiction": "eu"}
   JSON
   ```

2. Add the policy to `mirror-gateway.json`:

   ```json
   "operatorPolicy": {
     "policyPath": "/var/lib/forkmesh-mirror/operator-policy.json",
     "stateDirectory": "/var/lib/forkmesh-mirror/operator-policy"
   }
   ```

3. `mirror_gateway.py --check` validates the policy. Then restart the gateway.
   `node_operator_policy.py` must be installed beside `mirror_gateway.py`. The
   Vultr installer ships it. If the module is missing, a configuration that
   names `operatorPolicy` fails validation instead of running without a
   policy.

All commands below are `python3 -I tools/node_operator_policy.py <command>`
and print JSON.

## Profiles and safe defaults

`profile` chooses defaults for the kind of machine. Any field can be
overridden. Every limit is finite, so an unedited profile cannot fill a disk.

| Setting | home | community | commercial |
|---|---|---|---|
| `limits.maxRepositoryBytes` | 1 GiB | 4 GiB | 20 GiB |
| `limits.maxBlobBytes` | 50 MiB | 100 MiB | 400 MiB |
| `limits.maxPushBytes` (uncompressed new objects) | 512 MiB | 2 GiB | 8 GiB |
| `limits.maxRepositories` | 50 | 1000 | 10000 |
| `limits.storageBudgetBytes` (whole node) | 20 GiB | 500 GiB | 5000 GiB |
| `seeding.privateReplicas` | false | false | true |
| `violationAction` | quarantine | quarantine | quarantine |
| `retention.evidenceDays` | 90 | 180 | 365 |

Home and community nodes do not serve other people's encrypted private
replicas by default. Their operators usually cannot staff abuse review for
content they cannot inspect.

## Policy reference

```json
{
  "schemaVersion": 1,
  "profile": "community",
  "limits": {"maxBlobBytes": 104857600},
  "content": {
    "blockedExtensions": [".iso", ".exe"],
    "blockedPathPatterns": ["*.mp4", "datasets/*.parquet"],
    "blockedObjectIds": ["<40- or 64-hex object id from a takedown>"]
  },
  "repositories": {
    "allow": [],
    "block": ["spam-owner", "alice/leaked-keys"]
  },
  "seeding": {"privateReplicas": false},
  "violationAction": "quarantine",
  "retention": {"evidenceDays": 180},
  "jurisdiction": "eu"
}
```

- **Repositories and communities.** `repositories.allow` and `.block` take an
  owner (a person, an organization, or a community) or `owner/repository`. A
  non-empty `allow` list means the node stores only those repositories. The
  gateway also refuses to *create* a blocked or unlisted repository, or one
  past `maxRepositories`.
- **Content.** Unknown fields, duplicate keys, and group- or world-writable
  policy files are refused.
  - `blockedExtensions` is checked against every entry name in every new tree,
    so renaming an accepted file to a refused name is caught.
  - A pattern without `/` matches file names the same way.
  - A pattern with `/` matches the first path at which Git reaches the object.
  - `blockedObjectIds` names exact content, for example the blob a copyright
    notice identifies.
- **`violationAction`.** `quarantine` lets `enforce --apply` stop serving
  stored violators. `report` lists them only. Pushes that violate the policy
  are always refused.
- **`jurisdiction`.** One of `us`, `eu`, `uk`, `other`. It chooses the runbook
  attached to every case.
- **`secrets`.** Optional. It holds an organization secret policy, for example
  `"secrets": {"mode": "block"}`. `secret_gate.py` must be installed beside
  this module; the Vultr installer, `install.sh`, and
  `provision_ssh_gateway.sh` ship it. If the module is missing, a policy that
  names `secrets` fails validation and the hook refuses every push. See
  [Secret scanning before replication](../security/secret-scanning.md) for
  rules, repository policy, and signed overrides.

## Protected branches

`branches` protects refs on this node's writable repositories. The same
pre-receive hook enforces it for the HTTPS mirror gateway and for the SSH
gateway (see [Git SSH public-key authentication](git-ssh-gateway.md)).

```json
"branches": {
  "rules": [
    {"pattern": "main", "requireSignedCommits": true},
    {"pattern": "release/*", "repositories": ["alice/app"]},
    {"pattern": "refs/tags/v*", "blockForcePush": false}
  ],
  "allowedSignersPath": "/var/lib/forkmesh-mirror/allowed_signers"
}
```

- `pattern` is a glob. A bare name means `refs/heads/<name>`; start it with
  `refs/` for tags or other refs. `*` also matches `/`.
- `repositories` limits a rule to owners or `owner/repository`. An empty list
  covers every repository on the node.
- `blockDeletion` (default `true`) refuses deleting a matching ref.
- `blockForcePush` (default `true`) refuses any update whose new tip does not
  contain the current tip.
- `requireSignedCommits` (default `false`) refuses every commit the update
  adds unless an SSH key in `allowedSignersPath` signed it. The file uses
  OpenSSH's allowed signers format (`principal namespaces="git" <key>`), must
  be owned by the gateway account, and must not be group- or world-writable.
  A new protected ref checks the commits that are new to the repository. An
  update checks every commit between the old and new tips, so an unsigned
  commit already stored on another branch cannot be fast-forwarded in. Only
  SSH signatures are verified: OpenPGP and X.509 signatures are refused.
- A protected update whose expected old value no longer matches the ref is
  refused as `stale-ref`. Git also re-checks the old value under its ref lock
  after the hook, so a push that loses a race never moves the ref.

A refused push changes no ref and stores no object. The pusher sees each rule,
the evidence reference, and copy-ready recovery steps. For example:

```text
remote: This node's operator policy refused the push (other ForkMesh nodes may accept it):
remote:   protected-branch-force-update: refs/heads/main is protected and 3f1c… does not contain its tip 9ab2…
remote:   to recover (protected-branch-force-update):
remote:     git fetch origin
remote:     git rebase origin/main
remote:     git push origin HEAD:main
```

Every admitted push that updates a protected ref appends an `admit-push`
record, and every refusal appends `reject-push`. Both go to the hash-chained
audit log. Records name the repository, refs, old and new object ids, the
matching patterns, and the policy digest. They never contain file content,
keys, or the pusher's credentials. If the admit record cannot be written, the
push is refused.

Adding or tightening a branch rule counts as availability-reducing, so
`apply` needs `--confirm`.

The HTTPS mirror gateway already refuses every deletion and non-fast-forward
update, whatever the policy says. On that path, branch rules add the
signed-commit and stale-write checks and the audit trail.

## How enforcement works

**Pushes.** The gateway points `core.hooksPath` at a directory it owns
(`<stateDirectory>/hooks`), which holds only a `pre-receive` hook that runs
this module. Hooks planted inside a repository still never run.

Git has already staged the pushed objects in its receive quarantine when the
hook runs. The hook checks them against the policy:

- repository allow/block lists and quarantine
- push size, projected repository size, and the node storage budget
- blob sizes, names, paths, and blocked object ids
- probable secrets, when `secrets` is set. Blocked findings refuse the push.
  Warnings and signed overrides are admitted, shown to the pusher, and
  audited.

If anything fails, the hook exits non-zero and Git deletes the staged objects.
The repository's refs and existing objects, and every other repository, are
untouched. The pusher sees each reason as a `remote:` line with an evidence
reference.

If the policy cannot be read, the hook refuses every push (it fails closed).

**Stored data.** Upstream fetches and older data never pass through the hook.
Run `enforce` after tightening a policy, or on a timer:

```sh
node_operator_policy.py enforce --policy P --state S --root R          # preview
node_operator_policy.py enforce --policy P --state S --root R --apply  # act
```

With `secrets` set, `enforce` also scans stored history. Anything it finds
has already been replicated, so each affected repository in the output
carries a `secretResponse` plan: containment, rotation, notification, and
residual risk. Under `block` the repository is quarantined; under `warn` it
is only reported. With `--apply`, every detection is audited as
`secret-detected-stored`.

A quarantine writes `<state>/quarantine/<owner>/<repo>.json`. From that moment
the gateway answers that repository as not found, its signed health proof says
`available: false` (so the router routes elsewhere), and pushes to it are
refused. Its bytes stay on disk, so a quarantine can always be lifted with
`release`.

**Private replicas.** When `seeding.privateReplicas` is false, the gateway
serves no private replica, even if the store holds some.

## Preview, apply, and roll back

Never edit the active policy file in place. Write a candidate and apply it:

```sh
node_operator_policy.py preview --policy candidate.json --current active.json --state S --root R
node_operator_policy.py apply   --policy candidate.json --target active.json --state S --root R [--confirm]
node_operator_policy.py history --state S
node_operator_policy.py rollback --target active.json --state S --root R [--confirm]
```

`preview` reports capacity:

- stored bytes, storage budget, headroom, and utilization
- repository slots used and allowed
- the largest repository

It also lists the repositories that would become unavailable here and states
the consequences in plain language. A repository that only this node seeds
becomes unavailable to the whole network.

`apply` refuses any change that reduces availability until you pass
`--confirm`. That includes lower limits, new block rules, a narrower allow
list, dropping private replicas, or violators that would be quarantined.
Loosening a policy needs no confirmation.

Each apply archives the previous file under `<state>/policy-history/`, and
`rollback` restores the newest archive through the same preview and
confirmation step. Rolling back archives the policy being replaced, so you can
roll forward again.

## Reports, appeals, and removal

```sh
node_operator_policy.py report  --policy P --state S --owner alice --name app \
    --category copyright --reporter "rights@example.org" --details "..." [--legal-hold]
node_operator_policy.py action  --policy P --state S --case <id> --note "notice verified"
node_operator_policy.py appeal  --state S --case <id> --appellant alice --details "licensed MIT"
node_operator_policy.py resolve --state S --case <id> --decision reverse|uphold|dismiss --note "..."
node_operator_policy.py remove  --state S --root R --owner alice --name app --reason "court order" [--confirm]
```

- A case records the report, the jurisdiction runbook, and every step in its
  history.
- `action` quarantines the repository under the case.
- `resolve --decision reverse` or `dismiss` lifts a quarantine that case
  created.
- `remove` permanently deletes a repository from this node only. It works only
  on a quarantined repository, needs `--confirm`, and is recorded as
  irreversible.

**Evidence.** Evidence records hold only metadata: rules, object ids, paths,
sizes, refs, and the policy digest. They never hold the refused content. Keep
any legally required copy outside ForkMesh under counsel's direction.

`prune-evidence` deletes evidence past `retention.evidenceDays`. It never
deletes evidence for an open or appealed case, or for a case with a legal hold.

### Jurisdiction runbooks

`runbook --policy P` prints the checklist for the configured jurisdiction. The
checklists are process reminders, not legal advice:

- **us.** DMCA notice elements (17 U.S.C. §512(c)(3)), prompt quarantine, and
  restore 10–14 business days after a valid counter-notice unless suit is
  filed. CSAM is reported to NCMEC and its content is not retained.
- **eu.** Digital Services Act notice-and-action (Art. 16), statement of
  reasons (Art. 17), internal complaint handling (Art. 20), and notifying
  authorities of threats to life or safety (Art. 18).
- **uk.** Online Safety Act 2023 illegal-content assessment, a complaint route,
  and IWF/NCA reporting for CSAM.
- **other.** Record the legal basis, quarantine rather than delete until the
  request is verified, and take local advice.

## Audit and telemetry

Every quarantine, release, removal, policy apply or rollback, report, case
step, refused push, admitted protected-branch push, and evidence prune is appended to `<state>/audit.jsonl`.
Each record carries the SHA-256 of the record before it.

- `verify-audit` checks the chain and exits 4 at the first altered or missing
  record.
- `status` returns secret-free counters for local dashboards: quarantined
  repositories, open, appealed, and resolved cases, refused pushes, admitted
  protected-branch pushes, evidence records, and the audit state.
- `status` also returns `secrets` counters: blocked and warned pushes,
  overridden and allowlisted findings, invalid overrides, and detections in
  stored data.

## Not covered yet

- Bandwidth and egress caps are not enforced. Use the gateway's service
  counters and your reverse proxy.
- Upstream fetches by the Go daemon are not checked before they land. Run
  `enforce` after sync. That includes secret scanning, which there is
  detection after replication.
- The desktop app has no policy editor. The policy is a file plus this CLI.
- Branch rules do not know who is pushing. Per-collaborator roles come from the
  Worker or local SSH authorization (owner, `write`, `maintain`, `admin`, and
  pull-request namespaces). Required reviews and required checks are not
  enforced by the hook.
