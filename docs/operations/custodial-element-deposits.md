# Custodial deposit addresses for world-element purchases

The World element store accepts two payment methods. The second is optional.

| | Direct | Deposit |
|---|---|---|
| Buyer pays | the published pool address | a temporary ForkMesh address |
| Who holds the key | the buyer | **ForkMesh**, until the sweep lands |
| Confirmation | buyer submits the transaction signature | automatic; the address is watched |
| Mirror half | unsigned intent, signed on the operator's device | paid by the sweep transaction |

## Custody model

For a deposit, the server generates a fresh Ed25519 keypair, stores the seed
encrypted in `world_element_purchases`, and shows the public key as the payment
address. Once funded, it signs one transaction sending half to the treasury and
half to eligible online mirror nodes, then clears `deposit_secret`. Until the
sweep lands (a failed sweep is retried by cron), anyone who controls the server,
its database, or `DATA_KEY` can move those funds. The community reward pool key
is not involved.

The store shows custody text before purchase, and `/api/world/store/catalog`
marks the deposit entry with `forkMeshHoldsDepositKey: true`. Never describe
this method as non-custodial.

## Configuration

Deposits are off unless `TREASURY_SOLANA_ADDRESS` (or
`COMMUNITY_REWARD_POOL_ADDRESS`) is set. `CUSTODIAL_DEPOSITS = "0"` keeps the
treasury but disables deposits; buyers then see only the direct method.

## Invariants

- The shared read helper `_solana_rpc` cannot send; only `_solana_rpc_write`
  broadcasts, and only `sendTransaction`.
- The sweep is idempotent on `sweep_signature` and clears the seed on success;
  concurrent pollers cannot double-send.
- Payees come from the signed eligibility snapshot (`_eligible_reward_snapshot`),
  capped at `MAX_SWEEP_PAYEES`.

## Monitoring

A funded address with an empty `sweep_signature` and a `sweep_error` means the
server still holds a key for real funds:

```sql
SELECT purchase_id, deposit_address, sweep_error FROM world_element_purchases
WHERE method='deposit' AND deposit_secret<>'' AND sweep_signature='';
```

`treasury_not_configured` means the treasury variable was removed after the
address was minted; restore it and cron finishes the sweep.
