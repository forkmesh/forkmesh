# Business-model rollout and recovery

This runbook publishes the free-protocol / paid-services boundary without
giving the hosted operator control over protocol participation.

## Owners and release gate

Name a product owner, infrastructure-cost owner, support owner, and privacy or
legal reviewer before enabling a managed checkout. Managed checkout stays
disabled while any gate in `www/public/business-model.json` is open.

```sh
python3 tools/unit_economics.py validate-contract
python3 tools/quality_gate.py run --profile core
python3 tools/unit_economics.py report --input /path/to/period.json
python3 tools/unit_economics.py launch-gate --input /path/to/period.json
```

A period file combines the admin usage report, invoice-backed costs, and
aggregate funnel counts. Never pass the launch gate with synthetic evidence or
zero-valued missing costs.

## Supported surfaces

- Contract page and pricing: current desktop and mobile browsers.
- Portable proof: standard Git plus the web app.
- Self-hosted desktop: macOS, Windows, and Linux release packages.
- Managed mirror evidence: the Linux headless mirror service.

Self-hosted deployments receive documentation and community support unless a
separate support order names the platform.

## Deployment

1. Validate the contract JSON, visible matrix, docs links, analytics allowlist,
   and unit-economics tests.
2. Confirm `/business-model.json`, `/docs/business-model`, `/#pricing`,
   `/privacy`, and `/terms` are served.
3. Confirm the managed plan says `research-preview-not-for-sale`, has no
   checkout control, and no variable-cost service says unlimited.
4. Deploy the site; deploy the app only when the admin usage projection changed.

## Failure paths

| Failure | Required behavior | Recovery |
| --- | --- | --- |
| Contract JSON malformed or drifts from the matrix | CI/deploy fails | Fix source and matrix together |
| Provider analytics unconfigured | Usage endpoint returns `analytics_unconfigured`; cost unknown | Restore the scoped token and retry |
| Provider analytics unavailable | Usage endpoint returns `analytics_unavailable`; never record zero | Retry later or reconcile the invoice |
| A cost source is missing | Period report incomplete; launch gate fails | Attach an attributable source |
| Export or restore differs | Trial stops | Restore the last verified copy, fix the exporter, repeat |
| Optional analytics denied | No product event is sent | Continue; consent is never a service condition |

## Rollback

The contract deployment changes no protocol object, repository, identity,
entitlement, or customer data. Roll the site back to the last reviewed asset
revision. Never roll back by deleting repositories or signed collaboration
records; a pricing rollback cannot revoke license rights or make an export paid.

## Recovery and portability drill

Before the first managed trial and each material format change: export a
disposable project from the managed service without a paid-only exporter,
import it into a clean self-hosted deployment with hosted routes disabled,
compare all Git refs, object ids, and signed `.forkmesh` records, collaborate
from the self-host, then restore the managed service from the same export. Any
mismatch blocks launch.

## Telemetry and privacy

The infrastructure projection is aggregate and holds only fixed unit ids and
quantities. Period files may hold aggregate funnel counts, revenue, and cost
labels, never account or repository names, paths, IP addresses, request ids,
provider customer ids, or interviewee identity. Pricing-page events use the
default-off first-party analytics controller and are sent only after explicit
consent; Global Privacy Control and Do Not Track disable them.
