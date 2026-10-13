# ForkMesh free-protocol / paid-services contract

Published 2026-08-20. Contract version 1.

> ForkMesh lets developers collaborate on, mirror, and preserve source-code
> repositories across independent hosts—without relying on any single hosting
> provider.

The public, machine-readable source for this contract is
[`/business-model.json`](../public/business-model.json). The public
[entitlement matrix](https://forkmesh.com/docs/business-model) and homepage
pricing section render the same boundary in plain language.

## The durable boundary

ForkMesh sells work and infrastructure, not permission to participate in the
protocol.

The protocol, reference software, and self-hosting unit are MIT-licensed. Using,
studying, modifying, forking, and independently implementing them does not
require a ForkMesh account or payment. Public reading and cloning, standard Git
data, signed `.forkmesh` collaboration records, independent operation, and
protocol governance cannot become paid-plan entitlements.

Paid services may operate replaceable roles for a customer: hosting, team
administration, indexing, managed replicas, verified recovery, reliability,
hosted runners, support, and infrastructure. Payment buys accountable service,
reserved capacity, and human time. It gives ForkMesh no special authority over
protocol objects, independent operators, or exported repositories.

Tokens, blockchains, rewards, tips, bounties, and other speculative or
experimental incentives are strictly optional. No plan, protocol right, or
permission requires a wallet or token. No incentive, payment, or asset holding
changes identity, permissions, ranking, moderation, or governance. The
machine-readable contract publishes this as `tokenOrBlockchainRequired: false`
and `incentivesAffectIdentityPermissionsRankingModerationOrGovernance: false`.
See [incentive independence](incentive-independence.md) for the adapter
boundary and the payment and no-payment paths.

## Initial plans

| Plan | Price and status | Boundary |
| --- | --- | --- |
| Protocol + self-hosted | $0 forever; available | Complete MIT rights. The operator supplies capacity, backups, moderation, uptime, and support. |
| Hosted Free | $0; public preview | One personal account, anonymous public reads/clones, and best-effort shared relay/index. No managed private workspace, reserved replica, SLO, or priority support entitlement. |
| Supporter membership | $5/member/month; launch membership | Funds public infrastructure and adds membership recognition, but no hosted capability, reserved capacity, SLO, priority support, or protocol authority. |
| Pro membership | $10/member/month; launch membership | The currently published Pro workflows where configured and membership recognition. It is bounded by safety/capacity controls and is not an infrastructure SLO or unlimited plan. |
| Managed services | Research preview; not for sale | Proposed metered hosting, team administration, indexing, replicas, recovery, runners, support, and reliability. Checkout remains disabled until the research, cost, security, portability, and recovery gates pass. |

`Hosted Free` is deliberately described by its service boundary, not by an
invented unlimited allowance. Public relay and index use is best effort under
published security, abuse, and provider-capacity safeguards. There is no
reserved capacity, private-workspace GA claim, contractual uptime, or human
response time. When a numeric free allowance becomes enforceable, it must be
added to `business-model.json`, the UI, and the metering tests in the same
change.

## Usage units

Managed proposals use units that map to an observable cost driver:

- active member-month;
- indexed project-month;
- managed replica GiB-month;
- verified egress GiB;
- hosted runner minute;
- priority support hour;
- edge Worker request;
- Durable Object request; and
- Durable Object GB-second.

No paid plan may use `unlimited` for a variable-cost service. An allowance and
overage are not publishable until the unit can be reconciled to its provider
invoice or support ledger. Unknown usage or cost remains **unknown**; it is
never converted to zero to improve margin.

## Portability commitments

Canonical repository data remains Git objects and refs. Canonical
collaboration data remains signed `.forkmesh` repository records. Customers
may clone and export those formats without an upgrade or export fee, including
before cancellation. Billing-provider identifiers, abuse evidence, and
operational logs are service records, not canonical protocol data.

Before a managed plan can be sold, an end-to-end drill must export a managed
project, import it into a clean self-hosted deployment, compare Git object ids
and signed collaboration records, then restore the managed service from the
same export. Cancellation must not rotate project identity, rewrite signatures,
or require a proprietary verifier.

## Support boundary

Community support covers public documentation, discussions, and reproducible
issue reports. Security and network-safety incidents may be prioritized, but
free use and memberships carry no response or resolution time.

A future paid support order must name its supported platform, contact channel,
hours, response target, exclusions, escalation path, data-access boundary, and
the quantity of human time included. ForkMesh does not promise a service level
for a self-hosted system it does not operate. The current supported proof
surfaces are a modern browser plus standard Git, desktop clients on macOS,
Windows, and Linux, and the Linux headless mirror service. Mobile and editor
integrations do not become managed source-host controls merely because a
customer pays.

A repository-preservation order is written as a
[repository availability contract](availability-contract.md). The contract
names its copies, operator and region diversity, uptime, coverage,
verification frequency, recovery and response objectives, exclusions, and
credits. Only capacity that continuous probes and restore drills have
demonstrated can back one, and every period report reproduces from its
evidence. The contract model does not open managed checkout; the gates above
still apply.

## No data sale

ForkMesh does not sell personal data, repository data, collaboration data,
telemetry, or derived profiles, and does not use them to sell advertising.
Data is processed only to provide, secure, support, measure, and improve the
selected service under the public privacy policy. Optional product analytics
stays disabled by default and accepts only fixed, coarse action names after
explicit consent.

[ForkMesh Ads](operations/ad-network.md), the first-party text ad network,
is built inside that rule: an ad is placed by product surface and never by
viewer, the serve call reads no account or history, and the only measurement
is an aggregate click count. Its membership allowances are the `advertising`
block of the machine-readable contract.

## Acquisition and paid adoption model

The public-project loop is the acquisition product, not a loss leader that can
later be revoked:

1. A maintainer discovers ForkMesh through a public project or preservation
   need and completes an anonymous read or clone.
2. The project adds a signed contribution or independent mirror and observes
   the four-step product proof.
3. Repeated collaboration creates a qualified service need: less operator
   toil, coordinated team policy, faster indexing, managed recovery, an SLO,
   or accountable support.
4. A buyer enters a bounded trial for that paid job. Export stays available
   throughout the trial.
5. A paid proposal is accepted only when observed service cost supports the
   target gross margin; protocol rights and public-project value remain free.

The funnel reports aggregate public projects reaching proof, qualified service
conversations, trials, paid customers, trial-to-paid conversion, recognized
revenue, service cost, gross profit, and infrastructure margin. It does not
identify public readers or treat optional analytics as required surveillance.

## Unit-economics model and instrumentation

For a closed accounting window:

```text
qualified-demand rate = qualified service conversations / projects reaching proof
trial conversion      = paid customers / completed trials
service cost          = metered infrastructure + managed hosts + runners + support
gross profit          = recognized service revenue - service cost
gross margin          = gross profit / recognized service revenue
```

The admin-signed `/api/accounts/admin-usage-report` already reads actual
Cloudflare account analytics and now emits a stable `unitEconomics` projection
for Worker requests, Durable Object requests, and duration. It retains storage
operations and observed response bytes as provider diagnostics.
`tools/unit_economics.py` validates a period record, combines those quantities
with invoice-backed infrastructure, managed-host, runner, and support costs,
and calculates the funnel and margin without binary floating-point money.

The report is network aggregate. A paid trial that shares infrastructure must
add privacy-reviewed tenant attribution using a keyed blind index; raw account,
repository, path, IP, or request data must not enter economics exports. Provider
analytics unavailability is a failed sample, not a zero-cost day.

## Pricing research gate

The initial managed plan is a hypothesis, not validated pricing. The
[pricing-interview register](research/pricing-interviews.md) currently records
zero qualifying buyer interviews and therefore keeps managed checkout off.
Only consented, attributable real interviews can validate economic buyer,
budget ownership, and top paid jobs. Blank, synthetic, inferred, employee-only,
or public-comment records do not count.

## Failure, rollback, and recovery

- If the public JSON and visible matrix diverge, the business-model contract
  test fails and the site must not deploy.
- If telemetry, invoice, runner, host, or support cost is missing, the period is
  incomplete and gross margin is not reported as healthy.
- If interview evidence is below its threshold, managed checkout stays disabled
  and the page remains a research preview.
- If an entitlement change would remove a permanent commitment, roll back the
  marketing deployment; no protocol object or user data migration is involved.
- If a managed-service trial fails, export canonical data, verify it in a clean
  self-host, restore the last tested service configuration, and retain sanitized
  operational evidence for review.

The operational sequence, owner checklist, and exact evidence commands are in
[business-model rollout and recovery](operations/business-model-rollout.md).

## Linked automated evidence

- Public entitlement, portability, support, research, privacy, and plan
  contract: `app/tests/test_business_model_contract.py`
- Unit-economics success, malformed input, missing cost, and zero-revenue paths:
  `app/tests/test_unit_economics.py`
- Live provider-usage projection and analytics failure paths:
  `app/tests/test_admin_usage_report.py`
- Supported installer platforms: `app/tests/test_installer_release.py`
- Source-offline failure and recovery: `app/tests/test_mirror_serving.py`,
  `app/tests/test_https_mirror_routing_integration.py`, and
  `app/tests/test_mirror_service_recovery.py`
- Default-off, allowlisted commercial telemetry:
  `app/tests/test_first_party_analytics.py`
- Optional incentives isolated from canonical state and decisions:
  `app/tests/test_incentive_independence.py`
- Repository availability contracts, demonstrated capacity, simulated
  operator and regional failure, SLO misses, and credits:
  `app/tests/test_availability_contract.py`

Automated evidence does not replace actual interviews, provider invoices, an
export/restore drill, security review, current support documentation, or a
production rollout window.
