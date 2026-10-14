# Protocol and ecosystem governance

The charter governs the protocol suite, certification, compatibility and the
moderation baseline; repositories follow their own policy documents. Charter,
records and participant disclosures live in
[`conformance/governance/v1/`](../../../conformance/governance/v1/charter.json);
the [decision register](decision-register.md) and
`/docs/governance/register.json` are generated from them.

## Roles

| Body | Seats | Quorum |
| --- | --- | --- |
| Protocol council | 2 maintainers, 2 independent implementers, 2 community operators | 4 |
| Security response team | 1 maintainer, 1 independent implementer, 1 community operator | 2 |
| Appeals panel | 1 independent implementer, 1 community operator, 1 community member | 3 |

Independent constituencies must vote, and no employer or funder may cast more
than half the votes, so maintainers cannot decide alone.

## Proposals

`FMP-NNNN`: review at least 14 days (30 if breaking), last call 7 days, decision
within 14 days. A `no` vote needs a dissent statement. Narrowing a protected
right (free protocol, portable identity and repositories, token-free
governance) needs two thirds. See the [protocol suite](../protocol/README.md).

## Security decisions

Report as in [SECURITY.md](../../../SECURITY.md): acknowledge in 3 days, embargo
at most 90 days, publish `FMS-NNNN` within 7 days of disclosure, ratify within 30.

## Appeals

`FMA-NNNN` within 30 days, decided within 30 days by uninvolved panel members.

## Running the ledger

`python3 tools/protocol_governance.py validate|render|gate|status|closure`;
tests are `app/tests/test_protocol_governance.py`. Disclosures stay current for
180 days. Records are fixed or annotated, never deleted.
