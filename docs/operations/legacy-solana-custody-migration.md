# Legacy Solana custody migration

The live server is non-custodial: it never creates, accepts, signs with, or
broadcasts a Solana private key. Older deployments may still hold encrypted
wallet seeds in compatibility rows from retired signup-deposit, issue-bounty,
bounty-wallet, federated-signup, or central-fund flows. Until every such key is
migrated and scrubbed, the server refuses to start its database-backed API and
reports `legacy_custody_migration_required`.

`tools/legacy_solana_custody.py` performs the migration offline against an
exported SQLite snapshot. It never signs or submits a transaction, never moves
funds, never prints a seed, and never contacts the live database.

## Safety invariants

- Work on a trusted, tightly controlled machine with `umask 077`; quiesce
  server writes for the final snapshot; keep an encrypted database backup.
- Pass `DATA_KEY`, the artifact passphrase, and any RPC URL through hidden
  prompts or environment variables (`FORKMESH_LEGACY_DATA_KEY`,
  `FORKMESH_CUSTODY_ARTIFACT_PASSPHRASE`, `FORKMESH_SOLANA_RPC_URL`), never
  arguments. The passphrase must be new, at least 16 characters, and differ
  from `DATA_KEY`.
- Move funds manually with trusted wallet tooling.
- Never scrub while any inventoried address has a nonzero or unknown balance,
  and never send new funds to a historical address.

## Procedure

1. **Inventory.** Export the database to SQLite, then run `inventory --database
   <snapshot> --network mainnet-beta --output audit.json`. Every row is
   decrypted, each seed must derive its stored address, and finalized balances
   are fetched; any error aborts the whole run.
2. **Export.** `export ... --artifact legacy.fmcustody` writes an AES-256-GCM
   artifact keyed by scrypt from the new passphrase. Seeds exist only inside the
   ciphertext; the file is owner-only and never overwritten. Verify the owner
   can open it before continuing. The tool intentionally has no “sweep” command.
3. **Move funds externally** and rerun `inventory` on fresh snapshots until
   every address reports exactly zero.
4. **Confirm.** Create an owner-only confirmation file:

   ```json
   {
     "format": "forkmesh-legacy-solana-scrub-confirmation-v1",
     "artifactSha256": "64-lowercase-hex-characters",
     "auditSha256": "64-lowercase-hex-characters",
     "confirmedAt": 1784815200000,
     "confirmation": "I HAVE IMPORTED EVERY LEGACY KEY AND RECONCILED EVERY ADDRESS AT ZERO"
   }
   ```

5. **Prepare the scrub.** `prepare-scrub --database <final snapshot> --artifact
   ... --confirmation ... --output-sql scrub.sql` re-verifies the artifact,
   digests, phrase, exact record set, and zero balances, then writes (but does
   not apply) a SQL patch. The patch removes key fields and key-bearing rows
   using the original ciphertext in every predicate, aborts unless each change
   hits exactly one audited row, records key-free evidence in
   `legacy_custody_reconciliation` and a receipt in
   `legacy_custody_migration_audit`, sets the readiness marker last, and is
   idempotent. Do not add `BEGIN`/`COMMIT`; the database's file ingestion
   supplies the transaction.
6. **Apply and verify** through the normal database change process, then export
   again: `inventory` must find zero key-bearing records, the audit table must
   hold one `scrubbed` receipt, and the readiness marker must be `clean-v2:`.

If the guarded update fails, never remove the guard: take a fresh snapshot,
determine what changed, and repeat the audit.

## Retained data

After the scrub, account rows keep only historical public donation fields, and
the retired compatibility tables are empty. The only evidence kept is the public
address, finalized zero balance, RPC slot, timestamp, source-table label, and
digests.
