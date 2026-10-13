# Disaster recovery

Use this guide to prepare a community for the day its normal infrastructure is
gone: the origin host is wiped, forkmesh.com is down, names stop resolving, a
signing key is lost, a region is cut off, or a backup turns out to be corrupt.
It covers what to prepare, how to recover, and how to prove recovery works
before you need it.

| File | What it is |
| --- | --- |
| [runbook.md](runbook.md) | The printable runbook: triage, roles and checklists, prerequisites, a decision tree per scenario, procedures with verification and rollback, and an exercise worksheet. |
| [exercise-register.md](exercise-register.md) | Every recorded exercise, its findings and corrective owners, and what closure still needs. |
| [`conformance/disaster-recovery/v1/runbook.json`](../../../conformance/disaster-recovery/v1/runbook.json) | The runbook source (`forkmesh-disaster-runbook-v1`). `runbook.md` is generated from it. |
| `tools/forkmesh_disaster_recovery.py` | Validates and renders the runbook, builds and verifies the bootstrap kit, manages the roster and key rotations, and runs drills. |

Tracking issue: #518. The runbook builds on
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

4. **Print [runbook.md](runbook.md)** and keep it with the kit. Write the
   runbook digest and the kit public key on it.
5. **Run the drills** (below), then a tabletop and a live exercise with people.
   Record each exercise in the register.

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
  "contacts": [{"role": "coordinator", "name": "Alex", "channel": "phone +1 555 0100"}],
  "rotations": []
}
```

Key roles are `export` (signs removable media), `kit` (signs bootstrap kits),
and `recovery` (signs key rotations). A key's status is `active` or `revoked`.

`roster check` exits 3 when the roster is invalid, has no active export key, or
has fewer distinct recovery key holders than `recoveryThreshold`. It warns when
no mirror address works without DNS (`dnsFree` is false), when one custodian
can rotate keys alone, or when an export key holder also holds a recovery key.
A URL works without DNS when its host is a literal address, an onion address,
or a name in the mirror's `pinnedHosts`.

```sh
python3 tools/forkmesh_disaster_recovery.py roster check roster.json [--json]
python3 tools/forkmesh_disaster_recovery.py roster keys roster.json --role export
python3 tools/forkmesh_disaster_recovery.py roster profile roster.json --out lan.json
```

`roster profile` writes a `lan-only` [transport profile](../transports.md#postures)
that dials the roster's pinned local addresses, so a clone needs no DNS.

## The bootstrap kit

A kit is a directory that holds exactly the files a community needs to recover
with no network: the runbook source and its printable copy, the recovery
tools, the guides they cite, and the roster. Its manifest,
`forkmesh-recovery-kit.json`, is a `forkmesh-recovery-kit-signed-v1` envelope
over a `forkmesh-recovery-kit-v1` manifest that lists every file's SHA-256 and
size, the runbook version and digest, the build date, and a review date
(`reviewBy`, the build date plus the runbook's review interval).

```sh
python3 -B tools/forkmesh_disaster_recovery.py kit verify /media/usb/forkmesh-kit \
  --trusted-key <kit key from paper>
```

Run it from the kit itself on the disconnected machine. `kit verify` never
opens a network connection. It checks that:

- the signature verifies, and the signer is the key you passed (always read it
  from paper, never from the kit);
- every listed file is present with its digest, with no extra files and no
  symbolic links;
- the runbook inside the kit validates against the kit's own tools and guides,
  matches the digest in the manifest, and matches the printable copy;
- the roster inside the kit verifies.

The tool never writes bytecode caches, so running it from the kit does not add
files. `-B` makes that explicit for any Python started by hand.

| State | Exit code | Meaning |
| --- | --- | --- |
| `verified` | `0` | Everything checked, and the kit is before its review date. |
| `stale` | `4` | Everything checked, but the kit is past `reviewBy`. Use it, then rebuild it from a current roster. |
| `failed` | `3` | Do not run anything from this kit. |

`--today YYYY-MM-DD` checks freshness as of another date.

## Key recovery

A lost or stolen export or kit key is replaced by a signed rotation. The
recovery key custodians sign it, and no single custodian and no lost key can
do it alone.

```sh
python3 tools/forkmesh_disaster_recovery.py key rotate --roster roster.json \
  --recovery-key recovery-1.key --revoke <lost key> --successor <new key> \
  --holder "Mirror operator A" --reason "laptop stolen" --issued 2026-09-14 --out rotation.json
