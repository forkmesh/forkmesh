# CFO: financial review and payouts

The CFO special agent (`.forkmesh/workflows/cfo.json`) has two jobs on every
run:

1. **Financial review** (`.forkmesh/skills/cfo-financial-review.skill`): a
   read-only, forward-looking review of every financial part of the company,
   recorded below as a dated entry. It is advisory and never authorizes
   spending, payouts, price changes or account changes.
2. **Contributor payouts** (`.forkmesh/skills/cfo-payout-operations.skill`):
   run only in the desktop's native `contributor-payouts` workflow after the
   owner arms it in Control, Reward pool. An agent shell never signs or sends
   anything.

## Review checklist

Each run covers every line, then states what changed since the previous entry.

| Area | What to check | Where |
| --- | --- | --- |
| Cash and runway | reward pool and treasury balances, monthly burn, months of runway | ledger header (`app/src/ledger.py`), on-chain balances |
| Revenue and collection | whether money can be collected, who receives it, paying members, price consistency across pages | payment and plan code, membership prompt, `www/public/business-model.json` |
| Operating costs and renewals | fleet, autoscale and burst ceilings, AI subscriptions, developer programs, domain, build minutes, mail, receipts the ledger marks missing | [OPERATIONS.md](../OPERATIONS.md), [fleet/AUTOSCALING.md](../fleet/AUTOSCALING.md), `/ledger` |
| Contributor rewards and payout controls | backlog against the daily cap, how rewards become payable, address changes, held receipts | `desktop/src/TipSigner.cpp`, `desktop/src/MainWindowRewardPool.cpp`, `app/src/ledger.py` |
| Ledgers and reconciliation | recorded outflows against on-chain, unmatched transfers, audit trail | `app/src/ledger.py` chain backfill, `tools/public_interest_funding.py` |
| Tax and compliance | entity receiving revenue, recipient tax forms, filing deadlines | the accountant |
| Fundraising | pitch-deck claims against evidence | `app/public/pitch-deck/deck.html` |
| Promotions and commitments | contests, commissions, prizes, payment terms, deadlines | `www/public/hackathon/rules.html` |

Look ahead 30, 90 and 365 days for each area. Mark any figure that could not
be verified as unknown and name who can supply it. Give at most five ranked
actions with owners and due dates.

## Expected monthly expenses

Receipts reach the ledger only when recorded, so `GET /api/ledger` projects an
**expected expense** for missing months (`_expected_expenses` in
`app/src/ledger.py`):

- vendors with charges in two consecutive months within the last 6 months
  (one-off costs never match);
- every month from the vendor's first charge in that window through the
  current month with no charge; a vendor with no charge for 3 months counts as
  cancelled;
- amount and date from the vendor's most recent earlier month;
- status `missing_receipt` once the date has passed, `expected` before.

Rows have category `expected-expense` and direction `forecast`, are excluded
from totals, and feed a reminder and an **Expected** tab on `/ledger`. A row
disappears once a matching receipt is recorded. Each review lists the receipts
the reminder names.

## Recording expenses

A cost a member paid personally is recorded with the ForkMesh MCP tool
`record_expense`, not a migration. It posts to `POST /api/ledger/expenses`
(`operating_expense_record_handler` in `app/src/ledger.py`), signed with the
desktop's identity key under `forkmesh-ledger-expense-request-v1` over the
exact body:

- the signer must be an administrator with two-factor sign-in (`_is_admin`);
  browser sessions are not accepted, so the route has no cookie surface;
- `paidBy` must be an active account; `amount` has at most two decimals;
  `incurredOn` is a UTC date, not in the future; an original card currency
  needs both `originalAmount` and `originalCurrency`;
- the row is `operating_expenses`, reimbursement `outstanding`, so `/ledger`
  shows the expense and the debt owed to the payer;
- the invoice number is the key: sending the same invoice again changes
  nothing, and the same number with another amount, payer, vendor or date is
  refused with `expense_reference_conflict`;
- each recording, and each refused non-administrator, is in the audit log as
  `ledger.expense.record`.

A credit note is not a charge: record the invoice it credits only if the
member was actually charged.

## Contest prize minimums

Contest prizes require a minimum of eligible revenue per place (twice the
prize) and at least three distinct paying customers; an entrant below the
minimum drops to the highest place they qualify for, and unqualified places
are not awarded. Payments funded by the entrant do not count. The rules pages
state this (`app/tests/test_hackathon.py`).

## Review log

New entries go at the top; keep earlier entries.

### 2026-10-06: baseline

**Cash and runway.** Unknown: no balance, burn or runway figure is recorded.

**Revenue.** No working collection path in production: payment-provider
credentials are not configured, so payments cannot grant plans. Prices differ
across the pricing sources and must be reconciled.

**Costs.** Documented infrastructure is about $5 per 1 GB node per month, plus
autoscale and burst ceilings if enabled. AI subscriptions are the largest
unrecorded and growing cost (runners can hold several provider accounts).
Developer program fees, the domain, error tracking and build hardware are not
yet recorded; the CDN account should be on the free plan, pending the invoice.

**Payout controls.** Per-payout, daily and spacing limits in
`desktop/src/TipSigner.cpp` are unchanged. Open gaps: no owner approval for
estimated rewards, the daily cap is enforced only on the desktop, and there is
no waiting period after a payout address changes. Payouts processed: none.

**Commitments.** Contest rules lack payment date, method and currency and who
confirms the leaderboard; one listed plan matches no real tier.

**Actions.**

1. Publish contest payment terms and fix the plan list.
2. Configure payment collection in production with a company-owned account.
3. Reconcile prices across all pricing pages and materials.
4. Record AI subscriptions, provider accounts and other recurring costs, and
   read the CDN invoice, so burn and runway can be stated.
5. Close the payout-control gaps before payouts are armed.
