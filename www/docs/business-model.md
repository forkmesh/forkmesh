# ForkMesh free-protocol / paid-services contract

Contract version 1. The machine-readable source is
[`/business-model.json`](../public/business-model.json); the public entitlement
matrix and homepage pricing render the same boundary.

## The durable boundary

ForkMesh sells work and infrastructure, not permission to participate in the
protocol. The protocol, reference software and self-hosting unit are
MIT-licensed and need no account or payment. Public reading and cloning,
standard Git data, signed `.forkmesh` records, independent operation and
protocol governance can never become paid entitlements. Paid services operate
replaceable roles (hosting, administration, indexing, replicas, recovery,
runners, support) and give ForkMesh no authority over protocol objects.

Tokens, rewards, tips and bounties are optional and never affect identity,
permissions, ranking, moderation or governance
(`tokenOrBlockchainRequired: false`); see
[incentive independence](incentive-independence.md).

## Initial plans

| Plan | Price and status | Boundary |
| --- | --- | --- |
| Protocol + self-hosted | $0; available | Full MIT rights; the operator supplies capacity and support. |
| Hosted Free | $0; public preview | Best-effort public reads, clones, relay and index. No reserved capacity, SLO or support time. |
| Supporter membership | $5/member/month | Funds infrastructure; no extra capability or authority. |
| Pro membership | $10/member/month | Published Pro workflows within safety and capacity limits; not unlimited. |
| Managed services | Research preview; not for sale | Metered hosting, replicas, recovery, runners and support; checkout disabled until research, cost, security, portability and recovery gates pass. |

No paid plan may be `unlimited` for a variable-cost service. Usage units map to
cost drivers (member-month, replica GiB-month, egress GiB, runner minute,
support hour, request counts); unknown cost stays unknown, never zero.

## Portability commitments

Repository data stays Git objects and refs; collaboration data stays signed
`.forkmesh` records. Export is free at any time, including before
cancellation. A managed plan cannot be sold until a drill exports a managed
project, imports it into a clean self-hosted deployment, compares object IDs
and signatures, and restores from the same export.

## Support boundary

Community support covers documentation, discussions and reproducible issues;
free use and memberships carry no response time. A paid support order must
state platform, channel, hours, response target, exclusions and included human
time. Preservation orders use a
[repository availability contract](availability-contract.md), backed only by
demonstrated capacity (`app/tests/test_availability_contract.py`).

## No data sale

ForkMesh does not sell personal data, repository data, collaboration data,
telemetry, or derived profiles, and does not use them to sell advertising.
Optional analytics is off by default and consent-based. First-party ads are
placed by surface, never by viewer (the `advertising` block of the contract).

## Acquisition and paid adoption model

Public projects are the acquisition loop and stay free: anonymous read or
clone, signed contribution or mirror, a qualified service need, a bounded trial
with export available, and a paid proposal only when cost supports the target
margin. The funnel reports aggregate public projects reaching proof, qualified
service conversations, trials, paid customers, trial-to-paid conversion,
revenue, service cost and gross margin, without identifying readers.

## Unit-economics model and instrumentation

```text
gross profit = recognized service revenue - service cost
gross margin = gross profit / recognized service revenue
```

The admin usage report emits a `unitEconomics` projection from provider
analytics; `tools/unit_economics.py` combines it with invoice-backed costs.
Exports carry no raw account, repository, path or IP data. Provider analytics
unavailability is a failed sample, not a zero-cost day.

Managed pricing stays a hypothesis until the
[pricing-interview register](research/pricing-interviews.md) holds qualifying
interviews. Rollout and recovery steps:
[business-model rollout](operations/business-model-rollout.md). Contract test:
`app/tests/test_business_model_contract.py`.
