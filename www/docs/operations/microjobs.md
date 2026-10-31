# Paid microjobs

A useful issue report or a focused pull request can receive a small,
discretionary SOL reward after a repository owner reviews it. There is no
separate claim form, no fixed amount, and no payout schedule.
ForkMesh never holds a key: the owner pays directly from the owner's ForkMesh
desktop wallet.
Page: [`/microjobs`](https://app.forkmesh.com/microjobs).

A reward is a thank-you for useful work, not employment, an investment, a
balance, or a promise of future payment.

## Contribute

Submit the work once, through the repository's normal form:

- **Issues:** the [New issue form](https://app.forkmesh.com/forkmesh/forkmesh/issues/new).
  Check for duplicates first, then include reproduction steps, what happened,
  what you expected, and the version you tested.
- **Pull requests:** the [New pull request form](https://app.forkmesh.com/forkmesh/forkmesh/pulls/new).
  Keep the change focused, explain what it fixes, and include verification
  evidence.

Nothing else needs to be filed. Duplicates and spam are closed without a
reward.

## Estimate, pending, paid

| Stage | What it means |
| --- | --- |
| No estimate | The owner has not attached a reward. Most work, including accepted work, may stay here. |
| Estimate | The owner recorded an amount on the issue or pull request. Issue estimates are typically 0.001–0.005 SOL. An estimate is not an approved or earned payment. |
| `pending_merge` | A pull request estimate. It stays conditional until the pull is merged; a pull that is closed unmerged is not paid. |
| `pending_wallet` | The work qualifies, but no payment has been sent yet. It stays blocked while the contributor has no payout address. |
| `paid` | The owner sent the transfer. The ledger row carries the finalized Solana transaction signature. |

Estimates and their status appear on the issue or pull request and in the
public [Ledger → Pending payments](https://app.forkmesh.com/ledger). Only a
`paid` row with a transaction signature is a completed payment.

## Getting paid

Save a public Solana address under
[Settings → Verification and payout](https://app.forkmesh.com/dashboard/settings).
Never give anyone a wallet key or recovery phrase. The owner's desktop unlocks
the owner's wallet locally and broadcasts the exact direct transfer; the server
never signs or broadcasts. Timing is up to the owner.

## Legacy claim API

The sections below describe an earlier fixed-amount claim system. The web app
no longer offers it: `/microjobs` and the issue and pull-request forms do not
submit claims. Its server code (`app/src/microjobs.py`, `/api/microjobs/claims`,
and the hourly `microjob-payouts` cron phase) is still present, so an
installation may still answer direct API calls, but nothing here applies to
work submitted through the normal forms above, and none of it is a promise of a
fixed amount or an hourly payout.

### Jobs

| Job | Amount | Paid when |
| --- | --- | --- |
| `issue_report` | 0.005 SOL | An AI reviewer finds no existing issue or code already covering it |
| `merged_pull` | 0.05 SOL | The pull is merged **and** carries the MCP agent tool's signed marker |
| `referral_post` | 0.01 SOL | The public page still links the referral **and** its hostname has sent counted visitors |

Amounts are fixed in `microjobs.JOBS`. Claims reuse the
[community reward pool](community-reward-pool.md) custody boundary: an approved
claim becomes a public, unsigned `chain_intents` plan that the repository
owner's desktop re-validates, signs in its encrypted vault, and broadcasts.

### Claim lifecycle

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

- A merged pull's key is its signed MCP marker (number, base, head, author,
  ts), not the repository it was read from. Forks copy the pull ledger branch,
  so a key that named the repository let one merged pull be paid again from
  every fork.
- A referral page's key (`referral_subject` in `app/src/microjobs.py`) names
  the post, not the spelling of its URL, so one page cannot be claimed again
  under a variant link, and two different posts stay two claims:
  - When the fetched page has a `<link rel="canonical">` on the same site,
    that URL is the key. Every key, canonical or not, then goes through the
    steps below.
  - The host is lowercased and `www.`, `old.`, `m.` and `mobile.` are dropped.
    The path loses repeated and trailing slashes, and percent-encoding is
    normalised. The fragment is dropped.
  - On hosts that identify a post by query (`news.ycombinator.com`: `id`;
    `youtube.com/watch`: `v`; `/playlist`: `list`) only those keys are kept.
    On any other host the query is kept, sorted, without tracking keys
    (`utm_*`, `ref`, `fbclid`, `gclid`, `si`, `s`, `t` and similar). Dropping
    the whole query would merge every Hacker News item and every YouTube
    video into one claim.
  - `youtu.be/<id>`, `/shorts/<id>`, `/embed/<id>` and `/live/<id>` become
    `youtube.com/watch?v=<id>`.
  - Reddit (`*.reddit.com`, `redd.it`) is case-insensitive, so its path is
    lowercased. A post becomes `reddit.com/comments/<post>[/<comment>]`,
    without the subreddit or the slug Reddit ignores.
  - On other hosts without a same-site canonical link, extra query keys that
    aren't on the tracking list still make a separate key. A second key then
    stops one account reusing its own page:
    `microjob:referral_post:account:<account_bi>:<scheme://host/path>`, with
    no query. That allows one claim per account per page path. Another
    account can still claim the same path. Hosts on the allowlist, Reddit,
    and pages whose canonical link sets the key take no second key.
  - The second key is stored as a companion row in `microjob_claims`: same
    account, `status='rejected'`, `amount_lamports=0`, and `data` naming the
    claim it belongs to. One batch writes both rows. The claim insert refuses
    when any of its keys, the companion key included, already exists. The
    companion insert runs only if the claim row was written. The table's
    UNIQUE `subject_bi` index settles any race.
  - A companion row is never paid, counted against a cap, or batched into a
    payout. The claimant's list and the public ledger only show rows with
    `amount_lamports>0`.
- Keys written before these rules still count as claimed. That means the
  repository plus pull number, the full normalized URL, and the URL's own key
  when the page's canonical link gave the claim a different one.
- The claim insert is one conditional `INSERT ... SELECT` that re-checks the
  subject and the open, daily-count and daily-amount caps. Requests running at
  the same time on different cores cannot pass the caps together.

### Verification

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

### Payout batching

The hourly `microjob-payouts` phase is `microjobs.queue_payouts`: it reads
approved claims oldest first, looks up each claimant's public payout address
(claims without one stay approved), merges claims per recipient, and writes one
`chain_intents` row of policy `microjob-payout-v1` with at most 8 transfers,
expiring in 4 hours. The owner's desktop picks it up from
`GET /api/rewards/signing-jobs`. A finalized matching transfer marks the claims
`paid`; a rejected or expired plan returns them to `approved`. The phase only
moves claims made through the API; it never touches issue or pull request
estimates.

### Limits

At most 12 open claims, 8 claims per day, and 0.12 SOL per day per account,
and one claim per work item globally. These bounds raise the cost of farming;
they are not proof of personhood.

Payouts need the reward pool configuration (`COMMUNITY_REWARD_POOL_ADDRESS` or
`TREASURY_SOLANA_ADDRESS`, `COMMUNITY_REWARD_NETWORK`,
`REWARD_SIGNER_ACCOUNT`); otherwise `queue_payouts` returns
`pool_not_configured`. `microjob_claims` cannot be edited from the admin
console.
