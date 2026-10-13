# Protocol and ecosystem governance

Charter version: `1.0.0-rc.1` · Status: **proposed** (issue #513). The charter
is itself the first proposal,
[FMP-0001](decision-register.md#fmp-0001), and follows the process it
describes.

The machine-readable charter is
[`conformance/governance/v1/charter.json`](../../../conformance/governance/v1/charter.json).
Every proposal, security decision, and appeal is a JSON record in
[`conformance/governance/v1/records/`](../../../conformance/governance/v1/records/).
Participants, seat holders, and conflict and funding disclosures are in
[`participants.json`](../../../conformance/governance/v1/participants.json).
The public [decision register](decision-register.md) and the searchable
`/docs/governance/register.json` are generated from those files. A hand edit
to either output fails CI.

This charter governs the ForkMesh protocol suite, certification program,
compatibility commitments, and the moderation baseline. It does not govern an
individual repository. Repository authority still comes only from that
repository's policy document, as the
[collaboration protocol](../design/conflict-safe-collaboration.md) requires.

## Roles

| Body | Decides | Seats |
| --- | --- | --- |
| Protocol council | Protocol, compatibility, deprecation, certification, moderation baseline, the charter | 2 ForkMesh maintainers, 2 independent implementers, 2 community operators |
| Security response team | Security decisions | 1 ForkMesh maintainer, 1 independent implementer, 1 community operator |
| Appeals panel | Appeals | 1 independent implementer, 1 community operator, 1 community member |

Constituencies:

- **ForkMesh maintainer:** has write access to `main`, or is paid by the
  operator of forkmesh.com.
- **Independent implementer:** maintains a client, relay, mirror, index, or
  verifier that is not ForkMesh code, has a public https source, and holds or
  is pursuing a [certification badge](../protocol/certification.md). Is not
  paid by the operator of forkmesh.com.
- **Community operator:** operates a public mirror, relay, gateway, or
  moderation service that other people use. Is not paid by the operator of
  forkmesh.com.
- **Community member:** has an active ForkMesh identity with signed
  contribution history and holds no other seat.

Seats are held by people, not groups. A person holds at most one seat in each
body.

### Quorum

A decision counts only when:

- enough seat holders vote: 4 for the council, 2 for the security team, and 3
  for the appeals panel;
- the required constituencies vote: independent implementers and community
  operators on the council and appeals panel, and an independent implementer
  on the security team;
- no single employer or funder casts more than half of the votes, or more than
  a third on the appeals panel.

ForkMesh maintainers cannot decide anything alone, because an empty
independent seat blocks quorum and no other constituency can fill it. While
the independent seats are vacant, proposals can be opened and reviewed, but
none can be decided.

## Proposals

1. **Draft.** Open a record `FMP-NNNN` naming the affected requirements,
   threats, protected rights, and disclosures. Normative protocol changes
   still follow the suite's [change process](../protocol/README.md#change-process).
2. **Review:** at least 14 days, or 30 days for a breaking change. Anyone may
   comment. The record sets `dissentSolicited: true`.
3. **Last call:** at least 7 days after review closes.
4. **Decision:** due within 14 days after last call. Each vote names the seat
   and a rationale. A `no` vote without a recorded dissent statement fails
   validation. Dissent from anyone, not only seat holders, is recorded with a
   date.
5. **Implemented**, or **withdrawn** at any stage.

## Security decisions

Report privately as described in [SECURITY.md](../../../SECURITY.md).

- Acknowledge within 3 days.
- The embargo lasts at most 90 days.
- A public `FMS-NNNN` record is published within 7 days of disclosure.
- The security response team ratifies the decision within 30 days of
  disclosure.

A fix may ship under embargo before ratification. The record then shows who
decided, when, and any dissent. Outcomes are `mitigated`, `accepted` (with a
threat-register entry naming reviewer, date, and rationale), `rejected`, or
`deferred`.

## Appeals

Anyone affected by a decision may appeal it within 30 days by opening
`FMA-NNNN`. The appeals panel decides within 30 days of filing. Nobody who
wrote or voted on the original decision may vote on the appeal. Outcomes are
`upheld`, `overturned`, or `remanded`. Certification appeals follow the same
timeline; see [appeals and revocation](../protocol/certification.md#appeals-and-revocation).

## Deprecation and compatibility

- Wire version identifiers name frozen bytes and never change meaning.
- Only a major suite version removes acceptance of a wire identifier. The
  removal is announced at least one minor release and 180 days ahead, and
  ships with a migration path.
- Details are in the [compatibility policy](../protocol/README.md#compatibility-policy).

## Conflicts and funding

- Every author and voter on a record must link a disclosure in that record.
  Validation rejects a record without one.
- A disclosure lists conflicts and every funding source by kind: membership,
  donation, grant, contract, investment, reward pool, employment, or none.
- A disclosure is current for 180 days.
- A funding entry without a published amount is shown as "not published" and
  keeps the closure criterion open.
- The register lists, for each disclosure, every record it is linked to.

## Protected rights

No plan, token, hosted-service setting, or funder can remove these rights:

| Right | Guarantee | Business-model commitments |
| --- | --- | --- |
| Free protocol | Implementing, running, and certifying the protocol requires no payment, plan, or ForkMesh account | `protocolUseRequiresPayment`, `selfHostingRequiresPayment`, `independentImplementationsAllowed` |
| Portable identity | Identities and signed history export and recover without operator permission | `portableSignedCollaborationRequiresPaidPlan` |
| Portable repositories | Git data and signed records can be read, cloned, and moved off any host | `publicReadAndCloneRequirePaidPlan`, `portableGitDataRequiresPaidPlan` |
| Token-free governance | No asset, payment, plan, or funding amount grants a seat, vote, standing, or weight | `governanceRequiresPaidPlan`, `tokenOrBlockchainRequired`, `incentivesAffectIdentityPermissionsRankingModerationOrGovernance` |

Validation fails if any cited
[business-model](../business-model.md) commitment no longer holds. A proposal
that narrows a protected right must be breaking. It needs a two-thirds vote,
including a yes from every filled independent seat. See also
[incentive independence](../incentive-independence.md).

## Moderation baseline

- Every moderation action on a signed record is itself a signed record that
  names the moderator key and the rule applied.
- Hiding content never deletes the underlying signed record. Another host may
  serve it under its own policy.
- A person whose content or account is actioned is told the rule applied and
  may appeal.
- Operators publish their own policy. The baseline is a floor for every host,
  and no operator's policy binds another operator's host.

## Implementation-neutral participation

- Participation needs no ForkMesh client, hosted account, or plan. Proposals,
  comments, and dissent may arrive through any forge mirror of this repository
  or by email, and a maintainer records them with attribution.
- Certification evidence, not the choice of implementation, qualifies an
  independent-implementer seat.

## Succession

- A vacant seat is filled within 60 days. Until then it counts as absent.
- If the operator of forkmesh.com stops operating, the protocol council holds
  the suite, vectors, and certification registry. Anyone may republish them
  under the MIT license.
- Organization-level succession is described in
  [organization succession](../operations/organization-succession.md).

## Running the ledger

```bash
python3 tools/protocol_governance.py validate      # charter, disclosures, records
python3 tools/protocol_governance.py render        # regenerate the register
python3 tools/protocol_governance.py gate          # validate + render --check (CI)
python3 tools/protocol_governance.py status        # deadlines, stale disclosures, blockers
python3 tools/protocol_governance.py closure       # non-zero until #513 can close
```

`gate` never reads today's date, so it cannot fail on the calendar.
`status` and `closure` accept `--as-of YYYY-MM-DD`.

## Failure, rollback, and recovery

- **A record breaks the charter** (short review window, missing disclosure,
  undocumented `no`, missing quorum): `validate` fails and the
  `protocol-governance` quality gate blocks the change. Fix the record. Do not
  waive the gate.
- **A decision is overturned on appeal:** revert the implementing change and
  mark the original record with the appeal outcome. The record is never
  deleted.
- **A deadline passes:** `status` lists the record under `overdue` and
  `closure` fails with `timelines-met`. Record a decision, an extension as a
  new dated note, or a withdrawal.
- **The register is stale or hand-edited:** run `render` and commit the result.

## Measures

`status` reports, and each monthly [outcome review](../outcome-review.md)
records:

- proposal participation: comments and dissent entries per record;
- decision latency: days from `opened` to `decided`;
- appeals filed and their outcomes;
- independent representation: filled independent seats, and independent votes
  per decision;
- policy compliance: overdue records and stale disclosures. The target is zero.

## Closure criteria for issue #513

`closure` exits non-zero until every item holds. At charter adoption, all of
them are open:

- **proposal-completed:** a proposal adopted under the charter is decided with
  a quorum, full timeline, and solicited dissent.
- **security-decision-completed:** a security decision adopted under the
  charter is decided and ratified with a quorum.
- **independent-roles:** the council has independent-implementer and
  community-operator seat holders who voted in a completed decision.
- **disclosures-current:** every active participant has a current disclosure,
  and every funding amount is published.
- **timelines-met:** nothing is overdue.

The two pre-charter records, [FMP-0000](decision-register.md#fmp-0000) and
[FMS-0001](decision-register.md#fms-0001), are kept for public history. They
do not count toward closure.

## Linked automated evidence

- Charter, disclosure, timeline, quorum, dissent, protected-right, and
  register checks: `app/tests/test_protocol_governance.py`
- Quality gate: `protocol-governance` in
  [`quality-gates.json`](../engineering/quality-gates.json), in the `core` and
  `release` profiles
- CI: `.github/workflows/collaboration-conformance.yml` runs `gate` on Linux,
  macOS, and Windows.

Automated checks do not replace the seated independent reviewers, the real
decisions, or the published funding amounts that closure requires.
