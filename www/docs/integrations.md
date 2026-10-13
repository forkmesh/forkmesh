# Integration contract (v1)

ForkMesh publishes a versioned contract for tools that migrate, automate,
discover, deploy, analyze, or moderate repositories without scraping the
website or desktop UI. The contract has four parts:

- **HTTP resources** on the hosted API and on a self-hosted mirror node.
- **An event stream** of `forkmesh-event-v1` records.
- **Command-line output**, both the integration tool's and `forkmesh-mirror-node`'s.
- **Signed webhook delivery**.

| Artifact | Location |
| --- | --- |
| Machine-readable contract | [`conformance/integrations/v1/contract.json`](../../conformance/integrations/v1/contract.json) |
| SDK-light client, contract runner, watcher, webhook sender, offline fixture | [`tools/forkmesh_integration.py`](../../tools/forkmesh_integration.py) (one file, Python 3.10+ standard library) |
| Maintained examples | [`conformance/integrations/v1/examples/`](../../conformance/integrations/v1/examples/) |
| Contract tests | [`app/tests/test_integration_contract.py`](../../app/tests/test_integration_contract.py) |

The file `contract.json` is the source of truth. This page explains it. If the two
disagree, the JSON wins and the disagreement is a bug.

## Quick start

Download `forkmesh_integration.py` and `contract.json` into one directory.
The tool finds `contract.json` next to itself. Then run:

```sh
python3 forkmesh_integration.py discover --target hosted
python3 forkmesh_integration.py contract --target hosted --output report.json
python3 forkmesh_integration.py contract --target self-hosted --base-url http://127.0.0.1:8791
python3 forkmesh_integration.py contract --target hosted --fixture next   # offline, no network
```

`discover` prints one line with a capability map and a version-skew verdict.
`contract` checks every resource for its target and prints a
`forkmesh-integration-contract-report-v1` report. The report records the
status codes, required fields, headers, attempts, latency, additional
(unknown) field counts, and the service version. Reports hold no credentials:
base URLs are stripped of userinfo and query strings.

## Services and authentication

| Service | Default base URL | What it is |
| --- | --- | --- |
| `hosted` | `https://app.forkmesh.com` | The hosted application API |
| `self-hosted` | `http://127.0.0.1:8791` | The `forkmesh-mirror-node` daemon's loopback control API |

Every HTTP resource names exactly one authentication scheme:

- `anonymous`: public reads. No credentials; responses may be edge cached.
- `loopback-operator`: the daemon binds `127.0.0.1` only, so reaching the
  socket is the authorization. Never publish that port.
- `catalog-view-token`: an owner signs a short-lived
  `forkmesh-catalog-view-v1` token and passes `viewer`, `ts`, and `sig`. This
  adds the viewer's private and shared repositories to `GET /api/repositories`.
  Those responses are `no-store` and carry a weak `ETag`, so revalidate with
  `If-None-Match`.
- `account-bearer`: a bearer token from `POST /api/accounts/login`, sent as
  `Authorization: Bearer …`. Never put it in a URL.
- `webhook-capability-url`: an inbound chat webhook URL contains a random
  256-bit token, and the URL itself is the credential. Rotate the webhook to
  revoke it.
