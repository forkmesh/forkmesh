# Community reward pool

Part of [Operations](README.md).

The reward system is non-custodial. The server coordinates public Solana state
and unsigned transfer intents; it never creates, accepts, stores, or uses a
wallet private key. The desktop's local signer ([below](#local-signer)) is
the only component that signs an outgoing pool transaction.

## Custody boundaries

- Contributors sign direct transfers from their own wallets to the public pool
  address.
- The pool private key lives only in the instance owner's desktop client,
  inside its encrypted local vault. The client shows and validates every
  source, destination, amount, network, and expiry before the owner approves.
- The database stores only public addresses, Solana Pay references, eligibility
  snapshots, unsigned plans, transaction signatures, and status.
- The server fetches each finalized transaction and requires its System Program
  transfer list to match the approved intent exactly.
- Node operators supply only a public payout address.

User funds, pool funds, pending rewards, and completed transfers are separate
states. Contributions are voluntary community support, not investments.

## Configuration

```text
COMMUNITY_REWARD_NETWORK=mainnet-beta
COMMUNITY_REWARD_RPC_URL=https://api.mainnet-beta.solana.com
COMMUNITY_REWARD_POOL_ADDRESS=<public address>   # or TREASURY_SOLANA_ADDRESS
REWARD_SIGNER_ACCOUNT=<instance owner account>
REWARD_INTERVAL_MS=3600000
REWARD_JITTER_MAX_MS=2700000
REWARD_AMOUNT_LAMPORTS=100000
REWARD_MIN_UPTIME_MS=1800000
REWARD_MIN_CONTRIBUTION_UNITS=0
REWARD_HEALTH_FRESH_MS=600000
```

`COMMUNITY_REWARD_POOL_ADDRESS` wins when set; an invalid value fails closed
rather than falling back. The pool is exposed only when the network is known,
the RPC is credential-free HTTPS, `getGenesisHash` matches the cluster, and the
pool account exists. Devnet requires explicit development configuration.

## Contributions

`POST /api/rewards/contributions` with `action=prepare` returns a one-hour,
reference-bound Solana Pay URI:

- `trickle` leaves the contribution in the pool for randomized rounds;
- `instant_all_nodes` creates an equal unsigned plan for every eligible node,
  in batches of at most eight transfers.

Confirmation checks finality, the reference, destination, amount, replay, and
that it is an ordinary System transfer. No outgoing plan is created while
finality is unknown.

## Randomized mirror rewards

Each interval the server records a round with a reproducible jitter, freezes an
eligibility snapshot, and selects one node by SHA-256 modulo over a finalized
blockhash. The intent publishes the snapshot and its hash, rejection reasons,
entropy, algorithm, selected index, payee, amount, and expiry.

Eligibility requires a fresh signed heartbeat, a healthy direct HTTPS endpoint,
signed identity registration, an independently challenged refs proof matching
the integrity pin, a public payout address, minimum observed uptime, and the
optional contribution threshold. Federated relays must forward each node's
original signed registration and nonce-bound health message; relay approval is
a disclosed, revocable trust boundary. Identity, operator, device key, and
wallet are de-duplicated. These gates reduce Sybil risk but do not eliminate it.

## Pending walletless rewards

The instance owner can create a pending allocation
(`POST /api/rewards/pending-awards`) for an active recipient without a payout
wallet. It moves no funds, expires after 24 hours, and lets the recipient
submit a public address through `POST /api/rewards/pending`.

## Public inspection

`GET /api/rewards/pool` (and its alias `GET /api/accounts/central-fund`) shows
network, address, live balance, timing and amount settings, contribution modes,
recent finalized transfers, and the non-custodial notice. Retired custodial
deposit endpoints return HTTP 410. Legacy custodial keys are handled in
[Legacy Solana custody migration](legacy-solana-custody-migration.md).

## Local signer

The desktop **Control node** page ([desktop control node](desktop-control-node.md))
is the only component that can sign an outgoing community-pool transaction.
The server schedules and stores public plans but never receives a seed,
private key, passphrase, or signed transaction bytes. Signing is an
explicitly approved local flow, not an unattended hot wallet.

### Provisioning

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

### Local vault

`reward-pool/pool-key.vault` in the application-data directory holds the public
address (authenticated as additional data), a scrypt salt (`N=32768`, `r=8`,
`p=1`), an AES-256-GCM nonce, the encrypted 32-byte Ed25519 seed, and the tag.
The file must be owner-only, symlinks are refused, and writes are atomic. The
passphrase and seed exist only during one approved operation. The key is never
written to settings, server storage, logs, or command lines. Back up the
original key with trusted wallet procedures; it cannot be recovered.

### Queue contract

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

### Review and signing

**Review, sign and submit** shows source, total, every destination and amount,
policy, network, intent, and expiry; Cancel is the default. After approval the
desktop fetches a finalized blockhash and builds a legacy transaction with the
pool as sole signer and fee payer and only the reviewed System Program
transfers (at most 1,232 bytes). It reparses the serialized transaction and
rejects any deviation before unlocking the vault, then signs the exact message.

### Broadcast and reconciliation

The desktop broadcasts via `sendTransaction` with preflight, checks the returned
signature matches its own, and persists only public recovery fields. It reports
the receipt to `POST /api/rewards/signing-jobs` signed over
`forkmesh-reward-submit-v1\n<signer>\n<intent-id>\n<transaction-signature>\n<epoch-ms>`.
`202` means submitted. The server fetches the finalized transaction and marks
the intent complete only if it contains exactly the planned transfers; a
signature cannot be replayed across intents. **Reconcile last submission**
recovers after an interruption.

### Operational checks

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
