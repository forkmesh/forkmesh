# Community reward pool

The reward system is non-custodial. The server coordinates public Solana state
and unsigned transfer intents; it never creates, accepts, stores, or uses a
wallet private key.

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
