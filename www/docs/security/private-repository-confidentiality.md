# Private repository confidentiality boundary

This document is the normative leakage and recovery contract for private
ForkMesh repositories. A commercial-GA claim requires a keyless inspection
report for the deployed database backup and replica fleet, successful
revocation/recovery drills, and an independent cryptographic and tenant
isolation review. Passing repository tests alone is not that external review.

## Protected data

The protected set includes repository objects and refs, artifact bytes and
names, repository and organization names, source-device labels, discussions,
issues, pull requests, review bodies and decisions, agent prompts and
transcripts, notification text, security findings, media, and search terms or
indexes derived from any of them.

ForkMesh uses two different confidentiality boundaries:

- Private repository objects are sealed on an authorized device as
  `forkmesh.private-replica` ciphertext. A storage or replication operator has
  no recipient key and can copy, hash, retain, and restore the bytes without
  learning the repository identity or contents.
- Relay collaboration rows are AES-256-GCM encrypted under the instance
  `DATA_KEY`; equality lookups use a separately derived HMAC blind-index key.
  Owner-agent payloads and private/direct room frames add recipient encryption
  before reaching that layer. An instance operator controlling the live Worker
  and `DATA_KEY` is inside the relay-row trust boundary, but a database,
  snapshot, log, or backup operator without that secret is not.

There is no plaintext fallback when a required owner or room recipient key is
missing. Legacy agent rows are held for an explicit owner-device migration and
are not decrypted by a policy that requires end-to-end encryption.

## Declared metadata envelope

At-rest inspection may reveal only:

- random row, request, scan, queue, and opaque replica identifiers;
- keyed 256-bit blind indexes used for exact equality and tenant joins;
- bounded row counts, ciphertext sizes, coarse status enums, and timestamps;
- read/lease state, retry counts, and non-content feature flags;
- ciphertext and exact-file SHA-256 digests, key epochs, algorithms, and public
  recipient key identifiers; and
- whether a catalog row is private, needed to reject it before decryption on
  anonymous paths.

These fields leak existence, approximate activity time and volume, recipient
set cardinality, and equality within one instance-key lifetime. They must not
contain names, paths, device labels, message snippets, Git refs, object IDs, or
hashes derived from private Git refs. A `DATA_KEY` rotation changes blind
indexes and therefore breaks equality across independently exported epochs.

Private source authority is stored in encrypted `data`; its legacy `node_id`
and `machine_name` columns are blank. Private secondary-device lookup uses a
blind index. `repo_state_history` is public-mirror machinery and must have no
row for a private repository. Private integrity instead binds an opaque route
to the owner-signed encrypted-manifest hash and monotonically increasing key
epoch.

Authorized runtime plaintext can still reach memory, an explicitly exported
working copy, an unlocked device's swap/crash dump, or an old copy retained by
a former collaborator. Application encryption cannot retract data a recipient
already copied. Use device disk encryption and operating-system crash-dump
controls in addition to ForkMesh keys.

## Key lifecycle

The first recipient bundle is the owner authority. It contains public X25519
and ML-KEM-768 material; private halves stay in the local encrypted identity
vault. Every repository update and recipient rotation creates a fresh random
AES-256-GCM content key and advances `keyEpoch` atomically.

For device revocation or collaborator removal:

1. Revoke the recipient key or remove the share while an owner device remains
   available.
2. Fetch the fresh, owner-signed recipient set and reseal to exactly that set.
3. Publish the new encrypted manifest and opaque route at the new epoch.
4. Verify the removed key cannot open the new bytes and that every retained
   recipient can.
5. Import the new ciphertext into a second replica and verify an older epoch
   cannot overwrite it.

Revocation denies future epochs and downloads. It cannot erase an older
ciphertext or materialized checkout already held by the removed recipient.
Audit history is retained as signed, content-free key/share events rather than
by retaining removed recipient access.

Organization-managed deployments should escrow an organization recipient
private key in their approved HSM/KMS and enroll only its public bundle. The
KMS wrapping, quorum, export, and deletion policy remain organization-owned;
ForkMesh never accepts the private key through its relay APIs. Keep at least
two current owner recipients before rotation so one failed device does not
make the ciphertext unavailable.

## Backup and recovery

Back up exact `.fm-private` files, the encrypted D1 export, configuration, and
the separately protected identity recovery kit. Never place a plaintext Git
materialization or raw `DATA_KEY` in the ciphertext backup. Protect the
`DATA_KEY` recovery copy with a different administrative control and verify its
mode/ownership using the bootstrap runbook.

A quarterly recovery drill restores the D1 export and opaque replicas into an
isolated environment, runs the keyless audit, restores one authorized identity,
opens the newest epoch, and checks Git refs and collaboration event signatures.
It then attempts the same open with a revoked identity and attempts an older
epoch import. Record durations and only content-free outcomes; do not attach
repository names, object IDs, paths, ciphertext, keys, or decrypted output.

## Keyless inspection

Restore the D1 SQL export to a SQLite file, mount replica directories read-only,
and run:

```bash
python tools/audit_private_surfaces.py backup.sqlite3 \
  --replica-root /read-only/private-replicas \
  --output artifacts/private-surface-audit.json
```

The command takes no key. It validates every declared table column, blind-index
shape, encrypted payload framing, absence of private refs indexes and authority
labels, replica hybrid-envelope framing and digest, and monotonic duplicate
epochs. Reports contain only counts, row ordinals, and stable finding codes.
Exit `0` means complete inspection passed; `1` means a confidentiality finding;
`2` means input or inspection failure. `coverage_incomplete`, a missing table,
or an undeclared new column fails closed.

Run the audit on the primary export, each backup class, and a sample from every
replica operator. A rollback restores ciphertext and schema together; never
roll back only the database after a recipient rotation because epoch rollback
protection will correctly reject the older row.

## GA evidence

Before commercial GA, link all of the following to the release candidate:

- Linux, macOS, and Windows private-surface CI artifacts;
- production-like primary, backup, index, log, and replica inspection reports;
- add/remove/revoke/rotate drills with rotation time and denial outcomes;
- encrypted backup restore and owner-loss recovery duration and success;
- rollback, corrupt ciphertext, wrong key, missing key, stale epoch, and partial
  outage failure-path runs;
- current user and operator documentation;
- content-free telemetry for coverage percentage, rotation duration, recovery
  success, plaintext finding count, and unresolved security findings; and
- an independent review report covering the algorithms, implementation,
  recipient authentication, nonce/key generation, key erasure limits, tenant
  authorization, blind-index leakage, caches/logging, and operator threat model.

Any unresolved critical/high finding, plaintext finding, incomplete supported
platform, missing recovery run, or absent independent review blocks GA. Review
artifacts may be access-controlled, but their reviewer, scope, candidate commit,
date, disposition, and report digest must be recorded publicly.
