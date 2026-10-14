# Private repository confidentiality boundary

The leakage and recovery contract for private repositories.

## Protected data

Repository objects and refs, artifacts, repository and organization names,
device labels, discussions, issues, pull requests and reviews, agent prompts
and transcripts, notifications, security findings, media, and any search terms
or indexes derived from them.

Two confidentiality boundaries apply:

- **Repository objects** are sealed on an authorized device as
  `forkmesh.private-replica` ciphertext (fresh AES-256-GCM content key per
  update, wrapped to X25519 and ML-KEM-768 recipients). Storage and replication
  operators hold no recipient key and learn neither identity nor contents.
- **Relay collaboration rows** are AES-256-GCM encrypted under the instance
  `DATA_KEY`, with a separately derived HMAC blind index for equality lookups.
  Owner-agent payloads and private rooms add recipient encryption on top. An
  operator holding the live service and `DATA_KEY` is inside this boundary; a
  database, log or backup operator without it is not.

A missing recipient key never falls back to plaintext.

## Metadata envelope

At-rest inspection may reveal only random identifiers, keyed blind indexes,
counts, ciphertext sizes, coarse status values, timestamps, digests, key epochs,
algorithm and public-key identifiers, and a private flag. These leak existence,
activity volume and equality within one key lifetime, but never names, paths,
labels, message text, Git refs or object IDs. Public-mirror history tables hold
no rows for private repositories.

Plaintext still exists in authorized memory, exported working copies and
recipients' own copies; use disk encryption alongside ForkMesh keys.

## Key lifecycle

Each repository update or recipient change creates a new content key and
advances `keyEpoch` atomically. To revoke a device or collaborator: remove the
recipient, reseal to the fresh owner-signed recipient set, publish the new
epoch, and verify the removed key cannot open it and an older epoch cannot
overwrite it. Revocation cannot erase copies already held. Keep at least two
owner recipients; organizations may escrow a recipient key in their own KMS and
enroll only its public half.

## Backup and keyless inspection

Back up `.fm-private` files, the encrypted database export, configuration and
the identity recovery kit, never plaintext checkouts or the raw `DATA_KEY`
alongside ciphertext. Restore ciphertext and schema together.

```bash
python tools/audit_private_surfaces.py backup.sqlite3 \
  --replica-root /read-only/private-replicas \
  --output artifacts/private-surface-audit.json
```

The audit takes no key and checks column declarations, blind-index shape,
payload and replica framing, digests and epoch monotonicity. Exit `0` passes,
`1` is a confidentiality finding, `2` is an input failure; an undeclared column
or incomplete coverage fails closed.

## GA evidence

Commercial general availability requires, per release candidate: private-surface
CI on Linux, macOS and Windows; keyless inspection of primary, backup, log and
replica data; revocation, rotation and recovery drills including failure paths;
content-free telemetry; and an independent cryptographic and tenant-isolation
review. Any unresolved critical or high finding, plaintext finding or missing
review blocks GA.
