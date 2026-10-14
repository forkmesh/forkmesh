# Paid microjobs

A microjob is a small, verified piece of work any account holder can claim for
a fixed, deliberately tiny SOL payment. Page: `/microjobs`; API:
`/api/microjobs`; policy module: `app/src/microjobs.py`.

Microjobs reuse the [community reward pool](community-reward-pool.md) custody
boundary: the server never holds a key, never signs, and never broadcasts. An
approved claim becomes a public, unsigned `chain_intents` plan that the
repository owner's desktop re-validates, signs in its encrypted vault, and
broadcasts.

## Jobs

| Job | Amount | Paid when |
| --- | --- | --- |
| `issue_report` | 0.005 SOL | An AI reviewer finds no existing issue or code already covering it |
| `merged_pull` | 0.05 SOL | The pull is merged **and** carries the MCP agent tool's signed marker |
| `referral_post` | 0.01 SOL | The public page still links the referral **and** its hostname has sent counted visitors |

Amounts are fixed in `microjobs.JOBS`. A microjob is a thank-you for verified
work, not employment, an investment, or a promise of future payment.

## Claim lifecycle

```
submitted -> pending_review | approved | rejected
             pending_review -> approved
             approved -> queued -> paid
```

`pending_review` waits for referral counters; `queued` is batched into an
unsigned plan; `paid` records the matching finalized transaction signature;
`rejected` returns the reason. A UNIQUE blind index
(`microjob_claims.subject_bi`) over the exact work item makes each issue, pull,
or page claimable once.

## Verification

- **Issue reports.** Existing issues are read over the public mirror gateway, a
  fixed-string `git grep` finds related code, and an AI model classifies the
  report as `new`, `duplicate`, `already_addressed`, or `unclear`. Only `new`
  files the issue and approves the claim. `unclear`, an unparseable answer, or a
  missing `AI` binding fails closed with a retryable response. The ordinary
  issue endpoint is unaffected.
- **Merged pulls.** `tools/forkmesh_mcp_server.py` writes `client:
  forkmesh-mcp-v1` and `clientSig`, an Ed25519 signature over
  `forkmesh-mcp-pull-v1\n<number>\n<base>\n<head>\n<author>\n<ts>`. The server
  checks the signature and that the signing node belongs to the claimant. Pull
  stores must carry both fields through rewrites.
- **Referral posts.** The URL is normalized to an ordinary public HTTPS page
  (no credentials, IP literals, non-443 ports, or internal names) and fetched
  once to find the claimant's `/r/<name>` link. `site_referrers` must show
  `MIN_REFERRAL_SITE_VISITS` visits from that hostname and `referral_stats`
  `MIN_REFERRAL_CLICKS` clicks. Until then the claim is `pending_review` and is
  rechecked hourly.

## Payout batching

The hourly `microjob-payouts` phase is `microjobs.queue_payouts`: it reads
approved claims oldest first, looks up each claimant's public payout address
(claims without one stay approved), merges claims per recipient, and writes one
`chain_intents` row of policy `microjob-payout-v1` with at most 8 transfers,
expiring in 4 hours. The owner's desktop picks it up from
`GET /api/rewards/signing-jobs`. A finalized matching transfer marks the claims
`paid`; a rejected or expired plan returns them to `approved`.

## Limits

At most 12 open claims, 8 claims per day, and 0.12 SOL per day per account,
and one claim per work item globally. These bounds raise the cost of farming;
they are not proof of personhood.

Payouts need the reward pool configuration (`COMMUNITY_REWARD_POOL_ADDRESS` or
`TREASURY_SOLANA_ADDRESS`, `COMMUNITY_REWARD_NETWORK`,
`REWARD_SIGNER_ACCOUNT`); otherwise `queue_payouts` returns
`pool_not_configured`. `microjob_claims` cannot be edited from the admin
console.
