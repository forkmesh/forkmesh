# Business-model rollout and recovery

This runbook publishes the ForkMesh free-protocol / paid-services boundary
without granting the hosted operator control over protocol participation.

## Owners and release gate

Name a product owner, infrastructure-cost owner, support owner, and privacy or
legal reviewer before enabling a managed checkout. The initial contract is a
research preview and managed checkout must remain disabled while any gate in
`www/public/business-model.json` is open.

Run from the repository root:

```sh
python3 tools/unit_economics.py validate-contract
python3 tools/quality_gate.py run --profile core
```

For a candidate accounting window, export the admin usage report, add
invoice-backed costs and aggregate funnel counts using the period schema, then
run:

```sh
python3 tools/unit_economics.py report --input /path/to/period.json
python3 tools/unit_economics.py launch-gate --input /path/to/period.json
```

The launch gate is expected to fail until real interview evidence is marked
validated. Do not bypass it by adding synthetic research or zero-valued missing
costs.

## Supported surfaces

- Contract page and homepage pricing: current standards-based desktop and mobile
  browsers served by the WWW Worker.
- Portable proof: standard Git plus the web app in a current browser.
- Self-hosted desktop: macOS, Windows, and Linux packages covered by the release
  installer matrix.
- Managed mirror evidence: Linux headless mirror service.

Mobile and editor integrations can collaborate but are not advertised as
managed source-host controls. Self-hosted deployments receive documentation and
community support unless a separate, explicit support order names the platform.

## Deployment

1. Validate the JSON, visible matrix, docs links, analytics allowlist, and unit
   economics tests.
2. Confirm `/business-model.json`, `/docs/business-model`, `/#pricing`,
   `/privacy`, and `/terms` are in the WWW asset manifest and return on a local
   Worker smoke test.
3. Confirm the managed plan says `research-preview-not-for-sale`, contains no
   checkout control, and no variable-cost service says unlimited.
4. Deploy WWW normally. Deploy App only when the stable admin usage projection
   changed and its focused tests pass.
5. Record the commit, test run, reviewer decisions, live URLs, and screenshot or
   text capture of the entitlement matrix.

## Failure paths

| Failure | Required behavior | Recovery |
| --- | --- | --- |
| Contract JSON is malformed or drifts from the visible matrix | CI/deploy fails | Correct the source and matrix together; do not hand-edit production |
| Provider analytics is unconfigured | Usage endpoint returns `analytics_unconfigured`; cost is unknown | Restore the scoped analytics token, retry, and retain the missing interval |
| Provider analytics is unavailable | Usage endpoint returns `analytics_unavailable`; never record zero | Retry after provider recovery or reconcile the provider invoice manually |
| Invoice, host, runner, or support cost is missing | Period report is incomplete and launch gate fails | Attach an attributable source and rerun; do not estimate it as zero |
| Interview threshold is not met | Managed plan remains a research preview with no checkout | Conduct and record real qualifying interviews |
| Export or restore differs | Trial stops; no paid reliability claim | Preserve evidence, restore the last verified copy, repair the exporter, repeat on a clean deployment |
| Optional analytics is denied or blocked | No commercial product event is sent | Continue without tracking that visitor; never make analytics consent a service condition |

## Rollback

The public contract deployment changes no protocol object, repository,
identity, entitlement row, or customer data. Roll back WWW to the last reviewed
asset revision and invalidate only the changed static assets. If the App usage
projection must be rolled back, retain the newer JSON export as operational
evidence; old clients ignore unknown response fields.

Never roll back by deleting canonical repositories or signed collaboration
records. A pricing rollback cannot revoke MIT rights or make an export paid.

## Recovery and portability drill

Before the first managed trial and each material format change:

1. Select a disposable project with Git refs and signed collaboration records.
2. Export it from the managed service without using a paid-only exporter.
3. Import it into a clean self-hosted deployment with hosted routes disabled.
4. Compare all Git refs/object ids and signed `.forkmesh` records.
5. Clone and collaborate from the self-host, then restore the managed service
   from the same export.
6. Record duration, bytes, missing objects, conflicts, recovery outcome, and
   sanitized logs. Any mismatch blocks launch.

## Telemetry and privacy

The live infrastructure projection is aggregate at network level and contains
only fixed unit ids and quantities. Economics period files may contain aggregate
funnel counts, recognized revenue, and cost-source labels. They must not contain
raw account names, repository names, paths, IP addresses, request ids, provider
customer ids, interviewee identity, or confidential notes.

Pricing-page product actions use the existing default-off first-party analytics controller.
Only allowlisted action names are sent after explicit consent; denial, Global
Privacy Control, and Do Not Track continue to disable them.
