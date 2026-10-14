# Community reward-pool local signer

The desktop **Control node** page is the only component that can sign an
outgoing community-pool transaction. The server schedules and stores public
plans but never receives a seed, private key, passphrase, or signed transaction
bytes. Signing is an explicitly approved local flow, not an unattended hot
wallet.

## Provisioning

Server configuration (public values only): `COMMUNITY_REWARD_NETWORK`,
`COMMUNITY_REWARD_RPC_URL`, `COMMUNITY_REWARD_POOL_ADDRESS` (or
`TREASURY_SOLANA_ADDRESS`), and `REWARD_SIGNER_ACCOUNT` (fallback
`INSTANCE_OWNER_ACCOUNT`). The pool address must match the key imported on the
desktop.

On the owner's device: sign in as the signer account, open **Control node →
First-instance reward-pool signer**, import an existing 64-byte Solana CLI
keypair (JSON byte array or base58), set a strong vault passphrase, and choose
the network and a public HTTPS JSON-RPC origin. The importer rejects 32-byte
seeds, recovery phrases, and keypairs whose public half does not match; it never
generates a key. RPC URLs with credentials, query strings, paths, or plain HTTP
are rejected.

## Local vault

`reward-pool/pool-key.vault` in the application-data directory holds the public
address (authenticated as additional data), a scrypt salt (`N=32768`, `r=8`,
`p=1`), an AES-256-GCM nonce, the encrypted 32-byte Ed25519 seed, and the tag.
The file must be owner-only, symlinks are refused, and writes are atomic. The
passphrase and seed exist only during one approved operation. The key is never
written to settings, server storage, logs, or command lines. Back up the
original key with trusted wallet procedures; it cannot be recovered.

## Queue contract

```text
GET /api/rewards/signing-jobs?signer=<account>&ts=<epoch-ms>&sig=<identity-signature>
```

The identity signs `forkmesh-reward-jobs-v1\n<signer>\n<epoch-ms>`. The server
returns at most 50 jobs for the configured signer. The desktop requires custody
`external-local-signer` and validates each job: kind, status, expiry, network,
source address, plan schema 2, `custody.forkMeshHoldsUserKeys=false`,
`signing.method=external-local-qt`, an allowed policy
(`randomized-eligible-mirror-v2`, `instant-all-eligible-mirrors-v1`,
`pending-reward-claim-v1`), valid unique destinations with positive amounts
within per-policy caps, and no secret-shaped fields. Anything else stays
unsigned.

## Review and signing

**Review, sign and submit** shows source, total, every destination and amount,
policy, network, intent, and expiry; Cancel is the default. After approval the
desktop fetches a finalized blockhash and builds a legacy transaction with the
pool as sole signer and fee payer and only the reviewed System Program
transfers (at most 1,232 bytes). It reparses the serialized transaction and
rejects any deviation before unlocking the vault, then signs the exact message.

## Broadcast and reconciliation

The desktop broadcasts via `sendTransaction` with preflight, checks the returned
signature matches its own, and persists only public recovery fields. It reports
the receipt to `POST /api/rewards/signing-jobs` signed over
`forkmesh-reward-submit-v1\n<signer>\n<intent-id>\n<transaction-signature>\n<epoch-ms>`.
`202` means submitted. The server fetches the finalized transaction and marks
the intent complete only if it contains exactly the planned transfers; a
signature cannot be replayed across intents. **Reconcile last submission**
recovers after an interruption.

## Operational checks

Before funding a production pool, run `forkmesh-reward-pool-tests`, verify vault
permissions, rehearse the full flow on devnet, confirm the server account, pool
address, vault address, network, and RPC agree, and run the read-only preflight:

```bash
python3 tools/validate_reward_mainnet.py \
  --rpc-url https://api.mainnet-beta.solana.com \
  --pool-address "${COMMUNITY_REWARD_POOL_ADDRESS:-${TREASURY_SOLANA_ADDRESS:-}}"
```

It calls only `getGenesisHash`, `getAccountInfo`, and `getBalance`. Establish key
backup, compromise response, and rotation procedures before going live.
