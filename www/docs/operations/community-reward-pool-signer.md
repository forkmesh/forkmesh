# Community reward-pool local signer

The Qt **Control node** page is the only ForkMesh component that can sign an
outgoing community-pool transaction. The Cloudflare Worker schedules and stores
public plans, but it never receives a seed, private key, passphrase, or signed
transaction bytes.

This is an explicitly approved local signing flow, not an unattended hot
wallet. ForkMesh does not hold or control a contributor's or recipient's funds.
Community rewards are voluntary incentives, not investments, and do not
guarantee a return.

## Provision the first-instance owner

Configure the Worker with public values:

```text
COMMUNITY_REWARD_NETWORK=mainnet-beta
COMMUNITY_REWARD_RPC_URL=https://api.mainnet-beta.solana.com
TREASURY_SOLANA_ADDRESS=<existing public Solana address>
# Optional explicit modern override:
COMMUNITY_REWARD_POOL_ADDRESS=<same or replacement public Solana address>
REWARD_SIGNER_ACCOUNT=<first instance owner's ForkMesh account>
```

`INSTANCE_OWNER_ACCOUNT` is accepted as the signer-account fallback. The pool
address must match the key imported into Qt. The modern address variable takes
priority when non-empty; otherwise the existing `TREASURY_SOLANA_ADDRESS`
configuration remains active. Both are public-address-only settings.
Production defaults to `mainnet-beta`; devnet/testnet require an explicit
development selection and must use a matching RPC.

On the first-instance owner's device:

1. Sign in with the configured ForkMesh owner identity.
2. Open **Control node → First-instance reward-pool signer**.
3. Import an existing Solana CLI 64-byte keypair, either as its JSON byte array
   or base58 encoding.
4. Set a strong, unique local vault passphrase.
5. Select the exact network and enter an unauthenticated public HTTPS Solana
   JSON-RPC origin, then save it locally.

The importer deliberately rejects 32-byte seeds, recovery phrases, malformed
keypairs, and keypairs whose embedded public half does not match the private
seed. It never generates a pool key.

Do not place an RPC API key in the URL. The desktop rejects user information,
query strings, fragments, non-root paths, HTTP, and token-shaped RPC paths.
Authenticated RPC support requires a separate reviewed credential-store design.

## Local vault boundary

The vault is stored below the Qt application-data directory at
`reward-pool/pool-key.vault`. It contains:

- a clear public address used to select the correct queue;
- a random scrypt salt;
- a random AES-256-GCM nonce;
- the authenticated encrypted 32-byte Ed25519 seed; and
- the GCM authentication tag.

Scrypt uses `N=32768`, `r=8`, and `p=1`. The public address is authenticated as
additional data, so editing it invalidates decryption. The vault directory and
file must be reducible to owner-only permissions, symbolic links are refused,
and replacement uses an atomic save. An unavailable KDF, cipher, secure
location, or permission boundary fails closed.

The passphrase and decrypted seed exist only during one approved operation and
are cleansed from process-owned buffers afterward. The private key is never
stored in `QSettings`, a Worker secret, D1, logs, request bodies, or command-line
arguments. The current implementation uses a passphrase-encrypted local vault;
it does not claim hardware-wallet or operating-system-keychain protection.

Back up the original self-custodial key using trusted wallet procedures.
ForkMesh cannot recover either the key or the vault passphrase.

## Worker queue contract

The desktop fetches plans with:

```text
GET /api/rewards/signing-jobs?signer=<account>&ts=<epoch-ms>&sig=<identity-signature>
```

The local ForkMesh identity signs these exact UTF-8 bytes:

```text
forkmesh-reward-jobs-v1
<signer>
<epoch-ms>
```

The Worker authorizes only `REWARD_SIGNER_ACCOUNT` (or the instance-owner
fallback) and returns at most 50 `pending_signature` or `submitted` jobs. The
desktop requires the top-level custody value `external-local-signer`, then
independently validates every job:

- supported kind, status, intent ID, creation time, and short expiry;
- exact configured network and source pool address;
- plan schema version 2 and an allowed reward policy;
- `custody.forkMeshHoldsUserKeys=false`;
- `custody.fundsRemainInSourceWalletUntilSigned=true`;
- `signing.required=true`, `signing.performed=false`, and
  `signing.method=external-local-qt`;
