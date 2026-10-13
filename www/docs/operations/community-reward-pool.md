# Community reward pool

ForkMesh's reward system is non-custodial. Production coordinates public Solana
mainnet-beta state and unsigned transfer intents; it does not create, accept, store,
decrypt, or use a wallet private key in the Cloudflare Worker or D1.

## Custody boundaries

- A contributor signs a direct transfer from their own self-custodial wallet
  to the configured public reward destination. New deployments may name it
  `COMMUNITY_REWARD_POOL_ADDRESS`; existing one-click deployments may keep
  supplying the same public address as `TREASURY_SOLANA_ADDRESS`.
- The public pool private key is imported only into the first instance owner's
  local Qt client. It must use that client's encrypted local vault/keychain
  boundary.
- D1 stores public pool addresses, public Solana Pay references, eligibility
  snapshots, unsigned transfer plans, transaction signatures, and status.
- The Qt client must show and validate every source, destination, amount,
  network, and expiry before explicit owner approval.
- The Worker independently retrieves the finalized transaction and requires
  its exact System Program transfer list to match the approved intent.
- A node operator supplies only a public payout address. ForkMesh never asks
  for a node operator's seed, recovery phrase, or private key.

User-owned funds, community-pool funds, pending rewards, and completed
on-chain transfers are separate states in the APIs and interfaces. Community
incentives are voluntary and are not investments or guaranteed returns.

## Configuration

Production configuration:

```text
COMMUNITY_REWARD_NETWORK=mainnet-beta
COMMUNITY_REWARD_RPC_URL=https://api.mainnet-beta.solana.com
# Existing ForkMesh/one-click deployment name:
TREASURY_SOLANA_ADDRESS=<existing public mainnet address>
# Optional explicit modern override:
COMMUNITY_REWARD_POOL_ADDRESS=<same or replacement public mainnet address>
REWARD_SIGNER_ACCOUNT=<instance owner account>
REWARD_INTERVAL_MS=3600000
REWARD_JITTER_MAX_MS=2700000
REWARD_AMOUNT_LAMPORTS=100000
REWARD_MIN_UPTIME_MS=1800000
REWARD_MIN_CONTRIBUTION_UNITS=0
REWARD_HEALTH_FRESH_MS=600000
```

`COMMUNITY_REWARD_POOL_ADDRESS` has priority when it is non-empty.
`TREASURY_SOLANA_ADDRESS` remains a supported compatibility fallback, so an
existing instance does not lose its configured reward destination. If the
explicit modern value is non-empty but invalid, ForkMesh fails closed instead
of silently falling back to another address. Both variables contain only a
public Solana address. A seed, recovery phrase, private key, or signed
transaction must never be placed in either variable.

The Worker refuses to expose or schedule the pool unless the network is known,
the RPC is credential-free HTTPS, `getGenesisHash` matches that exact cluster,
and `getAccountInfo` proves the configured public pool account exists there.
Known canonical RPC/cluster mismatches fail before any request. The Worker
still never signs or broadcasts. Devnet remains available only through an
explicit development configuration; it is never a production fallback.

## Voluntary contribution choices

`POST /api/rewards/contributions` with `action=prepare` creates a bounded,
one-hour public intent. The response is a reference-bound Solana Pay URI:

- `trickle` leaves the finalized contribution in the public pool for
  configurable randomized rounds.
- `instant_all_nodes` creates an unsigned, equal all-eligible-node plan after
  the incoming transfer is finalized. Size-safe batches contain at most eight
  System transfers per transaction, so every eligible node remains included
  without exceeding the legacy Solana message limit. A small explicit
  remainder covers one network fee per batch chunk and integer division.

Confirmation requires the intent ID and the public transaction signature. The
Worker checks finality, the unique reference account, exact destination, exact
amount, source address validity, replay protection, and an ordinary System
Program transfer. No outgoing plan is created while finality is unknown.

The product may describe this action as joining or funding the reward program.
It is voluntary community support, not the purchase of equity, a tokenized
ownership interest, an investment contract, or a promise of profit. Preparing
the request does not move funds: the contributor reviews and signs the direct
transfer in their own wallet, and ForkMesh never signs that incoming transfer.

## Randomized mirror rewards

Every configured interval the Worker records a public round and a reproducible
jitter offset. At execution it freezes a fresh eligibility snapshot and selects
one candidate with SHA-256 modulo using a finalized Solana blockhash. The
intent includes:

- the full accepted snapshot and its hash;
- generalized rejection reasons;
- the public blockhash and entropy source;
- the selection algorithm and selected index;
- the selected public node and payout address;
- the configured amount and expiry.

Eligibility requires all of the following:

- a fresh signed node heartbeat;
- a fresh healthy direct HTTPS endpoint;
- signed node identity registration;
- an independently challenged `forkmesh/forkmesh` refs proof matching the
  configured integrity pin;
- a valid public self-custodial payout address;
- minimum server-observed continuous uptime; and
- the optional minimum contribution-unit threshold.

Approved federated relays use the same frozen snapshot for both randomized and
`instant_all_nodes` plans. A relay may not submit a wallet-only presence claim.
It must forward the node's original signed HTTPS registration and exact
node-signed, nonce-bound `forkmesh/forkmesh` repository-health message. The
main relay independently verifies both signatures, freshness, the current refs
digest, integrity, required-operation claim, relay approval, and
its own consecutive-observation history before the row can be eligible.
Migration 0057 deletes all pre-attestation federated presence rows.
The approved relay remains a disclosed, revocable trust boundary for whether
the remote network challenge was actually performed; its approval is not proof
of personhood or universal node trust. Remove relay approval to exclude its
forwarded nodes from future snapshots.

The policy de-duplicates node identity, operator identity, device public key,
and wallet. Signup throttles, signed device identity, external health checks,
server-observed uptime, integrity proofs, and those
de-duplication gates raise the cost of fake nodes, repeated accounts,
artificial uptime, wallet farming, and reward manipulation. They reduce but do
not eliminate Sybil risk; the public policy says so explicitly.

## Pending walletless rewards

The configured instance owner may create a signed pending allocation through
`POST /api/rewards/pending-awards`. It is metadata only:

- no wallet is created;
- no funds move out of the external pool;
- only an active recipient without a payout wallet is eligible;
- the allocation expires exactly 24 hours after creation;
- the recipient may submit only a public address through
  `POST /api/rewards/pending`; and
- expiry releases the reservation while funds remain at the source.

After an address is supplied, a separate unsigned intent enters the same local
signer and finalized-transaction verification flow.

## Public inspection

`GET /api/rewards/pool` and the World fountain display:

- network, public address, explorer link, and live public balance;
- randomized timing/amount configuration;
- contribution modes;
- recent finalized incoming contributions;
- recent finalized recipient transfers; and
- explicit fund-state and non-custodial notices.

The 3D fountain's light and coin particles are illustrative. Only a finalized
transaction signature linked to the configured cluster is an on-chain event.

`GET /api/accounts/central-fund` remains a compatibility alias for that same
canonical public payload, including its non-custodial notice and four explicit
fund states. It never reads the historical encrypted `central_fund` row.

`GET /api/accounts/treasury-address` and the relay-to-relay
`/api/federation/treasury-address` alias are retired. They return HTTP 410,
an empty address, and migration-only guidance instead of exposing an
operational legacy custodial deposit target. This does not remove
`TREASURY_SOLANA_ADDRESS`: that deployment variable remains a public-address-only
compatibility input for the non-custodial community reward pool described
above. Historical treasury, signup-deposit, bounty, and central-fund keys are
handled only by
`www/docs/operations/legacy-solana-custody-migration.md`.