python3 tools/forkmesh_disaster_recovery.py key cosign rotation.json --recovery-key recovery-2.key --roster roster.json
python3 tools/forkmesh_disaster_recovery.py key apply roster.json rotation.json --out roster-rotated.json
```

The statement is `forkmesh-recovery-key-rotation-v1`, wrapped in
`forkmesh-recovery-key-rotation-signed-v1`. `key apply` refuses a rotation with
the first matching problem codes:

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

After the rotation, media signed by the revoked key fails import against the
new roster's export keys. Re-export from a verified copy with the successor key.
Never lower the threshold during an incident.

Mirror node identity keys move with the node's sealed identity export
([moving or replacing an identity key](../operations/node-backup-and-recovery.md#moving-or-replacing-an-identity-key)).
Personal identities recover with the portable recovery kit
([recovery drills](../portable-identities.md#recovery-drills)).

## Drills

Each scenario has a live drill that builds a small community in scratch space:
an origin, a mirror, a maintainer clone, a signed export, and a roster. The
drill then causes the disaster and recovers using the same tools the runbook
names, and measures the result.

```sh
python3 tools/forkmesh_disaster_recovery.py drill run all --report drills.json --telemetry drills.jsonl
python3 tools/forkmesh_disaster_recovery.py drill run origin-loss --fault no-local-clone
```

| Scenario | What the drill does | Fault injection |
| --- | --- | --- |
| `origin-loss` | Deletes the origin after a push the mirror missed, restores from the mirror plus the maintainer's clone, and refuses a stale overwrite. | `no-local-clone`: the newest commit is lost, and the drill fails. |
| `main-service-loss` | Stops the hosted copy, detects it as unreachable, serves a read-only static mirror, and has a newcomer clone and verify it. | `no-surviving-mirror` |
| `dns-loss` | Clones by name over a `lan-only` route with the address pinned from the roster. On Linux, Git runs under strace, which flags any DNS query. | `no-pinned-address` |
| `key-loss` | Refuses a rotation signed by the lost key and one below threshold, applies a 2-of-3 rotation, refuses media forged with the lost key, and imports the re-export. | `threshold-unreachable` |
| `regional-isolation` | Two regions diverge and exchange signed media both ways; diverged `main` is kept aside and every commit reaches both regions. | `unpinned-region-key` |
| `corrupted-backup` | Rejects the corrupted newest backup, proves importing it changes nothing, and restores the previous verified backup, recording the commits lost. | `all-copies-corrupt` |
| `offline-bootstrap` | Builds a kit, verifies it and runs its roster check and offline import with the network blocked, and refuses a tampered kit. | `wrong-kit-key` |

A `forkmesh-disaster-drill-report-v1` report records `recoveryMs` (from the
disaster to the last verification), `withinObjective` against the scenario's
automated budget, `dataLoss` (refs and commits lost, and whether the
scenario's policy allows it), `missingPrerequisites`, `unsafeSteps` refused or
taken, each step with its runbook procedure and duration, every verification
check, and `verdict`. A drill passes only when every check passes, it finishes
within budget, the data loss is allowed, and no blocking prerequisite is missing.

`--telemetry FILE` appends one count-only `forkmesh-disaster-drill-telemetry-v1`
line per drill: scenario, fault, verdict, operating system, recovery time, and
counts. It never includes paths, keys, names, or addresses.

## Exercises and evidence

Record every exercise, by people or by automation, as
`conformance/disaster-recovery/v1/exercises/DRX-NNNN.json`
(`forkmesh-disaster-exercise-v1`). Each record has a kind (`tabletop` or
`live`), a status, the runbook version and digest, whether only the offline
runbook was used, and participants with their runbook roles and familiarity
(`unfamiliar`, `familiar`, or `automation`). Each scenario row records its
result, recovery time, whether a verified repository network was restored,
data loss, missing prerequisites, and unsafe steps.

Every missing prerequisite, and every unsafe step that was taken, must name a
finding. Every finding has a corrective owner: a `person`, a runbook `role`,
or an `issue`.

Retain automated drill reports as content-addressed evidence, then cite them:

```sh
python3 tools/forkmesh_disaster_recovery.py attest drills.json   # prints evidence/<sha256>.json references
python3 tools/forkmesh_disaster_recovery.py render
python3 tools/forkmesh_disaster_recovery.py gate
```

When a row cites `drillReport`, validation recomputes it from the report: the
scenario, runbook digest, recovery minutes, result, commits lost, missing
prerequisites, and counts of unsafe steps must match. Editing the runbook
without bumping its version fails validation for any exercise that used that
version.

## Closure

`closure` lists what issue closure still needs, and exits 1 until all of it is
met. The [register](exercise-register.md) shows the same status.

| Check | Met when |
| --- | --- |
| `automated-drills-current` | Every scenario has a passing drill report on the current runbook digest. |
| `platform-evidence` | Those reports cover Linux, macOS, and Windows. |
| `tabletop-drills-recorded` | Every scenario was run as a tabletop exercise with at least two people. |
| `live-drills-recorded` | People recovered every scenario in a live exercise. |
| `unfamiliar-team-restored` | A team of at least two people unfamiliar with ForkMesh restored a verified repository network in a live exercise using only the offline runbook. |
| `findings-owned` | Every open finding has a named person as its corrective owner. |

Automation cannot meet the people checks. Do not record an exercise that did
not happen.

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
| `forkmesh_disaster_recovery.py validate` | Check the runbook against the repository: roles, prerequisites, decision trees (every node reachable, no cycles, a recovered outcome), commands (each tool, command, and option exists), guide headings, and kit completeness. |
| `forkmesh_disaster_recovery.py render [--check]` | Write or check `runbook.md` and `exercise-register.md`. |
| `forkmesh_disaster_recovery.py gate` | Validate the runbook and every exercise, and check rendered output. It does not depend on the date. |
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

The drills need Git 2.40 or newer and Python 3.9 or newer. The quality gate
`disaster-recovery-conformance` runs the test suite, and the
`disaster-recovery.yml` workflow runs the suite, the gate, and every drill on
Linux, macOS, and Windows, and uploads the reports. DNS leak tracing in the
`dns-loss` drill needs strace, so it runs only on Linux. Elsewhere the report
records the tracer as a non-blocking missing prerequisite.

## Not covered yet

- No exercise with people has been recorded. The closure checks for tabletop,
  live, and unfamiliar-team exercises stay open until one is.
- The drills do not build the Go mirror node, so node archive restores
  (`--restore-drill`, `--restore`) and node identity imports are documented
  but not drilled here. `server/nodeops_test.go` covers them separately.
- The `search` and `organization-service` roles are hosted-only, so
  main-service loss cannot restore search or organizations.
- Rosters and kits are signed by one kit key. There is no threshold signature
  on the kit itself.
- Desktop and mobile apps do not show the runbook, roster, or kit state.
