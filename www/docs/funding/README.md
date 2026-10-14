# Public-interest funding

The program funds public repositories, volunteer mirrors, accessibility,
documentation, education and independent tooling through donations,
sponsorship, grants and matching. Funding never buys access, votes or network
rights. Program and rounds live in
[`conformance/funding/v1/`](../../../conformance/funding/v1/program.json); the
[funding register](funding-register.md) and `/docs/funding/register.json` are
generated from them. See also [incentive independence](../incentive-independence.md).

## Eligibility

Public, openly licensed work in a program area. No ForkMesh account, plan,
wallet or token is needed. The forkmesh.com operator, its staff, and a round's
funders and reviewers are excluded.

## Selection

Weighted 0-5 scores (benefit 30, feasibility 20, accessibility 20,
sustainability 15, decentralization 15); an award needs 60/100 from at least 3
reviewers including a non-maintainer, and no recipient may exceed 25% of a round.

## Conflicts

Reviewers with a shared employer or funder, or who are the applicant, must
recuse; funders never review.

## Payment rails

Recipient's choice of bank transfer, Open Collective, Liberapay or Solana; a
conventional rail is always offered.

## Independence

An award's `grants` list must be empty, and the business-model flags such as
`tokenOrBlockchainRequired` must stay `false`.

## Disclosure and reporting

Pool, funders, administrative cost (at most 10%), reviewers, scores, recusals,
awards and receipted milestone payments are published; the outcome report is
recomputed by the ledger.

## Community review

At least 14 days on concentration, accessibility and conflict findings.

## Failure, rollback, and recovery

Fix failing records; never waive the gate or delete records. Use `returned`,
`missed`, `waived` and `cancelled` states, and `render` for a stale register.

## Measures

`status` reports money flows, recipients, milestones, concentration and overdue
reports.

## Closure criteria for issue #515

A pilot round is reported, paid on chosen rails, reviewed, and nothing is
overdue (`closure` exits zero).

## Linked automated evidence

`python3 tools/public_interest_funding.py gate` runs in CI with
`app/tests/test_public_interest_funding.py`.
