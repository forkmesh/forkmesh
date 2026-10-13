# Incentive independence

Status: P0 product invariant (issue #516). Machine-readable conformance
requirements: [`conformance/incentives/v1/requirements.json`](../../conformance/incentives/v1/requirements.json).

> Repository access, identity, contribution, governance, and essential network
> participation work fully without a token, blockchain, reward system, or
> speculative asset.

ForkMesh offers optional incentives: Solana tips and bounties, the community
reward pool, microjob payouts, hackathon prizes, and paid memberships. Each one
is an **adapter**. An adapter may read canonical state to decide whom to pay or
thank. Canonical state must never read an adapter.

## Architecture

Canonical state is:

- Git objects and refs, and the signed refs pins that route reads to mirrors.
- Accounts, device keys, SSH keys, sessions, and portable identities.
- Signed `.forkmesh` collaboration records: issues, pull requests, reviews,
  comments, moderation decisions, and their repository policy documents.
- Organizations, teams, repository roles, and organization succession.
- Discovery: the catalog, public snapshots, mirror health, and routing.
- Governance: marketplace publication review and community ad votes.

Incentive and payment adapters are:

| Adapter | Code | Tables |
| --- | --- | --- |
| Solana read/write plumbing | `app/src/solana.py` | `wallet_balances`, `chain_intents` |
| Community reward pool | `app/src/reward_policy.py` | `reward_*`, `pending_rewards`, `central_fund`, `funds_received` |
| Public ledger | `app/src/ledger.py` | `ledger_backfill_*` |
| Tips and bounties | `entry.py` tip/bounty handlers | `issue_bounty`, `bounty_wallet`, `tip_notice`, `contributor_reward_estimates`, `reward_wallet_reminders` |
| Microjobs | `app/src/microjobs.py`, `microjobs_runtime.py` | `microjob_claims` |
| Hackathon prizes | `app/src/hackathon.py`, `hackathon_runtime.py` | `hackathon_*` |
| World purchases | `entry.py` element purchases | `world_element_purchases` |
| Legacy custody migration | `entry.py` custody audit | `legacy_custody_*` |
| Conventional memberships | `app/src/patreon.py`, `patreon_handler.py` | `patreon_*` |

The isolation rules are:

1. **Imports flow one way.** Canonical modules (`collaboration.py`,
   `catalog.py`, `catalog_routes.py`, `git_http.py`, `hosted_git.py`,
   `mirrors.py`, `mirror_replica.py`, `edge_routing.py`, `ssh_keys.py`,
   `portable_sessions.py`, `security_controls.py`, `organization_succession.py`,
   `marketplace_governance.py`, `repository_imports.py`,
   `repository_migration.py`, `public_snapshots.py`) never import an adapter.
2. **Hooks are best effort.** When a canonical write notifies an adapter (for
   example, a signup, a credited referral, or a first mirror notifying the
   hackathon), the call is optional and wrapped so that an adapter exception
   cannot fail the canonical write.
3. **No foreign keys point into adapter tables.** Removing every adapter table
   leaves every canonical table, and every foreign-key relation between
   canonical tables, unchanged.
4. **No asset signal in decisions.** Identity, authorization, ranking,
   moderation, and governance code never reads a wallet, payout address,
   balance, token, stake, tip, bounty, or payment history. A payout address is
   optional display metadata stored inside the account record. It is never an
   eligibility, verification, or trust requirement.

Wallet and tip leaderboards are views of an adapter. They are labelled as
such, are not used for search, catalog, roster, or trust ordering, and can be
removed without affecting any other ranking.

Marketplace publication is the one decision that used to depend on assets:
reviewers needed a SOL address and were listed by wallet value. Since #516, any
five distinct active members may approve and reviewers are listed in approval
order. The legacy `wallet_address`, `value_lamports`, and `value_checked_at`
approval columns are retired. New approvals store blanks, and nothing reads the
columns.

## Commercial commitment

The [business-model contract](business-model.md) publishes two permanent
commitments in `/business-model.json`:

- `tokenOrBlockchainRequired: false`: no ForkMesh product, plan, or protocol
  right requires a token, a blockchain account, or a speculative asset.
- `incentivesAffectIdentityPermissionsRankingModerationOrGovernance: false`:
  no incentive, payment, or asset holding changes identity, permissions,
  ranking, moderation, or governance.

ForkMesh never sells or issues a token and never presents rewards as an
investment.

## Protocol governance

Signed collaboration objects derive authority only from the repository policy
document (maintainer, moderator, and notifier keys). The
[collaboration protocol](design/conflict-safe-collaboration.md) forbids
inferring any role from asset ownership or transaction history. A change to
this invariant is a breaking protocol change. It requires a new protocol
version and cannot be made through a hosted configuration switch.

## Payment and no-payment paths

Every optional incentive has a no-payment path. Where money is accepted, a
conventional path is preferred:

| Offer | No-payment path | Conventional payment path |
| --- | --- | --- |
| Supporter or Pro membership | Hosted Free and self-hosting keep every protocol right | Patreon |
| Community reward pool contribution | Not required for anything | Supporter membership |
| Tips to members | Stars, follows, and signed reactions | Membership funds public infrastructure |
| Bounties and microjob payouts | Contribute without claiming a payout. Review and merge are unchanged. | Not yet offered: payouts are SOL-only today and stay optional |
| Hackathon prizes | Entry, judging, and votes need no wallet | Prize months are membership grants, not assets |
| Public-interest grants and matching | Applying, review, and selection need no wallet, account, or plan | Bank transfer, Open Collective, or Liberapay. Solana only if the recipient chooses it. |

Grant awards follow the [public-interest funding](funding/README.md) program. Its ledger rejects any award that grants access, identity rights, votes, or a network obligation.

A gap in the conventional column is tracked as open work. It never becomes a
reason to require a wallet.

## Disabled deployment

An adapter is disabled when its configuration is absent. That means no reward
pool signer, no `NODE_SOLANA_ADDRESS`, no Patreon credentials, and a
hackathon left in `draft`. A clean deployment in that state must pass:

- account signup, sign-in, device and SSH key registration;
- repository create, push, public browse, and clone through a mirror;
- signed issue, pull request, review, comment, and moderation records;
- organization roles and succession;
- catalog discovery and mirror routing; and
- marketplace submission, member approval, and owner publication.

Unconfigured adapters answer explicitly: `patreon_oauth_not_configured` and
`patreon_webhook_not_configured` with HTTP 503, and the microjob payout queue with `pool_not_configured`. They never block
a canonical response.

## Failure, rollback, and recovery

- **An adapter provider fails or is removed** (RPC outage, provider shutdown,
  revoked credentials): payouts and badges pause; canonical state is untouched.
  Unset the adapter's configuration, then drop or keep its tables. No canonical
  migration is involved.
- **A replacement provider:** add a new adapter that reads canonical state. Do
  not copy asset data into canonical tables.
- **A coupling regression reaches review:** `app/tests/test_incentive_independence.py`
  fails and the `incentive-independence` quality gate blocks the release. Revert
  the coupling change; do not waive the gate.
- **Legacy asset data in canonical rows** (for example, pre-#516 marketplace
  approval values): it is inert, and the next approval upsert blanks it.

## Telemetry and measures

Report these per release:

- **Essential no-incentive journey pass rate:** the disabled-deployment
  journeys above, from the quality-gate run with adapter configuration unset.
- **Adapter coupling violations:** the number of failing
  `test_incentive_independence.py` assertions. The target is zero.
- **User comprehension:** ask each person in a five-person cohort whether they
  need a wallet or token to use, contribute to, or govern a repository. A person
  passes by answering "no" without prompting. The cohort passes at 4/5.

Product analytics stay default-off and allowlisted. No wallet, address,
balance, or transaction identifier is recorded. Do not mark the comprehension
gate complete with blank, synthetic, or inferred results.

## Linked automated evidence

- Adapter import isolation, best-effort hooks, schema removal, asset-free
  decisions, unconfigured adapters, and published commitments:
  `app/tests/test_incentive_independence.py`
- Wallet-free marketplace governance: `app/tests/test_marketplace_governance.py`
- Non-custodial user-facing boundary: `app/tests/test_non_custodial_surfaces.py`
- Commercial contract: `app/tests/test_business_model_contract.py`
- Collaboration authorization from policy keys only:
  `app/tests/test_collaboration_protocol.py`

Automated checks support the release gate. They do not replace a recorded
disabled-deployment run, comprehension results, security review, or production
telemetry evidence.
