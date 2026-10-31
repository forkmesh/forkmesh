# Integration contract (v1)

A versioned contract for tools that migrate, automate, discover, deploy,
analyze, or moderate repositories without scraping a UI: HTTP resources, a
`forkmesh-event-v1` event stream, command-line output, and signed webhooks.
[`conformance/integrations/v1/contract.json`](../../conformance/integrations/v1/contract.json)
is the source of truth; if this page disagrees, the JSON wins. The client,
contract runner, watcher, webhook sender, and offline fixture are one
standard-library file,
[`tools/forkmesh_integration.py`](../../tools/forkmesh_integration.py).

```sh
python3 tools/forkmesh_integration.py discover --target hosted
python3 tools/forkmesh_integration.py contract --target self-hosted --base-url http://127.0.0.1:8791
python3 tools/forkmesh_integration.py contract --target hosted --fixture next   # offline
```

`contract` prints a `forkmesh-integration-contract-report-v1` report (status
codes, fields, headers, attempts, latency, service version; no credentials).

## Services and authentication

| Service | Default base URL | What it is |
| --- | --- | --- |
| `hosted` | `https://app.forkmesh.com` | The hosted application API |
| `self-hosted` | `http://127.0.0.1:8791` | The `forkmesh-mirror-node` loopback control API |

