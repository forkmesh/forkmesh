# Incentive independence

Repository access, identity, contribution, governance, and essential network participation work fully without a token, blockchain, reward system, or speculative asset.

Optional incentives (tips, bounties, reward pool, microjobs, hackathon prizes,
memberships) are **adapters**. An adapter may read canonical state; canonical
state never reads an adapter. Requirements:
`conformance/incentives/v1/requirements.json`.

## Architecture

Canonical state is Git data and signed refs pins, accounts and keys, signed
`.forkmesh` collaboration records, organizations and roles, discovery, and
governance. Adapters include `solana.py`, `reward_policy.py`, `ledger.py`,
`microjobs.py`, `hackathon.py` and `patreon.py` with their own tables.

1. Canonical modules never import an adapter.
2. Hooks from canonical writes to adapters are best effort and cannot fail the write.
3. No foreign key points into an adapter table.
4. Identity, authorization, ranking, moderation and governance never read
   wallets, balances, tokens, tips or payment history.

## Commercial commitment

`/business-model.json` publishes `tokenOrBlockchainRequired: false` and
`incentivesAffectIdentityPermissionsRankingModerationOrGovernance: false`.

## Protocol governance

Collaboration authority comes only from the repository policy document's keys.
Changing that requires a new protocol version.

## Payment and no-payment paths

Every incentive has a no-payment path; conventional payment is preferred over
crypto. Grants follow [public-interest funding](funding/README.md).

## Disabled deployment

With no adapter configured, signup, keys, repositories, clone, collaboration
records, organizations, discovery and marketplace approval all work.
Unconfigured adapters answer explicitly (for example HTTP 503
`patreon_oauth_not_configured`) and never block a canonical response.

## Failure, rollback, and recovery

A failed or removed adapter pauses payouts only: unset its configuration. A
coupling regression fails the `incentive-independence` quality gate; revert it.

## Telemetry and measures

- Essential no-incentive journey pass rate
- Adapter coupling violations (target zero)
- User comprehension: a cohort must answer that no wallet or token is needed

## Linked automated evidence

- `app/tests/test_incentive_independence.py`
- `app/tests/test_marketplace_governance.py`
- `app/tests/test_non_custodial_surfaces.py`
