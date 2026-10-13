# Custodial deposit addresses for world-element purchases

The World element store sells elements two ways. Operators choose whether the
second one exists.

| | Direct | Deposit |
|---|---|---|
| Buyer pays | the published pool address | a temporary ForkMesh address |
| Who holds the key | the buyer | **ForkMesh**, until the sweep lands |
| Confirmation | buyer pastes the transaction signature | automatic, the address is watched |
| Mirror half | unsigned intent, signed on the operator's device | paid by the sweep transaction |

## What "custodial" means here, precisely

When a buyer picks the deposit option, the Worker generates a fresh Ed25519
keypair, stores the 32-byte seed encrypted in `world_element_purchases`, and
gives the buyer the public key as a payment address. Once the address is
funded, the Worker signs one transaction sending **half to the treasury and
half to online functioning mirror nodes**, then clears `deposit_secret`.

Between minting and sweeping, ForkMesh can move those funds — and so can
anyone who compromises the Worker, the D1 database, or the `DATA_KEY`. That
window is normally seconds, but a failed sweep extends it until cron retries
(every 5 minutes). Only funds sitting in un-swept deposit addresses are at
risk; the community reward pool is unaffected, because its signing key still
lives only in the operator's local Qt client.

The store labels this option with its own custody text before purchase, and
`/api/world/store/catalog` returns a `paymentMethods` array where the deposit
entry carries `forkMeshHoldsDepositKey: true`. Do not describe this option as
non-custodial.

## Enabling and disabling

The path is **off unless a treasury address is configured**:

```
TREASURY_SOLANA_ADDRESS = "<public treasury address>"   # or COMMUNITY_REWARD_POOL_ADDRESS
```

To keep a treasury configured but refuse custodial deposits entirely:

```
CUSTODIAL_DEPOSITS = "0"
```

With deposits off the store still works; buyers just see the direct option
only, and no key is ever generated.

## Safety properties worth keeping

- `_solana_rpc` (the shared read helper) still cannot send. Broadcasting goes
  through `_solana_rpc_write`, which accepts `sendTransaction` and nothing else.
- The sweep is idempotent on `sweep_signature` and clears the seed on success.
- Two concurrent pollers cannot double-send: identical transactions dedupe by
  signature on chain, and a rolled blockhash makes the second attempt fail for
  insufficient funds.
- Payees come from the signed eligibility snapshot (`_eligible_reward_snapshot`),
  not raw presence rows, so only verified online mirrors are paid, capped at
  `MAX_SWEEP_PAYEES`.
- The retired per-signup donation funnel and bounty escrow stay disabled; this
  did not revive them.

## Operational checks

- A funded address with `sweep_signature=''` and a non-empty `sweep_error` is
  the case to watch — the Worker is still holding a key for real money.
  ```sql
  SELECT purchase_id, deposit_address, sweep_error FROM world_element_purchases
  WHERE method='deposit' AND deposit_secret<>'' AND sweep_signature='';
  ```
- `treasury_not_configured` means the treasury variable was removed after the
  address was minted; restore it and cron will finish the sweep.
