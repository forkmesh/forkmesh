# Node operator policy and legal response

An operator decides, for their own node only, what it stores and serves, how
much disk it may use, and how it answers abuse reports and legal requests. None
of this is a network-wide rule: a repository this node refuses or quarantines is
reported unavailable here and the router sends readers to another mirror. The
policy is opt-in; a gateway without `operatorPolicy` is unaffected.

## Enabling it

Create an owner-only (`0600`) policy file and a `0700` state directory for the
gateway's service user, then add to `mirror-gateway.json`:

```json
"operatorPolicy": {
  "policyPath": "/var/lib/forkmesh-mirror/operator-policy.json",
  "stateDirectory": "/var/lib/forkmesh-mirror/operator-policy"
}
```

`mirror_gateway.py --check` validates it; `node_operator_policy.py` must sit
beside the gateway or validation fails. Commands below are
`python3 -I tools/node_operator_policy.py <command>` and print JSON.

## Policy reference

`profile` (`home`, `community`, `commercial`) sets finite defaults for
`limits.maxRepositoryBytes`, `maxBlobBytes`, `maxPushBytes`, `maxRepositories`
and `storageBudgetBytes`, `seeding.privateReplicas` (off except commercial),
`violationAction` and `retention.evidenceDays`. Any field can be overridden.

- `repositories.allow` / `.block` take an owner or `owner/repository`; a
  non-empty allow list restricts the node to those repositories.
- `content.blockedExtensions`, `blockedPathPatterns` and `blockedObjectIds`
  refuse matching names, paths or exact objects in every new tree.
- `violationAction`: `quarantine` lets `enforce --apply` stop serving stored
  violators; `report` only lists them. Violating pushes are always refused.
- `jurisdiction` (`us`, `eu`, `uk`, `other`) selects the case runbook.
- `secrets` enables the [secret scanning](../security/secret-scanning.md) gate.
- Unknown fields, duplicate keys and group- or world-writable files are refused.

## Protected branches

```json
"branches": {
  "rules": [
    {"pattern": "main", "requireSignedCommits": true},
    {"pattern": "refs/tags/v*", "blockForcePush": false}
  ],
  "allowedSignersPath": "/var/lib/forkmesh-mirror/allowed_signers"
}
```

The same pre-receive hook enforces these on the HTTPS gateway and the
[SSH gateway](git-ssh-gateway.md). A bare `pattern` means `refs/heads/<name>`;
`repositories` scopes a rule. `blockDeletion` and `blockForcePush` default to
`true`. `requireSignedCommits` accepts only SSH signatures from the OpenSSH
allowed-signers file and checks every commit the update adds. A stale expected
old value is refused as `stale-ref`. A refused push changes no ref, stores no
object and prints recovery steps; admitted and refused protected pushes are
audited. Tightening a rule requires `apply --confirm`.

## Enforcement

- **Pushes.** The gateway sets `core.hooksPath` to `<stateDirectory>/hooks`,
  holding only its own `pre-receive`. Staged objects are checked for allow/block
  lists, quarantine, push and storage size, blob names and ids, and secrets. Any
  failure discards the staged objects. An unreadable policy refuses every push.
- **Stored data.** Upstream fetches bypass the hook, so run `enforce` (preview)
  and `enforce --apply` after tightening a policy or on a timer.
- **Quarantine** writes `<state>/quarantine/<owner>/<repo>.json`: the repository
  answers as not found and its signed health says `available: false`. Bytes stay
  on disk until `release` or `remove`.

## Changing a policy

Never edit the active file in place. `preview` reports capacity and which
repositories would become unavailable; `apply` archives the previous file under
`<state>/policy-history/` and requires `--confirm` for any change that reduces
availability; `history` and `rollback` restore through the same checks.

## Reports, appeals and removal

`report`, `action`, `appeal`, `resolve --decision reverse|uphold|dismiss` and
`remove` (quarantined repositories only, `--confirm`, irreversible on this node)
manage cases. Evidence holds metadata only, never refused content;
`prune-evidence` honours `retention.evidenceDays` and never prunes open,
appealed or legal-hold cases. `runbook` prints a jurisdiction checklist
(process reminders, not legal advice).

## Audit and status

Every policy, case, quarantine and push decision is appended to the hash-chained
`<state>/audit.jsonl`; `verify-audit` exits 4 at the first broken record.
`status` returns secret-free counters for dashboards. Bandwidth caps, a desktop
policy editor and per-pusher branch rules are not provided.
