# Disaster recovery

This guide prepares a community for losing its normal infrastructure: the
origin host is wiped, the hosted service is down, names stop resolving, a
signing key is lost, a region is cut off, or a backup is corrupt. It covers
preparation, recovery, and proving recovery works.

| File | What it is |
| --- | --- |
| [runbook.md](runbook.md) | The printable runbook: triage, roles and checklists, prerequisites, a decision tree per scenario, procedures with verification and rollback, and an exercise worksheet. |
| [exercise-register.md](exercise-register.md) | Every recorded exercise, its findings and corrective owners, and what closure still needs. |
| [`conformance/disaster-recovery/v1/runbook.json`](../../../conformance/disaster-recovery/v1/runbook.json) | The runbook source (`forkmesh-disaster-runbook-v1`). `runbook.md` is generated from it. |
| `tools/forkmesh_disaster_recovery.py` | Validates and renders the runbook, builds and verifies the bootstrap kit, manages the roster and key rotations, and runs drills. |

The runbook builds on
[offline and low-bandwidth sync](../offline-sync.md),
[transports and anonymity postures](../transports.md),
[self-hostable service roles](../operations/service-roles.md),
[node backup and recovery](../operations/node-backup-and-recovery.md), and
[portable identities](../portable-identities.md).

## Start here: prepare before a disaster

1. **Make keys.** One kit key, one export key per mirror operator, and one
   recovery key for each custodian. Keep the seeds on separate devices, held by
   different people.

   ```sh
   tools/forkmesh_offline_sync.py keygen kit.key          # prints the kit public key
   tools/forkmesh_offline_sync.py keygen export.key
   tools/forkmesh_offline_sync.py keygen recovery-1.key   # one per custodian
   ```

2. **Write the community roster** (below), then check it:

   ```sh
   python3 tools/forkmesh_disaster_recovery.py roster check roster.json
   ```

3. **Build the bootstrap kit** onto removable media, and write the kit public
   key and the manifest digest it prints on paper:

   ```sh
   python3 tools/forkmesh_disaster_recovery.py kit build /media/usb/forkmesh-kit \
     --signing-key kit.key --roster roster.json
   ```

4. **Print [runbook.md](runbook.md)** with its digest and the kit public key.
5. **Run the drills**, then tabletop and live exercises; record each one.

## The community roster

The roster (`forkmesh-recovery-roster-v1`) is how members find each other
without the website. It lists pinned keys, mirrors, and contacts, and it
travels inside the signed kit.

```json
{
  "schema": "forkmesh-recovery-roster-v1",
  "community": "example community",
  "updated": "2026-09-14",
  "recoveryThreshold": 2,
  "keys": [
    {"keyId": "export-1", "publicKey": "<base64url>", "role": "export", "holder": "Mirror operator A", "status": "active"},
    {"keyId": "recovery-1", "publicKey": "<base64url>", "role": "recovery", "holder": "Custodian 1", "status": "active"},
    {"keyId": "recovery-2", "publicKey": "<base64url>", "role": "recovery", "holder": "Custodian 2", "status": "active"},
    {"keyId": "recovery-3", "publicKey": "<base64url>", "role": "recovery", "holder": "Custodian 3", "status": "active"}
  ],
  "mirrors": [
    {"name": "library mirror", "urls": ["http://mirror.example.lan:8080/project.git", "http://10.0.0.20:8080/project.git"],
     "pinnedHosts": {"mirror.example.lan": "10.0.0.20"}}
  ],
  "contacts": [{"role": "coordinator", "name": "<name>", "channel": "<phone or radio>"}],
  "rotations": []
}
```

Key roles are `export` (signs removable media), `kit` (signs bootstrap kits),
and `recovery` (signs key rotations); status is `active` or `revoked`.
`roster check` exits 3 when the roster is invalid, has no active export key, or
has fewer distinct recovery holders than `recoveryThreshold`. It warns when no
mirror URL works without DNS (literal address, onion address, or a
`pinnedHosts` name), when one custodian can rotate keys alone, or when an export
key holder also holds a recovery key.

```sh
python3 tools/forkmesh_disaster_recovery.py roster check roster.json [--json]
python3 tools/forkmesh_disaster_recovery.py roster keys roster.json --role export
python3 tools/forkmesh_disaster_recovery.py roster profile roster.json --out lan.json
```

