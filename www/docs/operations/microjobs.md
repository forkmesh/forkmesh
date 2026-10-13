# Paid microjobs

A microjob is a small piece of work anybody with a ForkMesh account can pick
up and be paid a fixed, deliberately tiny amount of SOL for. The section lives
at `/microjobs`; the API is `/api/microjobs`, and the policy module is
`app/src/microjobs.py`.

Microjobs reuse the community reward pool's custody boundary exactly. The
Worker never holds a key, never signs, and never broadcasts. An approved claim
becomes a public, unsigned `chain_intents` plan; the repository owner's own
desktop client re-validates that plan locally, signs it inside its encrypted
vault, and broadcasts it. See
[community-reward-pool.md](community-reward-pool.md) for the custody rules the
payout path inherits.

## The jobs and what they pay

| Job | Amount | Paid when |
| --- | --- | --- |
| `issue_report` | 0.005 SOL | A Workers AI reviewer finds no existing issue covering it and no code already addressing it |
| `merged_pull` | 0.05 SOL | The pull request is merged **and** its record carries the MCP agent tool's signed marker |
| `referral_post` | 0.01 SOL | The public page still contains the referral link **and** the hostname has already sent counted visitors |

Amounts are fixed in `microjobs.JOBS`, not operator-configurable per claim, so
the number a contributor reads on the page is exactly the number that reaches
the payout plan. They are small on purpose: a microjob is a thank-you for a
small piece of verified work, not employment, an investment, or a promise of
future payment. The page states that explicitly.

## Claim lifecycle

```
submitted -> pending_review | approved | rejected
             pending_review -> (re-verified later) -> approved
             approved -> queued -> paid
```

* `pending_review` — a posted referral's counters have not caught up yet. The
  hourly payout phase rechecks a bounded set; nothing is paid meanwhile.
* `approved` — verified, waiting for the next payout batch.
* `queued` — batched into a public unsigned plan the owner's desktop may sign.
* `paid` — a finalized transfer matched the exact public plan. The claim keeps
  the public transaction signature so the contributor can check it on-chain.
* `rejected` — verification said no, and the reason is returned to the
  claimant.

`microjob_claims.subject_bi` is a blind index over the exact piece of work
(repo + issue title, repo + pull number, or the posted URL). Its UNIQUE index
is the de-duplication: one issue, one merged pull, or one posted page can be
claimed once, by whoever got there first.

## Duplicate review for reported issues

`POST /api/microjobs/claims` with `job=issue_report` runs before anything
reaches the repository:

1. The repository's existing issue numbers are listed over the public mirror
   gateway, and their records are read in one batched call.
2. The most distinctive words of the report drive a fixed-string repository
   `git grep`, so the reviewer can see code that already addresses it.
3. Workers AI (the `AI` binding, same models as ForkBot) is asked one question
   in JSON mode: is this `new`, a `duplicate`, `already_addressed`, or
   `unclear`?

Only a `new` verdict files the issue and approves the claim. `duplicate` and
`already_addressed` file nothing, and the claim is rejected with the reviewer's
own one-sentence reason.

`unclear` — including an unparseable answer, a missing `AI` binding, and "no
online host could serve the existing issues" — fails closed with a retryable
response. It files no issue and records no claim, so only a report affirmatively
classified as `new` can reach the repository.

The gate is scoped to the microjobs path. The ordinary
`POST /api/repo/<owner>/<repo>/issues` endpoint is untouched: filing an issue
normally still costs nothing, waits for nothing, and is never turned away by a
model.

## Why pull requests must come from the MCP agent tool

`tools/forkmesh_mcp_server.py` stamps two extra fields into the `pull.md` it
writes:

```
client: forkmesh-mcp-v1
clientSig: <base64url Ed25519 signature>
```

`clientSig` covers `forkmesh-mcp-pull-v1\n<number>\n<base>\n<head>\n<author>\n<ts>`
— see `mcp_pull_canonical`, which is reproduced byte-for-byte in
`app/src/microjobs.py`. The plain `client` field proves nothing on its own
(anybody can type it); the signature binds the marker to this exact pull, and
the Worker additionally checks that the signing node belongs to the account
claiming the payout.

`desktop/src/PullStore.cpp` carries both fields through every rewrite. It never
authors them and never interprets them — but a merge rewrites `pull.md`, and
dropping the marker there would silently make an MCP-opened pull unpayable long
after the work was done.

A pull request opened by hand, from the website composer, or from the desktop
UI is a perfectly good pull request. It is simply not a paid microjob, and both
the pull composer and the microjobs page say so with the exact command to run.

## Referral-post validation

Both halves are required before a referral post is payable:

1. **The link is really posted.** The Worker fetches the submitted page once,
   bounded, and looks for the claimant's `/r/<name>` link in any spelling. The
   URL is first reduced by `normalize_public_url` to an ordinary public HTTPS
   page — no credentials, no bare IP literal, no non-443 port, no
   internal/loopback name — so this can never become a probe for anything on
   the inside.
2. **It actually sent people.** `site_referrers` must show at least
   `MIN_REFERRAL_SITE_VISITS` counted visits for that hostname, and
   `referral_stats` must show at least `MIN_REFERRAL_CLICKS` clicks on the
   claimant's own link.

A post that passes (1) but not yet (2) is held at `pending_review` with the
current numbers in its detail line, so the claimant can see exactly what is
missing rather than being rejected for being early. Each hourly payout phase
re-fetches up to four oldest pending pages and promotes a claim only when the
link is still present and both counters now pass.

## Payout batching

The `microjob-payouts` cron phase (minute 7 of each hour) calls
`microjobs.queue_payouts`. It:

* reads approved claims oldest-first;
* looks up each claimant's own public payout address from their account
  record — ForkMesh never creates a wallet, so a claim without one just stays
  approved;
* collapses multiple approved claims by the same person into one transfer,
  because a legacy Solana message may not repeat a destination;
* writes one `chain_intents` row of kind `microjob_payout` with policy
  `microjob-payout-v1`, at most 8 transfers, expiring in 4 hours; and
* flips those claims to `queued`.

The owner's desktop picks the plan up from the existing
`GET /api/rewards/signing-jobs` queue. On a finalized transfer matching the
exact plan, `_complete_chain_intent` marks every batched claim `paid` and
records the public signature. On a rejected or expired plan,
`_mark_chain_intent_failed` returns the claims to `approved` so the work is not
stranded behind a plan nobody will sign.

## Anti-farming bounds

* at most 12 open claims per account;
* at most 8 claims per account per day;
* at most 0.12 SOL per account per day;
* one claim per exact piece of work, globally.

These raise the cost of grinding a job and cap the blast radius of a
verification bug. They are not proof of personhood, and the public policy
payload says so in those words.

## Operating notes

* The `AI` binding is required for `issue_report` to approve. Without it the
  microjobs endpoint fails closed: no issue and no claim are created.
* Payouts require the same `COMMUNITY_REWARD_POOL_ADDRESS` /
  `TREASURY_SOLANA_ADDRESS`, `COMMUNITY_REWARD_NETWORK` and
  `REWARD_SIGNER_ACCOUNT` configuration the reward pool already uses. Without
  them `queue_payouts` returns `pool_not_configured` and does nothing.
* `microjob_claims` is on the admin console's restricted list. Editing a claim
  over HTTP would be authoring a transfer, so claims move only through their
  own verified API and the owner-signed batcher.