- `webhook-hmac`: outbound deliveries are signed with a shared secret (see
  [Signatures](#signatures)).

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

Use `GET /api/openapi.json` or `GET /api/metrics/endpoints` for discovery.
They enumerate every route the Worker serves. Only the routes above are part
of the stable contract. Other routes may change without notice.

## Pagination

Contract collections are bounded rather than paged. `GET /api/repositories`
returns at most 200 records, newest `updatedAt` first. Feeds ordered by time
resume with a `since` cursor set to the previous response's `latestTs`.
Entries exactly at that boundary can repeat, so deduplicate them.

`GET /api/repositories/search` is the one keyset-paged collection. Pass
its `nextCursor` back as `cursor` with the same filters until `nextCursor` is
`null`; each match is returned exactly once. See
[Searchable repository index](repository-index.md).

## Filtering

Contract collections have no server-side filters, except
`GET /api/repositories/search` (`q`, `view`, `language`, `topic`, `owner`,
`cloneable`). Otherwise filter on the client using
documented fields: `owner`, `name`, `visibility`, `liveHost`, `cloneOnline`,
`termsFlagged`. Query parameters not listed in the contract are not part of
it.

## Idempotency

- `GET` and `HEAD` are always safe to repeat.
- `POST /v1/control/sync` is idempotent. Repeating it while a sync is pending
  returns `202` with `"queued": false`.
- Inbound chat webhook POSTs are **not** deduplicated. Every accepted POST
  becomes a message, so a sender should retry only when no response arrived.
- Every outbound event has a stable `id`. Receivers use it as the idempotency
  key and answer `2xx` for duplicates.

## Ordering and the event stream

`watch` polls stable resources and prints one `forkmesh-event-v1` JSON object
per change:

```json
{"schema":"forkmesh-event-v1","id":"4c1f…","type":"repository.changed",
 "subject":"forkmesh/forkmesh","sequence":7,"digest":"9a0e…",
 "occurredAt":1789347378245,"source":"https://app.forkmesh.com",
 "data":{"owner":"forkmesh","name":"forkmesh","commit":"…","termsFlagged":false}}
```

| Type | Service | Subject | Producer |
| --- | --- | --- | --- |
| `repository.changed`, `repository.removed` | hosted | lowercase `owner/name` | `watch` |
| `repository.push`, `branch.push` | hosted | lowercase `owner/name` | relay |
| `pull.merged`, `release.published` | hosted | lowercase `owner/name` | relay |
| `mirror.node.changed` | self-hosted | `node:<name>` | `watch` |
| `mirror.repository.changed`, `mirror.repository.removed` | self-hosted | the daemon's repository key | `watch` |

The `watch` types are produced by polling the public catalog. The four relay
types are produced by the relay itself and are only ever delivered to a
[registered repository webhook](#repository-webhooks).

```sh
python3 forkmesh_integration.py watch --target hosted --state watch-state.json --interval 60
```

Guarantees:

- **At-least-once.** State is saved *after* events are printed. A crash in
  between re-emits the same ids on restart; nothing is lost.
- **Per-subject order.** `sequence` increases by exactly one for each observed
  change to a subject. There is no global order across subjects.
- **Full state.** Every `changed` event carries the subject's complete current
  `data`. A consumer that gives up on a gap still converges.
- Volatile telemetry (CPU, memory, disk) is excluded from `data`, so idle
  repositories stay quiet.

### Recovering from duplicate, delayed, missing, and out-of-order delivery

`EventLedger` (in the tool) implements the consumer rules. Use it as it is,
or port these five rules:

1. **Duplicate**: an already-seen `id` is acknowledged and ignored.
2. **Out of order (stale)**: a `sequence` at or below the last applied one for
   that subject is ignored.
3. **Delayed**: a `sequence` more than one ahead is buffered until the gap
   fills. It is then applied in order.
4. **Missing**: once a gap has waited `gapTimeoutSeconds` (120), the ledger
   emits `resync` with the missing range. It then applies the lowest buffered
   event. Re-read the resource over HTTP if the skipped transitions matter.
5. **Baseline**: the first event seen for a subject is applied whatever its
   sequence. A late-joining consumer does not wait for history.

```python
import forkmesh_integration as fm
ledger = fm.EventLedger()
for action, payload in ledger.accept(event) + ledger.expire():
    if action == "apply":
        handle(payload["subject"], payload.get("data"))
    elif action == "resync":
        refetch(payload["subject"])
```

The relay's owner WebSocket (`{"type":"event","topic","repo","state","generation"}`)
is an internal, owner-signed doorbell for an account's own nodes. It is not a
third-party interface.

## Webhooks

### Outbound (`forkmesh-webhook-v1`)

```sh
export FORKMESH_WEBHOOK_SECRET=…            # never on the command line
python3 forkmesh_integration.py watch --target hosted --state s.json \
  | python3 forkmesh_integration.py deliver --url https://hooks.example/forkmesh \
      --dead-letter dead.ndjson
```

Each delivery is a `POST` whose body is one event serialized as canonical JSON
(sorted keys, no spaces). It carries these headers:

| Header | Value |
| --- | --- |
| `ForkMesh-Webhook-Id` | the event `id` (idempotency key) |
| `ForkMesh-Webhook-Timestamp` | Unix seconds, taken again on every attempt |
| `ForkMesh-Webhook-Signature` | `v1=<hex>`; during rotation, several comma-separated values |
| `ForkMesh-Webhook-Attempt` | `1`, `2`, … |
| `ForkMesh-Event-Type` | the event `type` |

The receiver's response decides what happens next:

- `2xx` acknowledges the event.
- `410 Gone` stops delivery for that event.
- `408`, `425`, `429`, `5xx`, and network errors are retried (see [Retries](#retries)).
- Any other `4xx`, or running out of retries, writes the event to the
  dead-letter file.

To replay, run `deliver --input dead.ndjson --dead-letter dead.ndjson`. The
file is rewritten to hold only the events that still fail.

### Signatures

The signed message and signature are:

```text
message   = "forkmesh-webhook-v1\n" + timestamp + "\n" + id + "\n" + raw_body
signature = "v1=" + hex(HMAC-SHA256(secret, message))
```

A receiver can verify a delivery without the tool:

```python
import hashlib, hmac, time

def verify(secret: bytes, headers, body: bytes, tolerance=300):
    ts = int(headers["ForkMesh-Webhook-Timestamp"])
    if abs(time.time() - ts) > tolerance:
        raise ValueError("stale")
    msg = b"forkmesh-webhook-v1\n%d\n%s\n" % (ts, headers["ForkMesh-Webhook-Id"].encode()) + body
    expected = "v1=" + hmac.new(secret, msg, hashlib.sha256).hexdigest()
    if not any(hmac.compare_digest(expected, part.strip())
               for part in headers["ForkMesh-Webhook-Signature"].split(",")):
        raise ValueError("bad signature")
```

Reject timestamps more than 300 seconds from now. Deduplicate by
`ForkMesh-Webhook-Id` so a captured request cannot be replayed within that
window.

Repository, issue, and pull records keep their own Ed25519 signatures over
`forkmesh-<kind>-v1` canonical strings. Node-to-relay requests carry
`X-ForkMesh-Signature`. Neither involves the webhook secret.

### Repository webhooks

A repository owner registers endpoints on the repository's **Settings** tab in
the dashboard (`POST /api/repo/<owner>/<repo>/webhooks`, session-authenticated,
owner only). ForkMesh then delivers the events below with the same
`forkmesh-webhook-v1` envelope the `deliver` command uses — no watcher process
to run, and the signing secret is generated by the relay and shown once.

| Event | Fires when |
| --- | --- |
| `repository.push` | Any ref (branch or tag) moved: an owner-signed catalog publication, or a plain-git push to a repository ForkMesh hosts. |
| `branch.push` | A signed publication also moved the default branch tip. A hosted repository's plain-git push does not raise this, because that path pins a refs digest without a branch tip. |
| `pull.merged` | A merge completed *and* the node reported the result published. |
| `release.published` | A signed release manifest was published for a tag. |

Notes that matter when wiring a CI provider:

- A push produces `repository.push`, and additionally `branch.push` when the
  default branch moved, so a provider that should build only on the default
  branch subscribes to `branch.push` alone.
- Only public repositories emit these events.
- Destinations must be public `https`. Loopback, link-local, private-range and
  `forkmesh.com` addresses are refused when the webhook is saved.
- Deliveries are queued and drained out of band, so a slow endpoint never slows
  a push, a merge or a release down. Retries follow [Retries](#retries) and stop
  after 6 attempts; the Settings tab shows the last outcome per endpoint.
- `sequence` counts that endpoint's own deliveries and increases by exactly
  one per event it receives, so the consumer rules above apply unchanged. Two
  endpoints subscribed to the same event agree on its `id` but keep their own
  sequence, and a delivery test consumes one.

### Inbound chat webhooks

`POST /api/chat/webhooks/{token}` with a body of `{"text": "…"}` (`content`
or `message` also work) posts to a public channel and answers `202`. Errors:

| Status | `error` |
| --- | --- |
| 400 | `empty_webhook` |
| 404 | `not_found` |
| 405 | `method_not_allowed` (with `Allow: POST`) |
| 409 | `public_channel_required` |

## Retries

Retry idempotent requests on transport errors and on `408`, `425`, `429`,
`500`, `502`, `503`, and `504`:

- Make at most 6 attempts, with exponential backoff starting at 0.5 s, capped
  at 30 s, with jitter.
- `Retry-After` (seconds or an HTTP date) or a `retryAfterMs` body field
  replaces the computed delay.
- Non-idempotent requests retry only on `429`.
- Never retry any other `4xx`.

`Client` in the tool implements this and counts retries in its telemetry.

## Rate limits

Throttled requests answer `429` with `Retry-After` and usually
`{"error":"rate_limited","retryAfterMs":…}`. Hosted public reads are edge
cached (the catalog for 30 s). Polling faster returns the same bytes and
spends the shared daily request budget. `watch` enforces a minimum poll
interval of 30 s for hosted and 5 s for self-hosted.

## Errors

- Hosted errors are JSON objects with a stable snake_case code, for example
  `{"error":"invalid_json"}`.
- The daemon answers `{"ok":false,"error":"<message>"}`.
- Branch on the HTTP status class first and the `error` code second. Treat an
  unknown code by its status class.
- New error codes are additive and may appear at any time.

## Command-line output

Every line written to stdout is one JSON object:

- Command results carry `"schema":"forkmesh-cli-v1"` and `"command"`.
- `watch` lines are `forkmesh-event-v1` events.
- Diagnostics go to stderr.

| Exit code | Meaning |
| --- | --- |
| 0 | success |
| 1 | contract failure or undeliverable events |
| 2 | usage or configuration error |
| 3 | unsupported service version skew |

Commands: `validate`, `discover`, `contract`, `watch`, `deliver`, `fixture`.

`forkmesh-mirror-node` has a smaller contract: the flags `-config`, `-init`,
`-check`, `-public-key`, `-set-version`, `-register-link-code`, and
`-version`. It exits `0` on success and `1` on failure, with the reason on
stderr. On success it prints these exact lines:

| Flag | stdout |
| --- | --- |
| `-check` | `mirror-node configuration OK` |
| `-set-version` | `mirror-node version updated` |
| `-register-link-code` | `node account registered` |

## Deprecation

To deprecate a contract entry:

1. Mark it `"stability": "deprecated"`.
2. List it under `deprecations` with `announced`, `sunset`, and `replacement`.
   The sunset must be at least **180 days** after the announcement.

A deprecated entry keeps working until its sunset, and contract reports warn
while it is in use. Removal requires the next contract major (v2). No entries
are deprecated in v1.0.0.

## Version skew

- Within v1, services only add things: fields, resources, event types, and
  error codes. Consumers **must ignore unknown fields**.
- A service reporting a version below `serviceMinimum` (`0.8.0`) is
  `unsupported`, and the tools exit with code 3.
- A version that cannot be parsed, such as a `dev` build, is `unknown`. It is
  reported, not failed.
- The fixture serves three skew profiles: `previous` (0.8.0, without
  `deployFingerprint`), `current`, and `next` (0.9.0, with extra fields). The
  contract tests run every profile against both services. The `broken` and
  `unsupported` profiles prove that failures are detected.

## Examples

Each example uses only the resources and rules on this page, plus
`forkmesh_integration.py` placed next to it (or pointed to by
`FORKMESH_INTEGRATION_TOOL`). The contract tests run all of them against the
offline fixture.

| Category | Example | Run |
| --- | --- | --- |
| Discovery, analytics | [`discovery_catalog.py`](../../conformance/integrations/v1/examples/discovery_catalog.py) | `python3 discovery_catalog.py --query mesh --live-only` |
| Migration | [`migration_export.py`](../../conformance/integrations/v1/examples/migration_export.py) | `python3 migration_export.py --output ./export` (rerun to resume) |
| Automation, moderation | [`webhook_moderation.py`](../../conformance/integrations/v1/examples/webhook_moderation.py) | `FORKMESH_WEBHOOK_SECRET=… python3 webhook_moderation.py --port 8787 --state ledger.json` |
| Deployment | [`deployment_gate.py`](../../conformance/integrations/v1/examples/deployment_gate.py) | `python3 deployment_gate.py --expect-version 0.8.8 --timeout 300` |

With `--stop-after`, `webhook_moderation.py` stops only after it has written
the last acknowledgement. If it stopped first, the server's handler threads
(which are daemon threads) could die with the process between the response
headers and the body. The sender would then get `IncompleteRead` on its final
delivery.

## Contract tests and telemetry

- `app/tests/test_integration_contract.py` checks each contract entry against
  the source that serves it: Worker routes and response fields, daemon routes
  and status JSON tags, and `forkmesh-mirror-node` flags and output. It then
  runs the contract against the fixture for both services and every skew
  profile, checks retry, ledger, and webhook recovery properties, and runs
  every example.
- CI runs the same contract offline for both services and every profile.
  Against production, run
  `contract --target hosted` (and `--target self-hosted` on a node).
- Each report's `telemetry` block records requests, retries, transport errors,
  status classes, and total latency, which is enough to track contract pass
  rate, API errors, and upgrade breakage per service version.
- Hosted per-route traffic and error rates are visible live in the desktop's
  Network → Web Requests tab, counted from the Worker log tail (they are not
  stored server-side).