`roster profile` writes a `lan-only` [transport profile](../transports.md#postures)
that dials the roster's pinned local addresses, so a clone needs no DNS.

## The bootstrap kit

A kit is a directory holding exactly what a community needs to recover with no
network: the runbook source and printable copy, the recovery tools, the guides
they cite, and the roster. Its manifest, `forkmesh-recovery-kit.json`, is a
`forkmesh-recovery-kit-signed-v1` envelope over a `forkmesh-recovery-kit-v1`
manifest listing every file's SHA-256 and size, the runbook version and digest,
the build date, and `reviewBy`.

```sh
python3 -B tools/forkmesh_disaster_recovery.py kit verify /media/usb/forkmesh-kit \
  --trusted-key <kit key from paper>
```

Run it from the kit on the disconnected machine; it never opens a network
connection. It checks the signature against the key you pass (read it from
paper, never from the kit), that every listed file matches with no extras or
symlinks, that the runbook validates against the kit's own tools and guides and
matches its digest and printable copy, and that the roster verifies.

A key that starts with `-` must be joined to its flag:
`--trusted-key=-Ab…`. Written as two words, the tool reads the key as an
unknown option and exits with code 2.

| State | Exit code | Meaning |
| --- | --- | --- |
| `verified` | `0` | Everything checked, and the kit is before its review date. |
| `stale` | `4` | Everything checked, but the kit is past `reviewBy`. Use it, then rebuild it from a current roster. |
| `failed` | `3` | Do not run anything from this kit. |

`--today YYYY-MM-DD` checks freshness as of another date.

## Key recovery

A lost or stolen export or kit key is replaced by a rotation signed by a
threshold of recovery custodians; no single custodian or lost key can do it.

```sh
python3 tools/forkmesh_disaster_recovery.py key rotate --roster roster.json \
  --recovery-key recovery-1.key --revoke <lost key> --successor <new key> \
  --holder "<holder>" --reason "<reason>" --issued <YYYY-MM-DD> --out rotation.json
python3 tools/forkmesh_disaster_recovery.py key cosign rotation.json --recovery-key recovery-2.key --roster roster.json
python3 tools/forkmesh_disaster_recovery.py key apply roster.json rotation.json --out roster-rotated.json
```

The statement is `forkmesh-recovery-key-rotation-v1`, wrapped in
`forkmesh-recovery-key-rotation-signed-v1`. `key apply` refuses a rotation with:

| Code | Refused because |
| --- | --- |
| `statement_digest` | `statementSha256` does not match the statement. |
| `community_mismatch` | The rotation names another community. |
| `already_applied` | The roster already lists this rotation. |
| `bad_signature` | A signature does not verify. |
| `signer_not_recovery_key` | A signer is not an active recovery key. The lost key cannot sign its own replacement. |
| `below_threshold` | Fewer distinct recovery signatures than `recoveryThreshold`. |
| `revoke_unknown` | A revoked key is not an active key in the roster. |
| `revoke_recovery_key` | Recovery keys change only by building a new kit with the custodians present. |
| `successor_known` | The successor key is already in the roster. |
| `successor_role` | The successor must be an export or kit key. |
| `statement_invalid` | The successor key or issued date is malformed. |

Media signed by the revoked key then fails import; re-export from a verified
copy with the successor key. Never lower the threshold during an incident.

Mirror node identity keys move with the node's sealed identity export
([moving or replacing an identity key](../operations/node-backup-and-recovery.md#moving-or-replacing-an-identity-key)).
Personal identities recover with the portable recovery kit
([recovery drills](../portable-identities.md#recovery-drills)).

## Drills

Each scenario has a live drill that builds a small community in scratch space
(origin, mirror, maintainer clone, signed export, roster), causes the disaster,
recovers with the runbook's tools, and measures the result.

```sh
python3 tools/forkmesh_disaster_recovery.py drill run all --report drills.json --telemetry drills.jsonl
python3 tools/forkmesh_disaster_recovery.py drill run origin-loss --fault no-local-clone
```

| Scenario | What the drill does | Fault injection |
| --- | --- | --- |
| `origin-loss` | Deletes the origin, restores from mirror plus maintainer clone, refuses a stale overwrite. | `no-local-clone` |
| `main-service-loss` | Stops the hosted copy and serves a read-only static mirror a newcomer clones and verifies. | `no-surviving-mirror` |
| `dns-loss` | Clones over a `lan-only` route with roster-pinned addresses; strace flags any DNS query. | `no-pinned-address` |
| `key-loss` | Refuses invalid rotations, applies a 2-of-3 rotation, refuses media forged with the lost key. | `threshold-unreachable` |
| `regional-isolation` | Two diverged regions exchange signed media; every commit reaches both. | `unpinned-region-key` |
| `corrupted-backup` | Rejects a corrupt backup and restores the previous verified one, recording loss. | `all-copies-corrupt` |
| `offline-bootstrap` | Verifies a kit and imports offline with the network blocked; refuses a tampered kit. | `wrong-kit-key` |

A `forkmesh-disaster-drill-report-v1` report records `recoveryMs`,
`withinObjective`, `dataLoss`, `missingPrerequisites`, `unsafeSteps`, each step
and check, and `verdict`. A drill passes only when every check passes within
budget, data loss is allowed by policy, and no blocking prerequisite is missing.
`--telemetry FILE` appends one count-only `forkmesh-disaster-drill-telemetry-v1`
line per drill with no paths, keys, names, or addresses.

## Exercises and evidence

Record every exercise as
`conformance/disaster-recovery/v1/exercises/DRX-NNNN.json`
(`forkmesh-disaster-exercise-v1`): kind (`tabletop` or `live`), status, runbook
version and digest, whether only the offline runbook was used, participants
with roles and familiarity (`unfamiliar`, `familiar`, `automation`), and per
scenario the result, recovery time, data loss, missing prerequisites, and
unsafe steps. Each missing prerequisite or unsafe step taken names a finding
with a corrective owner (`person`, runbook `role`, or `issue`).

Retain automated drill reports as content-addressed evidence, then cite them:

```sh
python3 tools/forkmesh_disaster_recovery.py attest drills.json   # prints evidence/<sha256>.json references
python3 tools/forkmesh_disaster_recovery.py render
python3 tools/forkmesh_disaster_recovery.py gate
```

A row citing `drillReport` must match the report it cites. Editing the runbook
without bumping its version fails validation for exercises on that version.

## Closure

`closure` lists the unmet checks and exits 1 until all are met; the
[register](exercise-register.md) shows the same status.

| Check | Met when |
| --- | --- |
| `automated-drills-current` | Every scenario has a passing drill report on the current runbook digest. |
| `platform-evidence` | Those reports cover Linux, macOS, and Windows. |
| `tabletop-drills-recorded` | Every scenario was run as a tabletop exercise with at least two people. |
| `live-drills-recorded` | People recovered every scenario in a live exercise. |
| `unfamiliar-team-restored` | Two or more people unfamiliar with ForkMesh restored a verified network using only the offline runbook. |
| `findings-owned` | Every open finding has a named person as its corrective owner. |

Automation cannot meet the people checks.

## Clean restoration

When normal infrastructure returns:

- compare its refs with the restored network before anything syncs, and push
  restored work to it as fast-forwards only;
- keep any diverged branches under review, never force-push over them;
- stop emergency static mirrors and delete temporary transport profiles;
- apply any key rotations to every roster copy, then rebuild and re-sign the
  kit, and write the new key and digest on paper;
- write the exercise or incident record, with a corrective owner for every
  finding.

## Commands

| Command | Purpose |
| --- | --- |
| `forkmesh_disaster_recovery.py validate` | Check the runbook's roles, prerequisites, decision trees, commands, guide headings, and kit completeness against the repository. |
| `forkmesh_disaster_recovery.py render [--check]` | Write or check `runbook.md` and `exercise-register.md`. |
| `forkmesh_disaster_recovery.py gate` | Validate the runbook, every exercise, and rendered output. |
| `forkmesh_disaster_recovery.py closure` | Report the closure checks. |
| `forkmesh_disaster_recovery.py roster check`, `roster keys`, `roster profile` | Roster commands (above). |
| `forkmesh_disaster_recovery.py key rotate`, `key cosign`, `key apply` | Key rotation (above). |
| `forkmesh_disaster_recovery.py kit build`, `kit verify` | Bootstrap kit (above). |
| `forkmesh_disaster_recovery.py drill run` | Live drills (above). |
| `forkmesh_disaster_recovery.py attest` | Retain drill reports as evidence. |

| Exit code | Meaning |
| --- | --- |
| `0` | Done. |
| `1` | Validation, a drill, the gate, or closure failed. |
| `2` | Usage error or unreadable input. |
| `3` | A kit, roster, or rotation did not verify. |
| `4` | The kit verified but is past its review date. |

## Supported platforms

The drills need Git 2.40+ and Python 3.9+. The `disaster-recovery-conformance`
quality gate runs the test suite, and CI runs the suite, gate, and every drill
on Linux, macOS, and Windows. DNS leak tracing in the `dns-loss` drill needs
strace and runs only on Linux.

## Limits

- Drills do not build the Go mirror node; node archive restores and identity
  imports are covered by `server/nodeops_test.go` instead.
- The `search` and `organization-service` roles are hosted-only, so
  main-service loss cannot restore them.
- Kits are signed by one kit key, not a threshold.
