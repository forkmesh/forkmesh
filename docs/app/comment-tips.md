# Tips on comments (Solana Pay)

A comment, issue or pull request description, review or discussion post whose
author has a Solana address on their profile (the existing public `solana`
payout field in Settings → Verification and payout, `SOLANA_RE`) shows a
**Tip** button to signed-in readers other than the author. Tips go directly
wallet to wallet: **the forge never holds keys or funds and never signs or
submits a transaction.** `comment_tips.py` only builds payment requests and
reads the chain through the read-only `_solana_rpc` (never `_solana_rpc_write`
or the platform payout code).

## Flow

1. The Tip sheet (a dialog, a bottom sheet on phones) offers SOL, USDC (if
   on) and the instance token (if configured), presets and an amount.
2. `POST /api/repo/<owner>/<repo>/tips` `{kind, number, comment, recipient,
   token, amount}` checks the session, read access to the repository, the
   recipient's address (not yourself), the token and the amount (more than
   zero, at most the token's precision, at most 1000 SOL), then stores a
   pending request with a fresh random 32-byte `reference` public key and
   answers with a Solana Pay link from `_solana_pay_uri`:
   `solana:<recipient>?amount=0.05&reference=<ref>&label=Tip%20for%20%40bob&message=…`
   (`&spl-token=<mint>` for a token).
3. The sheet shows the link as a QR code (`ForkMeshQR`, as on the profile's
   tips card), an **Open in wallet** link and **Copy payment link**. The
   tipper's wallet signs and sends.
4. While the sheet is open it asks `POST /api/tips/verify {reference}` every
   6 seconds for 3 minutes (and on **I've sent it - check now**). The
   `tip-verify` cron phase (every 5 minutes) looks again for up to three
   pending tips from the last two hours, least recently checked first.
5. Verification calls `getSignaturesForAddress(reference)` and
   `getTransaction(…, jsonParsed)` for at most two signatures. A transaction
   matches when it succeeded, names the reference and raised the recipient's
   balance of the asked token by at least the asked amount (SOL balances, or
   `postTokenBalances` for the mint and owner). A signature confirms one tip
   only. Then the tip is marked confirmed, the comment shows "💸 tipped" (with
   a count above one), the author gets a `tip_sent` ping ("dave tipped your
   comment on alice/app#7"), and a SOL tip is also written to `tip_notice`
   (the receipt table desktop tips and the tip leaderboard use).

## Limits and storage

`comment_tips` keeps one row per request: the repository and comment, blind
indexes of tipper and recipient, token, mint, amount in base units, status,
signature, timestamps, check count and an encrypted `{tipper, recipient,
address, owner, repo}`. Pending requests expire after a day; an account has at
most 10 pending (the oldest makes way), 50 a day and 20 a minute; verification
is 12 a minute; a record holds at most 5000. Repository and account deletion
remove the rows. RPC calls go through `_solana_rpc`, so they fail over across
the keyless public endpoints plus the operator's `SOLANA_RPC_URL` and are
charged to the cron phase's subrequest budget.

## Settings

Administrators set, in Settings → Reactions, GIFs & tips → Comment tips
(`GET/POST /api/tips/settings`, stored in `instance_settings` under `tips`):
tips on or off, USDC on or off, and an optional instance token `{mint,
symbol, decimals}`. `GET /api/tips/config` gives the tokens to the dashboard;
`GET /api/tips/recipients?names=` gives the public addresses of up to 30
authors.

Code: `app/src/comment_tips.py`, `app/public/dashboard/js/09-comment-tips.js`,
`_solana_pay_uri` / `_amount_units` in `app/src/entry.py`.
