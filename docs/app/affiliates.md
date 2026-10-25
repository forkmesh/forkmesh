# Affiliate program

Part of [the App docs](README.md). The September Revenue Contest
([www/docs/hackathon.md](../../www/docs/hackathon.md)), kept open all year
without its leaderboard. People with a verified ForkMesh account join as
affiliates, claim companies before any outreach, and bring them onto a paid
plan through the Patreon link on the homepage. They earn 50% of each eligible
customer's first month. Every claim also lands in the company CRM with the
affiliate named as its source.

| Area | Where |
| --- | --- |
| Rules, validation and routes | `app/src/affiliates.py` |
| Worker adapter | `app/src/programs_runtime.py` (`_AffiliatesRuntime`) |
| Lead hand-off to the CRM | `plugin_data.deliver_lead` → the CRM's `record_lead` hook |
| Page | `www/templates/pages/affiliates.html`, `www/public/assets/hackathon/affiliates.js` |
| Schema | `app/migrations/0262_affiliates_and_plugin_race.sql` (`affiliates`, `affiliate_leads`) |
| Campaign | [docs/marketing/campaigns/plugin-race-and-affiliates-2026-10-16.md](../marketing/campaigns/plugin-race-and-affiliates-2026-10-16.md) |

## Routes

| Route | Who | What |
| --- | --- | --- |
| `GET /api/affiliates` | anyone | Program terms; with a session, the caller's affiliate status and leads |
| `POST /api/affiliates/join` | verified account | `{channel, acceptTerms: true}` |
| `POST /api/affiliates/leads` | active affiliate | Claim: `{company, website, contactName?, contactTitle?, contactEmail?, notes?, consent}`; `consent: true` is required when an email is given |
| `POST /api/affiliates/leads/<id>/activity` | the claiming affiliate | `{note}`: log outreach; keeps the claim 7 more days |
| `GET /api/admin/affiliates` | admin | Every affiliate and lead, with contact emails and the pipeline status |
| `POST /api/admin/affiliates` | admin | `{lead, status, note?, firstMonthCents?}` or `{affiliate, status}` |

POSTs need an allowed origin. Admin means the `is_admin` flag plus TOTP
(`_is_admin`).

## Rules the server enforces

The terms come from the contest's official rules
(`/hackathon/archive/revenue-contest-2026-rules`) and are returned by
`program()`. The server enforces these:

- A company is identified by its website host (lowercased, `www.` removed).
  `forkmesh.com` and IP addresses are refused. The first open claim on a host
  wins, guarded by a partial unique index on `domain_bi`.
- A claim expires 7 days after it was made or after its last logged activity
  (`expires_at`). Logging outreach, or an admin moving the lead to
  `contacted` or `trial`, renews it. Expired claims are swept on the next
  claim or log and free the host again.
- An affiliate can have at most 10 open claims (`new`, `contacted`, `trial`).
- Contact details are optional, because a claim comes before outreach. If an
  email is given, the affiliate must tick that the person is happy for
  ForkMesh to have it.
- A claim whose email or domain matches the affiliate's own email is refused
  as a self-referral.
- A claim whose email is already a CRM contact is stored as `known`: it never
  earns commission, holds no claim and does not count toward the open limit.
  That is how "already in an active sales conversation" is decided
  automatically. Anything subtler, such as related accounts, the customer's
  own payment method or coordinated purchases, is for an admin to judge.
- `won` and `paid` need `firstMonthCents`; the commission is half of it,
  rounded down to the cent.

Statuses: `new`, `known`, `contacted`, `trial`, `won`, `paid`, `lost`,
`rejected`, `expired`. Affiliates are `active`, `paused` or `removed`.

## The CRM link

Core code must not name the CRM package (its lifecycle tests check this), so
the hand-off goes through a generic hook. `plugin_data.deliver_lead(runtime,
org, lead)` calls `record_lead` on every organization-data package that
exports it. The CRM's hook creates one contact in the org's CRM:

- flow `customers`, stage `lead`, next step "Check with @<name> before
  contacting them"
- named after the contact, or the company when no contact was given
- source `Affiliate: @<name>`
- tags `affiliate` and `affiliate-<name>`
- custom fields `affiliate` and `affiliate_lead` (the lead id)
- a note saying who claimed them and, with an email, that consent was given

It never edits an existing contact: a matching email returns `exists`. A
claim without an email always creates a new row; duplicates by company are
already prevented on the affiliate side by the website host. The
organization defaults to `forkmesh`; set `AFFILIATE_PIPELINE_ORG` to change
it. If the CRM is not installed or fails, the lead is still stored, with
`pipeline_status` set to the reason (`not_installed`, `crm_full`,
`delivery_failed`), so nothing is lost. The admin listing shows that status.

The affiliate does the selling. The CRM row lets sales see who owns a
company before contacting it. The lead's own status (and the commission) is
moved with the admin route once a Patreon payment is matched to it.

## Payment

Commission is paid by hand. Once a lead is `won` and the plan has stayed paid
for 30 days, email the affiliate at their account address, pay them, then set
the lead to `paid`.

### Tax forms (not yet in the public terms)

The public `/affiliates` page does not name a tax form yet. The owner decided
on 2026-10-25:

- **Companies can be paid**, not only individuals. The tax-form line from the
  Revenue Contest's unapproved decision D6 therefore needs W-8BEN-E added:
  "Payment requires a completed Form W-9 (US persons), W-8BEN (non-US
  individuals) or W-8BEN-E (non-US entities); payments may be subject to tax
  reporting or withholding."
- **No licensed tax professional has reviewed that wording.** It came from
  the CPA teammate, who asked for that review before it is approved.

Get that review before the line goes on `/affiliates` or the first commission
is paid. No referral has been completed yet, so nobody has been paid under the
current terms.

## Data

`affiliates.data` and `affiliate_leads.data` are sealed with the row key; the
contact's details exist only there and in the CRM. Both tables are in
`tools/next_rekey.py` (blind indexes `account_bi`, `affiliate_bi`,
`domain_bi`).