Each resource names one scheme: `anonymous` (public, may be edge cached),
`loopback-operator` (socket access is the authorization; never publish the
port), `catalog-view-token` (owner-signed `forkmesh-catalog-view-v1` `viewer`,
`ts`, `sig`; adds private repositories, `no-store` with a weak `ETag`),
`account-bearer` (`Authorization: Bearer …`, never in a URL),
`webhook-capability-url` (the 256-bit token in the URL is the credential), and
`webhook-hmac` (see [Signatures](#signatures)).

## HTTP resources

| Service | Method and path | Required fields |
| --- | --- | --- |
| hosted | `GET /api/version` | `ok`, `rev`, `worker`, `version`, `now` |
| hosted | `GET /api/openapi.json` | `openapi`, `info`, `paths` |
| hosted | `GET /api/metrics/endpoints` | `ok`, `endpoints[].method`, `endpoints[].template` |
| hosted | `GET /api/repositories` | `ok`, `repositories[].owner`, `.name`, `.visibility` |
| hosted | `GET /api/repositories/search` (beta) | `ok`, `total`, `results[].owner`, `.name`, `.clone` |
| hosted | `GET /api/status` | `ok`, `now`, `systems[].id`, `systems[].status` |
| hosted | `GET /api/mirrors/https` | `ok`, `protocol`, `registration` |
| hosted | `POST /api/chat/webhooks/{token}` | inbound webhook, `202 {"ok", "messageId"}` |
| self-hosted | `GET /healthz` | `ok`, `node` |
| self-hosted | `GET /readyz` (200 or 503) | `ok`, `ready`, `version` |
| self-hosted | `GET /v1/status` | `ok`, `ready`, `node`, `version`, `uptimeSeconds`, `repositories`, `lastSyncOk`, `syncCount` |
| self-hosted | `POST /v1/control/sync` | `202 {"ok", "queued"}` |

Only these routes are stable; others discovered via OpenAPI may change.

## Pagination

Collections are bounded: `GET /api/repositories` returns at most 200 records,
newest `updatedAt` first. Time feeds resume with `since` = previous `latestTs`
(deduplicate the boundary). `GET /api/repositories/search` is keyset-paged:
pass `nextCursor` back as `cursor` until it is `null`. See
[Searchable repository index](repository-index.md).

## Filtering

Only search filters server-side (`q`, `view`, `language`, `topic`, `owner`,
`cloneable`). Otherwise filter client-side on documented fields.

## Idempotency

`GET`/`HEAD` are safe to repeat. `POST /v1/control/sync` is idempotent
(`"queued": false` while pending). Inbound chat webhook POSTs are not
deduplicated. Every outbound event has a stable `id` to use as the idempotency
key.

## Ordering and the event stream

```json
{"schema":"forkmesh-event-v1","id":"4c1f…","type":"repository.changed",
 "subject":"owner/name","sequence":7,"digest":"9a0e…","occurredAt":1789347378245,
 "source":"https://app.forkmesh.com","data":{"owner":"owner","name":"name"}}
```

| Type | Service | Subject | Producer |
| --- | --- | --- | --- |
| `repository.changed`, `repository.removed` | hosted | lowercase `owner/name` | `watch` |
| `repository.push`, `branch.push` | hosted | lowercase `owner/name` | relay |
| `pull.merged`, `release.published` | hosted | lowercase `owner/name` | relay |
| `mirror.node.changed` | self-hosted | `node:<name>` | `watch` |
| `mirror.repository.changed`, `mirror.repository.removed` | self-hosted | the daemon's repository key | `watch` |

Delivery is at-least-once, `sequence` increases by one per subject (no global
order), and every `changed` event carries full current `data`. `EventLedger`
in the tool applies the consumer rules: ignore seen ids and stale sequences,
buffer gaps, emit `resync` after `gapTimeoutSeconds` (120), and accept the
first event per subject as the baseline.

## Webhooks

`deliver` posts events as canonical JSON with these headers: `ForkMesh-Webhook-Id`
(event id), `ForkMesh-Webhook-Timestamp` (Unix seconds per attempt),
`ForkMesh-Webhook-Signature` (`v1=<hex>`, comma-separated during rotation),
`ForkMesh-Webhook-Attempt`, `ForkMesh-Event-Type`. `2xx` acknowledges, `410`
stops, retryable statuses retry, anything else goes to the dead-letter file
(replay with `deliver --input dead.ndjson`).

### Signatures

```text
signature = "v1=" + hex(HMAC-SHA256(secret, "forkmesh-webhook-v1\n" + timestamp + "\n" + id + "\n" + raw_body))
```

```python
msg = b"forkmesh-webhook-v1\n%d\n%s\n" % (ts, headers["ForkMesh-Webhook-Id"].encode()) + body
```

Reject timestamps more than 300 seconds off and deduplicate by id.

### Repository webhooks

Owners register public `https` endpoints in repository **Settings**
(`POST /api/repo/<owner>/<repo>/webhooks`); the relay generates the secret and
shows it once. Events: `repository.push` (any ref moved), `branch.push`
(default branch moved), `pull.merged`, `release.published`. Only public
repositories emit them; deliveries are queued out of band and stop after 6
attempts. `sequence` is per endpoint.

### Inbound chat webhooks

`POST /api/chat/webhooks/{token}` with `{"text": "…"}` posts to a public
channel (`202`). Errors: 400 `empty_webhook`, 404 `not_found`, 405
`method_not_allowed`, 409 `public_channel_required`.

## Retries

Retry idempotent requests on transport errors and `408`, `425`, `429`, `500`,
`502`, `503`, `504`: at most 6 attempts, exponential backoff from 0.5 s capped
at 30 s with jitter; `Retry-After` or `retryAfterMs` overrides. Non-idempotent
requests retry only on `429`.

## Rate limits

Throttled requests answer `429` with `Retry-After`. Hosted public reads are
edge cached (catalog 30 s). `watch` polls no faster than 30 s hosted, 5 s
self-hosted.

## Errors

Hosted errors are `{"error":"<snake_case>"}`; the daemon answers
`{"ok":false,"error":"<message>"}`. Branch on status class, then code. New
codes are additive.

## Command-line output

Each stdout line is one JSON object (`"schema":"forkmesh-cli-v1"`, or events
for `watch`); diagnostics go to stderr. Commands: `validate`, `discover`,
`contract`, `watch`, `deliver`, `fixture`.

| Exit code | Meaning |
| --- | --- |
| 0 | success |
| 1 | contract failure or undeliverable events |
| 2 | usage or configuration error |
| 3 | unsupported service version skew |

`forkmesh-mirror-node` flags: `-config`, `-init`, `-check`, `-public-key`,
`-set-version`, `-register-link-code`, `-version`; exit `0` or `1`.

## Deprecation

Deprecated entries are marked `"stability": "deprecated"` and listed with
`announced`, `sunset` (at least 180 days later), and `replacement`. Removal
needs contract v2.

## Version skew

Within v1 services only add; consumers must ignore unknown fields. A version
below `serviceMinimum` is `unsupported` (exit 3); unparseable versions are
`unknown`. The fixture serves `previous`, `current`, `next`, `broken`, and
`unsupported` profiles.

## Examples

Examples in `conformance/integrations/v1/examples/` use only this contract:
`conformance/integrations/v1/examples/discovery_catalog.py`,
`conformance/integrations/v1/examples/migration_export.py`,
`conformance/integrations/v1/examples/webhook_moderation.py`,
`conformance/integrations/v1/examples/deployment_gate.py`.

## Contract tests and telemetry

`app/tests/test_integration_contract.py` checks every entry against the
serving source, runs the contract against the fixture for both services and
every profile, and runs every example. Each report's `telemetry` block records
requests, retries, transport errors, status classes, and latency.
