# Legacy Solana custody migration

ForkMesh’s live Worker is non-custodial. It does not create, accept, import,
sign with, or broadcast a Solana wallet private key. Older deployments may
still have AES-GCM-encrypted wallet seeds in compatibility rows created by the
former signup-deposit, issue-bounty, owner bounty-wallet, federated-signup, or
central-fund flows.

Those rows are not an operational compatibility mode. Migration 0057 clears
the readiness marker, and the upgraded Worker decrypts every historical
key-bearing shape before enabling its D1-backed API. If any wallet key remains,
if a relevant ciphertext is unreadable, or if the scan is incomplete, startup
fails closed with `legacy_custody_migration_required`. No live request, cron,
status sample, or admin action may claim ready while that condition remains.

The offline tool at `tools/legacy_solana_custody.py` provides a fail-closed
migration. It never signs or submits a transaction, never moves funds, never
prints a seed, and never contacts D1. It operates only on an explicitly exported
SQLite snapshot.

## Safety invariants

- Use a trusted, offline or tightly controlled owner machine.
- Quiesce Worker writes while taking the final snapshot and applying the scrub.
- Back up D1 before doing anything. Keep that backup encrypted and access
  controlled until the migration and recovery-retention period are complete.
- Supply `DATA_KEY`, artifact passphrase, and any authenticated RPC URL through
  hidden prompts or environment variables, never command-line arguments.
- Use a new artifact passphrase of at least 16 characters. It must differ from
  `DATA_KEY`. ForkMesh cannot recover it.
- Review the redacted inventory. It contains public addresses and balances but
  no key material.
- Import the encrypted artifact only into an owner-controlled encrypted local
  workflow or a reviewed keychain/hardware-wallet migration workflow.
- Move funds manually with trusted wallet tooling. The ForkMesh tool will not do
  this and does not prescribe a destination.
- Do not scrub D1 while any inventoried address has a nonzero or unavailable
  balance.
- Do not send new funds to a historical deposit or bounty address.

## Prerequisites

Install Python 3.12+, `cryptography`, Wrangler, and SQLite on the migration
machine. Apply migration `0049_legacy_custody_audit.sql` (or deploy the matching
Worker schema) before applying the final scrub.

Set a restrictive shell umask:

```sh
umask 077
```

Export D1 without changing it, then build a local SQLite snapshot:

```sh
npx wrangler d1 export forkmesh --remote --output forkmesh-d1-export.sql
sqlite3 forkmesh-d1-snapshot.sqlite < forkmesh-d1-export.sql
```

Use the actual database binding/name for the instance. Never use a production
export on a shared workstation.

Set secrets without placing them in shell history:

```sh
read -rsp "Historical DATA_KEY: " FORKMESH_LEGACY_DATA_KEY
export FORKMESH_LEGACY_DATA_KEY
read -rsp "New artifact passphrase: " FORKMESH_CUSTODY_ARTIFACT_PASSPHRASE
export FORKMESH_CUSTODY_ARTIFACT_PASSPHRASE
export FORKMESH_SOLANA_RPC_URL="https://your-trusted-solana-rpc.example"
```

The RPC must be HTTPS. The selected `--network` must match the network on which
the historical addresses were funded.

## 1. Inventory and reconcile

```sh
python tools/legacy_solana_custody.py inventory \
  --database forkmesh-d1-snapshot.sqlite \
  --network mainnet-beta \
  --output forkmesh-custody-audit.json
```

The command decrypts every row in each compatibility table, verifies that every
stored seed derives its adjacent public address, deduplicates repeated
addresses, and performs a finalized `getBalance` lookup. A wrong data key,
malformed row, seed/address mismatch, RPC failure, or partial response aborts
the whole inventory. The tool does not silently skip an unreadable row.

Review:

- `keyBearingRecordCount`
- `uniqueAddressCount`
- every `sourceTable`
- every public address and finalized balance
- the selected network and redacted RPC origin

## 2. Export custody to an encrypted owner artifact

```sh
python tools/legacy_solana_custody.py export \
  --database forkmesh-d1-snapshot.sqlite \
  --network mainnet-beta \
  --artifact forkmesh-legacy-custody.fmcustody \
  --audit-output forkmesh-custody-export-audit.json
```

The `.fmcustody` file is JSON framing around an AES-256-GCM ciphertext. Its key
is derived from the new passphrase with scrypt. Public framing contains only
format/version identifiers, counts, a redacted-audit digest, and encryption
parameters. Seeds and D1 row locators exist only inside the authenticated
ciphertext. The file is created with owner-only permissions and is never
overwritten.

