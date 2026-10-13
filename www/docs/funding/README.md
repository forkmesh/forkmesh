# Public-interest funding

Program version: `1.0.0-rc.1` · Status: **proposed** (issue #515). The pilot
round, [PIF-0001](funding-register.md#pif-0001), is a **draft**. No funder has
committed a pool and no reviewer is seated, so it has no applications,
selections, or payments yet.

The program pays for work that the whole ecosystem uses: public repositories,
volunteer mirrors, accessibility, documentation, education, and independent
tooling. Being funded never changes who can reach a repository, what an
identity may do, who votes, or what the network requires.

The machine-readable program is
[`conformance/funding/v1/program.json`](../../../conformance/funding/v1/program.json).
Each funding round is a JSON record in
[`conformance/funding/v1/rounds/`](../../../conformance/funding/v1/rounds/).
Reviewers and their conflict disclosures come from the
[governance ledger](../governance/README.md). The public
[funding register](funding-register.md) and the searchable
`/docs/funding/register.json` are generated from those files. A hand edit to
either output fails CI.

## What the program offers

| Offer | Status | How it works |
| --- | --- | --- |
| Donation | planned | A one-off gift to the pool. A donor may restrict it to an area, never to a named recipient. |
| Recurring sponsorship | planned | Monthly or yearly support for the pool or one area. A sponsor can stop at any time. |
| Targeted grant | planned | A milestone-based award chosen in a published round. |
| Matching | planned | The pool matches public contributions a recipient raised on their own rail, up to 2:1. |

The pilot round will make targeted grants and matching available. Donations
and sponsorship open once the first round shows the pool can be administered
and reported in public.

These offers are separate from the support that already exists. Solana tips,
issue bounties, and profile and repository donations go straight to the person
who published the address. Memberships fund forkmesh.com. None of them passes
through this pool, and all of them stay optional adapters under
[incentive independence](../incentive-independence.md).

## Eligibility

Areas and their rules:

- **Public repositories:** anyone can read and clone the repository without an
  account, and it uses an OSI-approved or Creative Commons license. It may be
  hosted anywhere.
- **Volunteer mirrors:** a public mirror, relay, or gateway that other people
  have used for at least 60 days, with a health endpoint or status page.
- **Accessibility:** work on a ForkMesh-compatible client, website, or document
  measured against WCAG 2.2 AA or platform guidelines, tested with at least one
  person who uses assistive technology.
- **Documentation:** public documentation, translations, or tutorials under an
  open license.
- **Education:** free courses, workshops, or learning materials about
  decentralized collaboration, under an open license.
- **Ecosystem tooling:** an independent client, relay, index, verifier, or
  library with a public https source under an OSI-approved license. A
  [certification badge](../protocol/certification.md) is welcome but not
  required.

Every applicant must also meet these rules:

- Individuals, informal groups, and organizations may apply from any
  jurisdiction the funder can lawfully pay.
- Applying, being reviewed, and being paid need no ForkMesh account, plan,
  client, wallet, or token.
- The operator of forkmesh.com and the people it pays are not eligible for
  awards.
- A round's funders and reviewers are not eligible in that round.
- Work that needs a paid plan on a ForkMesh-operated service to use is not
  eligible.

## Applying

Each round lists its application channels. Every round offers at least one
channel that needs no ForkMesh account. The pilot accepts an issue labelled
`funding-application` on any forge mirror of `forkmesh/forkmesh`, or a private
email to the address in the round announcement. The application window is at
least 21 days.

An application names the applicant, a public https source, the applicant's
employer or funder (or `none`), the area, the offer, the amount requested, and
milestones.

## Selection

Reviewers score each criterion from 0 to 5:

| Criterion | Weight |
| --- | --- |
| Public benefit | 30 |
| Feasibility | 20 |
| Accessibility and inclusion | 20 |
| Sustainability | 15 |
| Decentralization: a new operator, region, implementation, or maintainer | 15 |

The weighted score is out of 100. An award needs:

- at least 60 points;
- scores from at least 3 reviewers, at least 1 of whom is not a ForkMesh
  maintainer;
- an eligible application; and
- room in the pool after administrative cost.

Awards go to the highest-scoring applications. No recipient may receive more
than 25% of a round's awards. A matching award may not exceed the matched
contributions times the ratio, and the contributions need a public receipt.

## Conflicts

- Reviewers are governance participants with a current disclosure in
  [`participants.json`](../../../conformance/governance/v1/participants.json).
- A reviewer who is the applicant, or who shares an employer or funder with the
  applicant, must record a recusal and must not score that application. The
  ledger detects these matches and fails until the recusal is recorded.
- Any other conflict a reviewer declares is recorded and recused the same way.
- A funder never reviews, never chooses a recipient, and may restrict a gift
  only to an area.
- Recusals are published with the round, beside the selections they affected.

## Payment rails

The recipient chooses the rail for each award. Every rail pays into an account
the recipient controls and needs no ForkMesh account or plan.

| Rail | Kind | Recurring |
| --- | --- | --- |
| Bank transfer (SEPA, ACH, or international wire) | conventional | yes |
| Open Collective, through the recipient's own fiscal host | conventional | yes |
| Liberapay | conventional | yes |
| Solana transfer to an address the recipient controls | on-chain | no |

Every round offers at least one conventional rail. Solana is never the only
rail. A rail that needs a ForkMesh account, a plan, or a hosted balance fails
validation.

## Independence

An award, donation, or sponsorship pays for work. It never grants repository
access, identity rights, protocol votes, governance seats, ranking or
discovery, moderation outcomes, required network behavior, or use of a hosted
product. An award's `grants` list must be empty.

The only obligations an award may carry are a milestone report, an outcome
report, an open license, an accessibility statement, a public receipt, and a
conflict disclosure.

The ledger checks that these business-model commitments still hold:
`protocolUseRequiresPayment`, `publicReadAndCloneRequirePaidPlan`,
`governanceRequiresPaidPlan`, `tokenOrBlockchainRequired`, and
`incentivesAffectIdentityPermissionsRankingModerationOrGovernance` are all
`false`. It also checks that the governance charter still protects
`token-free-governance`. If any of these changes, the gate fails.

## Disclosure and reporting

Before applications open, a round publishes:

- its committed pool, and each funder's name, funding kind, amount, conflicts,
  and any area restriction. Funders must cover the whole pool;
- its administrative cost, capped at 10% of the pool. This covers payment fees,
  accounting, and reviewer stipends;
- its reviewers and their disclosures; and
- its application window.

As the round runs, it publishes every application with its eligibility result
and weighted score, every recusal, every award and its milestones, and every
disbursement with a public receipt. A payment is made only after its milestone
report is accepted.

When the round ends, it publishes an outcome report with:

- funds raised, awarded, and disbursed;
- administrative cost;
- applications and recipients;
- milestones accepted out of the total;
- volunteer-mirror operators funded, and how many still operate;
- accessibility impact; and
- lessons, including what did not work.

The ledger recomputes each figure, so a report that disagrees with the ledger
fails validation. A cancelled round also publishes an outcome report and its
lessons.

## Community review

After selections are published, community review stays open for at least 14
days. Anyone may comment through an issue on any forge mirror or by email, and a
maintainer records each finding with attribution. Findings cover three risks:

- **Concentration:** one recipient, area, region, or funder holding too much of
  a round. The ledger warns when more than 50% of awards go to one area or more
  than 50% of the pool comes from one funder.
- **Accessibility:** forms, channels, or criteria that exclude people.
- **Conflict:** undisclosed ties between reviewers, funders, and applicants.

Each finding is `corrected` with the correction recorded, `accepted-risk` with a
rationale, or `open`. A finding raised only by a ForkMesh maintainer does not
count as independent review.

## Round lifecycle

| Stage | What the ledger requires |
| --- | --- |
| `draft` | Areas, offers, rails, and application channels. No applications. |
| `open` | Announcement and application window, pool, funders, administrative cost, and reviewers. |
| `reviewing` | Scores on every criterion. Every detected conflict is recused. |
| `selected` | Published selections. Each eligible application meets the reviewer quorum. Awards meet the minimum score and concentration cap. |
| `disbursing` | Payments against accepted milestones, on round rails, with receipts. |
| `reported` | No pending milestones, a community review window, and an outcome report that matches the ledger. |
| `cancelled` | The stage it was cancelled from, and an outcome report with lessons. |

## Running the ledger

```bash
python3 tools/public_interest_funding.py validate   # program and rounds
python3 tools/public_interest_funding.py render     # regenerate the register
python3 tools/public_interest_funding.py gate       # validate + render --check (CI)
python3 tools/public_interest_funding.py status     # measures, overdue reports, warnings, blockers
python3 tools/public_interest_funding.py closure    # non-zero until #515 can close
```

`gate` never reads today's date, so it cannot fail on the calendar.
`status` and `closure` accept `--as-of YYYY-MM-DD`.

Program changes publish a new version in `program.json`. A change applies only
to rounds announced at least 14 days later, and a round keeps the version it
was announced under.

## Failure, rollback, and recovery

- **A round breaks the program** (unpublished funding, a missing recusal, a
  score below the minimum, a concentrated award, an award that grants a right):
  `validate` fails and the `public-interest-funding` quality gate blocks the
  change. Fix the record. Do not waive the gate.
- **A payment bounces or is sent in error:** mark the disbursement `returned`
  with a note, then record a new `sent` disbursement. Returned payments do not
  count as disbursed.
- **A milestone is missed:** mark it `missed` with its report, or `waived` with
  a note. No payment follows a milestone that has not been accepted.
- **A round cannot proceed** (no pool, no reviewers, a conflict that can't be
  resolved): set `stage` to `cancelled`, record `cancelledFrom`, and publish the
  outcome report and its lessons. Unspent funds stay in the pool or go back to
  funders, as the report states.
- **A finding shows an award was wrongly selected:** record the finding and its
  correction. Stop the remaining disbursements by leaving their milestones
  unaccepted. Records are corrected, never deleted.
- **The register is stale or hand-edited:** run `render` and commit the result.

## Measures

`status` reports these per round, and each monthly
[outcome review](../outcome-review.md) records them:

- funds raised, awarded, and disbursed;
- administrative cost as a share of the pool;
- applications and distinct recipients;
- milestone completion;
- volunteer-mirror operators funded and still operating at the outcome report;
- accessibility awards and reported accessibility impact;
- largest recipient, area, and funder share of a round; and
- overdue milestone and outcome reports. The target is zero.

Measures come only from the ledger. Product analytics record no applicant,
recipient, amount, or payment identifier.

## Closure criteria for issue #515

`closure` exits non-zero until every item holds. At program publication, the
first three are open:

- **pilot-round-reported:** a pilot round is `reported` with applications,
  conflict handling, selections, disbursements, milestones, and an outcome
  report.
- **recipients-paid-on-portable-rails:** every recipient in that round was paid
  on a rail they chose.
- **community-review-corrected:** independent community findings cover
  concentration, accessibility, and conflict. No finding is open, and every
  concentration warning is addressed.
- **reports-on-time:** no milestone or outcome report is overdue.

## Linked automated evidence

- Eligibility, conflicts, selection quorum and score, concentration, rails,
  independence, disbursements, returned payments, cancellation, overdue
  reports, measures, and register checks:
  `app/tests/test_public_interest_funding.py`
- Quality gate: `public-interest-funding` in
  [`quality-gates.json`](../engineering/quality-gates.json), in the `core` and
  `release` profiles. The tests also run in the `incentive-independence` gate.
- CI: `.github/workflows/collaboration-conformance.yml` runs `gate` and the
  tests on Linux, macOS, and Windows.
- Program `closureEvidence` links each kind of evidence (supported platforms,
  failure paths, rollback and recovery, documentation, and telemetry) to a test
  or file. Validation fails if a link goes missing.

Automated checks do not replace a committed pool, seated reviewers, real
applications and payments, or independent community review, all of which
closure requires.