- an exact reviewed kind/policy pair: randomized node rewards
  (`randomized-eligible-mirror-v2`), instant eligible-node batches
  (`instant-all-eligible-mirrors-v1`), or pending claims
  (`pending-reward-claim-v1`);
- one or more unique, valid destination addresses and positive integer
  lamport amounts, with one destination for randomized rewards and pending
  claims, at most eight destinations for an instant batch, a 10 SOL cap for
  randomized/claim jobs, and a 100 SOL cap for instant batches; and
- absence of fields named like secrets, private keys, seeds, mnemonics, or
  keypairs.

Malformed, expired, duplicate, mismatched, or unsupported jobs remain unsigned.

## Explicit review, construction, and signing

Selecting **Review, sign and submit** displays the source, total, every
destination and amount, transfer count, policy, network, intent ID, and expiry.
Cancel is the default. Approval applies only to that exact plan.

After approval, Qt fetches a finalized recent blockhash directly from the saved
Solana RPC. It constructs a legacy transaction locally with:

- the pool address as the only signer and fee payer;
- only ordered System Program `transfer` instructions;
- exactly the reviewed destinations and lamport amounts;
- no arbitrary program, memo, extra account, or extra instruction; and
- an exact message SHA-256 bound into the local intent.

The serialized transaction is limited to 1,232 bytes and currently to at most
20 transfers. The Worker currently emits smaller chunks (at most eight).
Oversized plans are refused; the Worker must split them into separate reviewed
intents.

Qt reparses the complete serialized transaction before unlocking the vault. It
rejects pre-existing signatures, source or blockhash changes, reordered or
extra transfers, unexpected account flags, trailing bytes, and checksum
changes. The user then enters the vault passphrase for that operation. Qt signs
only the exact serialized Solana message with Ed25519.

## Broadcast and reconciliation

Qt sends the signed transaction directly to the configured Solana RPC using
`sendTransaction` with preflight enabled. It requires the RPC's returned
signature to equal the locally derived transaction signature. Only these public
recovery fields are persisted:

- intent ID;
- public transaction signature;
- pool address;
- network;
- public RPC origin; and
- submission time.

The signed transaction bytes and vault passphrase are not persisted. Qt submits
the public receipt to:

```text
POST /api/rewards/signing-jobs
```

with `signer`, `intentId`, `transactionSignature`, `ts`, and `sig`. The identity
signature covers these exact UTF-8 bytes:

```text
forkmesh-reward-submit-v1
<signer>
<intent-id>
<transaction-signature>
<epoch-ms>
```

A `202` response means submitted, not completed. Qt polls
`getSignatureStatuses` directly until finality and retries the public receipt.
The Worker then independently calls finalized `getTransaction` and marks the
intent complete only when the on-chain transaction contains exactly the
planned, ordered System Program transfers from the configured pool. Mismatches
are rejected and transaction signatures cannot be replayed across intents.

Use **Reconcile last submission** after a desktop or network interruption. A
visual fountain animation is never evidence of payment; only a finalized
signature for the configured network is a completed on-chain transfer.

## Operational checks

Before funding a production pool:

- build and run `forkmesh-reward-pool-tests`;
- verify the Qt vault path and owner-only permissions on the deployment OS;
- test fetch, reject, approval, broadcast, restart, and reconciliation with an
  explicitly isolated devnet development deployment;
- run read-only mainnet `getGenesisHash`, `getAccountInfo`, `getBalance`, and
  dry-run intent validation before enabling locally approved signing;
- verify the configured Worker account, pool address, Qt vault address,
  network, and RPC all agree;
- verify Cloudflare/D1 contains no private-key-shaped data;
- monitor rejected plans and failed finality without logging response bodies;
- establish key backup, compromise response, and rotation procedures; and
- retain enough pool balance for both reviewed transfers and network fees.

Run the read-only mainnet preflight before enabling local signing:

```bash
python3 tools/validate_reward_mainnet.py \
  --rpc-url https://api.mainnet-beta.solana.com \
  --pool-address "${COMMUNITY_REWARD_POOL_ADDRESS:-${TREASURY_SOLANA_ADDRESS:-}}"
```

The preflight calls only `getGenesisHash`, `getAccountInfo`, and `getBalance`.
It accepts no key and cannot create, sign, simulate, or broadcast a transaction.

The repository test suite uses deterministic local fixtures and does not send a
live Solana transaction. A successful unit test is not a production key
ceremony or security audit.