Copy this artifact through the owner’s encrypted backup/keychain process and
verify that the owner can open/import it in the chosen offline workflow before
continuing. Hardware wallets generally do not import arbitrary legacy seeds;
an operator may instead use trusted offline wallet software to transfer each
balance to a hardware-wallet address. That decision and every signature remain
outside ForkMesh.

The tool intentionally has no “sweep” command.

## 3. Move funds externally and reconcile at zero

Use trusted owner-controlled wallet tooling to handle each artifact entry.
Record public transaction signatures in the operator’s incident/change record.
Do not paste keys into ForkMesh, a browser form, a ticket, chat, or a log.

Run `inventory` again against a fresh snapshot until every listed public address
has a finalized balance of exactly zero. A fee reserve is still a nonzero
balance and blocks scrubbing.

## 4. Explicitly confirm

Obtain the artifact SHA-256 printed by `export` and the `auditSha256` value in
the artifact’s public framing. Create an owner-only confirmation file:

```json
{
  "format": "forkmesh-legacy-solana-scrub-confirmation-v1",
  "artifactSha256": "64-lowercase-hex-characters",
  "auditSha256": "64-lowercase-hex-characters",
  "confirmedAt": 1784815200000,
  "confirmation": "I HAVE IMPORTED EVERY LEGACY KEY AND RECONCILED EVERY ADDRESS AT ZERO"
}
```

This separate file is the explicit authorization to prepare a scrub. Creating
an inventory or encrypted artifact is not authorization to delete a key.

## 5. Prepare and review the scrub

Take a fresh, quiesced D1 export and run:

```sh
python tools/legacy_solana_custody.py prepare-scrub \
  --database forkmesh-d1-snapshot-final.sqlite \
  --network mainnet-beta \
  --artifact forkmesh-legacy-custody.fmcustody \
  --confirmation forkmesh-custody-confirmation.json \
  --output-sql forkmesh-legacy-custody-scrub.sql
```

The tool reopens the artifact, verifies the confirmation digests and phrase,
requires the current key-bearing record set to match the artifact exactly, and
performs fresh finalized balance reads. Any nonzero or unavailable balance
aborts. It then writes—but does not apply—an owner-only SQL patch.

The patch:

- removes `donation_secret` and obsolete `payout_tx` from active account rows;
- deletes key-bearing rows from the retired `federated_signup`,
  `issue_bounty`, `bounty_wallet`, and `central_fund` compatibility tables
  after their exact public reconciliation evidence has been prepared;
- uses the original ciphertext in every `UPDATE` or `DELETE` predicate so a
  concurrent row change cannot be overwritten;
- deliberately fails the transaction unless each first application changes
  exactly one audited row (or an idempotent re-application finds the exact
  prior reconciliation evidence); and
- inserts a content-free receipt into
  `legacy_custody_migration_audit`;
- inserts only opaque-row-id, public-address, finalized-zero-balance evidence
  into `legacy_custody_reconciliation`;
- writes `schema_meta.legacy_wallet_custody_v2` only after every guarded update
  and receipt succeeds; and
- is idempotent when the exact reviewed SQL is applied again, while still
  aborting if any target row diverges from either its audited source ciphertext
  or exact scrubbed ciphertext.

The SQL contains encrypted replacement blobs and opaque database locators but
no plaintext wallet key. Do not add `BEGIN TRANSACTION` or `COMMIT`: D1’s SQL
file ingestion already supplies the transaction, and explicit transaction
statements make `wrangler d1 execute --file` fail.

## 6. Apply explicitly and verify

Only after review, apply the SQL through the operator’s normal D1 change
process:

```sh
npx wrangler d1 execute forkmesh --remote \
  --file forkmesh-legacy-custody-scrub.sql
```

Export D1 again and rerun `inventory`. It must report zero key-bearing records.
Also verify one `status='scrubbed'` receipt with matching digests in
`legacy_custody_migration_audit`, one zero-balance public reconciliation row per
artifact record, and a `clean-v2:` readiness marker. Only then may the upgraded
Worker report its D1-backed API ready.

If the optimistic update fails, do not edit out the guard. Export a fresh
snapshot, determine what changed, repeat the audit, and issue a new artifact and
confirmation if the key-bearing record set changed.

## Historical compatibility fields

Active account records remain, but after the scrub none may contain a
wallet-key field:

- `users.data` / `nodes.data`: historical `donation_address`,
  `donation_*` status fields, and—until scrubbed—`donation_secret`.
- Legacy `accounts.data`, if an export predates migration 0042.

Key-bearing `federated_signup`, `issue_bounty`, `bounty_wallet`, and
`central_fund` rows are removed by the explicit scrub. Their only retained D1
evidence is the key-free public address, finalized zero balance, RPC slot,
reconciliation timestamp, source-table label, and audit/artifact digests in
`legacy_custody_reconciliation`. The empty tables remain schema compatibility,
not active custody features.
