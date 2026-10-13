# CFO: financial review and payouts

The CFO special agent (`.forkmesh/teammates/cfo.json`) has two jobs on every run:

1. **Financial review** (`.forkmesh/skills/cfo-financial-review.skill`). A
   read-only, forward-looking review of every financial part of the company,
   recorded below as a dated entry. It is advisory and never authorizes
   spending, payouts, price changes or account changes. The owner added it on
   2026-10-01, because a run that only did payouts produced nothing when
   payouts were not armed.
2. **Contributor payouts** (`.forkmesh/skills/cfo-payout-operations.skill`).
   These run only in the desktop's native `contributor-payouts` workflow, after
   the owner arms it in Control → Reward pool. An agent shell never signs or
   sends anything. See
   [desktop/agents-ui.md](../desktop/agents-ui.md#special-agents-formerly-brains-team-and-teammates).

## Review checklist

Each run covers every line, then states what changed since the previous entry.

| Area | What to check | Where |
| --- | --- | --- |
| Cash and runway | reward pool and treasury balances, monthly burn, months of runway | ledger header (`app/src/ledger.py` `ledger_handler`), on-chain balances |
| Revenue and collection | whether money can be collected, who receives it, paying members, price consistency | [app/payments-and-plans.md](../app/payments-and-plans.md), `app/src/patreon.py`, `app/public/membership-prompt.js`, `www/public/business-model.json` |
| Operating costs and renewals | fleet, autoscale and burst ceilings, AI subscriptions, Apple, domain, build minutes, mail, receipts the ledger marks missing | [OPERATIONS.md](../OPERATIONS.md) cost section, [fleet/AUTOSCALING.md](../fleet/AUTOSCALING.md), [RUNNERS.md](../RUNNERS.md) |
| Contributor rewards and payout controls | backlog against the daily cap, how rewards become payable, address changes, held receipts | `desktop/src/TipSigner.cpp`, `desktop/src/MainWindowRewardPool.cpp`, `app/src/ledger.py`, `app/src/discord_bugs.py` |
| Ledgers and reconciliation | recorded outflows against on-chain, unmatched transfers, audit trail | `app/src/ledger.py` chain backfill, `tools/public_interest_funding.py` |
| Tax and compliance | entity receiving revenue, recipient tax forms, filing deadlines | @CPA |
| Fundraising | pitch-deck claims against evidence | `app/public/pitch-deck/deck.html` |
| Promotions and commitments | contests, commissions, prizes, deadlines | `www/public/hackathon/rules.html` |

Look ahead 30, 90 and 365 days for each area. Mark any figure that could not be
verified as unknown and name who can supply it.

## Contest prize minimums (2026-10-01)

The September Revenue Contest ranked entrants by eligible first-month revenue
with no floor. If nobody else sold anything, a single $5 sign-up would have
won the $100 first prize. The terms now set prize minimums
(`www/public/hackathon/rules.html` sections 5 and 7, summarised on
`www/public/hackathon.html`):

- **Revenue minimum per place:** $200 for 1st, $100 for 2nd, $50 for 3rd. That
  is twice each prize, so prize plus commission never exceeds the revenue
  earned.
- **At least 3 different eligible customers** to win any prize.
- **Unqualified places:** an entrant below the minimum for their place drops to
  the highest lower place they qualify for. A place nobody qualifies for is not
  awarded.
- **Who pays:** the customer must pay with their own payment method. A payment
  made or funded by the entrant doesn't count.

Commissions are unchanged. Rule 10 allows changes if they are announced
publicly, and no prize had been awarded yet, so the minimums apply to this
contest. `app/tests/test_hackathon.py` checks that both pages state them.

## Expected monthly expenses (2026-10-07)

Receipts reach the ledger only when someone writes a migration, so a month
can pass with nothing recorded. On 2026-10-07 the ledger had Anthropic charges
for June, July and September 2026. August and October were missing, and
nothing showed that.

`GET /api/ledger` now projects an **expected expense** for those months
(`_expected_expenses` in `app/src/ledger.py`):

- **Which vendors:** any vendor in `operating_expenses` with charges in two
  consecutive calendar months within the last 6 months. One-off costs such as
  Apple, the domain and the business registration never match.
- **Which months:** every month from that vendor's first charge in the window
  through the current month that has no charge. A vendor with no charge for
  3 months counts as cancelled and is no longer projected.
- **Amount and date:** the total of the vendor's most recent earlier month,
  dated on that month's last charge day (noon UTC).
- **Status:** `missing_receipt` once the date has passed, `expected` before
  it.

The rows have category `expected-expense` and direction `forecast`. They are
left out of the Expenses, Debt and Income totals on the web page, and out of
the desktop Ledger page's totals. The payload's `expectedExpenses` list feeds
a reminder above the records on `/ledger`, and an **Expected** tab filters
them. A row disappears once a receipt for that vendor and month is recorded.

Each financial review run lists the receipts the reminder names and asks the
owner for them.

## Review log

### 2026-10-06: contest terms due in two days; AI capacity is the growing cost

Compared with the 2026-10-02 entry after merging 819 commits from `main`
(933 commits since that entry's `main`, `f15666eaf9`).

**What moved.** `app/src/ledger.py`, `app/src/patreon.py`,
`app/src/discord_bugs.py`, `app/public/membership-prompt.js`,
`www/public/business-model.json`, `desktop/src/TipSigner.cpp` and
`desktop/src/MainWindowRewardPool.cpp` are unchanged. The changes that matter:

- **Pitch deck.** "Live alpha" now reads "Live in production"
  (`app/public/pitch-deck/deck.html`). That describes the platform, not
  revenue, which still can't be collected. The raise materials should keep the
  two apart.
- **AI capacity.** A runner now holds up to 12 Claude Code and 12 Codex
  accounts (`docs/RUNNERS.md:678`). On 2026-10-06 r1 hit a weekly limit and
  switched to a second Claude account (`docs/RUNNERS.md:692`). AI
  subscriptions were already the largest unrecorded cost, and they now grow
  with the number of accounts.
- **Cloudflare.** The last KV namespace was deleted on 2026-10-04, and the
  account has no KV left (`docs/OPERATIONS.md`). Cloudflare costs should be at
  most the free plan, which is unverified until the invoice is read.
- **Fleet risk.** A 15-minute outage on the operator box nearly removed n1
  from DNS on 2026-10-04 (`docs/OPERATIONS.md`). No cost, but the core fleet
  depends on one box.
- **Marketing plan.** Its Revenue row still says "No measurement; no CFO run
  yet" (`docs/marketing/plan.md:37`). Revenue is measured here, and it is $0
  because nothing can be collected.

**Cash and runway.** Still unknown. No balance, burn or runway figure has been
recorded.

**Revenue.** Unchanged: production still has no `PATREON_*` credentials
(`docs/CUTOVER.md:98`), and checkout still goes to the personal page
`patreon.com/checkout/mdkaifansari04` (`app/public/membership-prompt.js:18-22`).

**Payout controls.** The limits are unchanged (`desktop/src/TipSigner.cpp:28-33`),
and the three gaps from the baseline are still open. **Payouts processed this
run:** none. `cfo.json` still shows `lastRunAtMs=0`, and an agent shell can't
arm, sign or send a payout.

**Contest.** The commission and prize terms are still missing
(`www/public/hackathon/rules.html`): no payment date, method or currency, and
nobody named to confirm the leaderboard. The commission table still lists a
"$25 plan" (line 275) that no Patreon tier matches ($5 / $10 / $50 / $100).

**Looking ahead.**

| When | What |
| --- | --- |
| 8 Oct 2026 (2 days) | Action 1 is due: the contest's payment terms and the $25 plan line. |
| 15 Oct 2026 (9 days) | The contest ends. Actions 2 and 4 are due. |
| 14 Nov 2026 | The earliest date a contest commission or prize could become final and owed. |
| 90 days | Each extra runner account adds a subscription. Without a count, burn and runway can't be stated to investors. |
| 365 days | Tax filings and recipient forms (@CPA). Audit entries begin to expire on 2027-10-01. |

**Actions.** The five baseline actions stay open in the same order, with two
changes:

- **Action 1** also covers replacing the "$25 plan" line with a real tier.
- **Action 4** also covers counting the Claude and Codex accounts signed in on
  the runners and their monthly price, and reading the Cloudflare invoice.

### 2026-10-02: no financial movement; contest payout terms missing

Compared with the 2026-10-01 baseline after merging 245 commits from `main`
(merge base `d58b9de506`).

**What moved.** None of the files behind the baseline figures changed:
`app/src/ledger.py`, `app/src/patreon.py`, `app/src/discord_bugs.py`,
`app/public/membership-prompt.js`, `www/public/business-model.json`,
`desktop/src/TipSigner.cpp`, `docs/OPERATIONS.md` and the pitch deck are
byte-identical to the baseline. `main` only renamed "CFO brain" to "CFO
special agent" in the Reward pool text (`desktop/src/MainWindowRewardPool.cpp`)
and added `www/public/llms.txt`. That file says "only bounded hosted services
are paid" (line 9), which adds one more pricing claim to keep consistent
(action 5).

**Cash and runway.** Still unknown. No balance, burn or runway figure has been
recorded since the baseline.

**Revenue.** Still no working way to collect money:

- Production still has no `PATREON_*` credentials (`docs/CUTOVER.md:98`).
- Checkout still goes to the personal page `patreon.com/checkout/mdkaifansari04`
  (`app/public/membership-prompt.js:18-21`).

**Payout controls.**

- The limits are unchanged (`desktop/src/TipSigner.cpp:28-36`): 0.025 SOL per
  payout, 0.05 SOL per day, 60 s spacing.
- The three gaps from the baseline are still open: no owner approval for
  Discord-estimated rewards, a daily cap kept only on the desktop, and no
  waiting period after a payout address changes.
- **Payouts processed this run:** none. The CFO runtime record still shows
  `lastRunAtMs=0`, so the native workflow has not been armed or run. An agent
  shell can't sign or send a payout.

**New finding: the contest never says how or when it pays out.**
`www/public/hackathon/rules.html` holds commissions for 30 days (section 7, "in
good standing for 30 days"). But it never says:

- when commissions and prizes are paid;
- in what currency or through which payment method;
- who confirms the leaderboard.

A referral paid on the last day (15 Oct) only becomes final on **14 Nov 2026**.
That's the earliest date a payout could be owed. Without production Patreon
credentials no referral can be attributed, so the likely amount owed is $0. The
risk is entrants expecting a payment the company can't calculate.

**Looking ahead.**

| When | What |
| --- | --- |
| 8 Oct 2026 (6 days) | Action 1 is due: the contest's commission and prize terms. |
| 15 Oct 2026 (13 days) | The contest ends. Actions 2 and 4 are due. |
| 14 Nov 2026 | The earliest date a contest commission or prize could become final and owed. |
| 90 days | Burn is still unknown, so a runway figure can't go into the raise materials. |
| 365 days | Tax filings and recipient forms (@CPA). Audit entries begin to expire on 2027-10-01. |

**Actions.** The baseline's five actions stay open in the same order. Action 1
now also covers the missing terms: publish the payment date, payment method and
currency for commissions and prizes, and say who confirms the leaderboard. Do
this before 8 Oct.

### 2026-10-01: baseline

Verified from the repository (file references are at this date).

**Cash and runway.** Unknown. No balance, burn or runway figures are recorded
anywhere. Pool and treasury balances live on-chain, and the pool and treasury
may be one wallet when only one is configured. The pitch deck plans for a
$1.0M raise at about $55.6K/month over 18 months.

**Revenue.** No working way to collect money.

- Patreon replaced Polar, which was removed on 2026-09-22. But production has no
  `PATREON_*` credentials (`docs/CUTOVER.md:98`), so a payment never grants a
  plan. All four `patreon_*` tables were empty on 2026-09-10.
- The checkout buttons point to a personal creator page,
  `patreon.com/checkout/mdkaifansari04` (`app/public/membership-prompt.js:16-23`).
  A separate campaign, `patreon.com/16434219/join`, is also linked.
- Prices disagree across sources:
  - Patreon tiers: $5 / $10 / $50 / $100 (`app/src/patreon.py:13-15`).
  - `business-model.json` lists only $5 and $10.
  - The homepage shows yearly prices of $50 and $100.
  - The contest names a $25 plan.
  - The deck sells enterprise contracts that don't exist.
- Ads earn nothing (`paymentProcessor: null`), and all 30 marketplace packages
  are free.

**Costs.** Documented infrastructure is about $20/month: four `vc2-1c-1gb`
nodes (`docs/OPERATIONS.md:598-603`). Documented ceilings would add up to $25
of autoscale capacity and $30 of burst runners if enabled. The likely largest
costs are not written down:

- Claude and Codex subscriptions
- Apple Developer Program fee
- domain renewal
- Sentry
- the office Mac

Codemagic's free macOS minutes are used up. The Vultr account and its
full-account API key are shared with unrelated businesses' machines, so its
invoice mixes costs.

**Payout controls.**

- **Limits** (`desktop/src/TipSigner.cpp:30-35`):
  - 0.001 to 0.025 SOL per payout (issue rewards at most 0.005 SOL).
  - 0.05 SOL per local day.
  - At least 60 s between payouts.
- **Gaps:**
  - The daily cap is enforced only on the desktop and is kept in local
    settings. It resets on a local date change.
  - "Ready" means only that the recipient published an address
    (`app/src/ledger.py:583-585`). Payouts go to the profile's current address,
    with no waiting period after it changes.
  - Discord bug reports that the AI triage marks "new" or "unclear" create
    payable rewards with no owner approval step
    (`app/src/discord_bugs.py:938-950`).
  - The `paid` update writes no audit entry, and audit entries are deleted
    after 365 days.
  - The wallet has a single signer.
- **Payouts processed this run:** none. The session was not armed, and
  `lastRunAtMs` was 0.

**Ledgers.** There is no reconciliation report. Chain backfill reads only 3
transactions per run and doesn't alert on unmatched transfers. Plain tips are
recorded as the sender reports them. Operating expenses are entered only
through SQL migrations. There is no CSV or tax export.

**Looking ahead.**

| When | What |
| --- | --- |
| 15 Oct 2026 | The referral contest ends. It promises 50% of each referred customer's first month plus $100 / $50 / $25 cash prizes, paid "through Patreon", which can't track a referral. |
| 30 days | Every new paying user is lost or goes uncredited until collection works under a company account. |
| 90 days | Burn stays unknown until AI, Apple and domain costs are recorded, so runway can't be stated to investors. |
| 365 days | Tax filings and recipient tax forms for any revenue and contributor payouts (@CPA). Audit entries start expiring a year after they are written. |

**Actions** (ranked)

1. **Decide how the contest's promised commissions and prizes will be honoured
   or amended, and publish it before it ends.** Owner and @Marketing, by
   2026-10-08. Partly done: prize minimums were added the same day (see
   above). Referral tracking through Patreon and the nonexistent $25 plan in
   the commission table are still open.
2. **Move collection to an account the company owns, connect Patreon in
   production, and confirm the entity and tax treatment.** Owner and @CPA, by
   2026-10-15.
3. **Before arming payouts again:**
   - add an owner approval step for Discord-estimated rewards;
   - enforce the daily cap on the relay;
   - add a waiting period after a payout address changes.

   Owner and @Sentinel.
4. **Record the monthly burn** (AI subscriptions, Apple, domain, Sentry,
   Vultr invoice) **and give ForkMesh its own Vultr account and key.** @COO, by
   2026-10-15.
5. **Make prices consistent** across the Patreon tiers, `business-model.json`,
   the homepage, the contest and the deck. @CEO and @Sales, by 2026-10-31.
