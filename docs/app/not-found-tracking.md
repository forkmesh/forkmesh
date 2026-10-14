# 404 tracking and the admin 404s list

Every 404 on a ForkMesh site is counted in one grouped list in the admin
console (Reliability → "404s", `?view=not-found`). When a group reaches 10
hits, administrators get one Ping.

## Code map

| Code | Role |
| --- | --- |
| `app/src/not_found_log.py` | Recording, pruning, the alert, the `/api/not-found` relay endpoint, the admin page |
| `app/src/app_runtime.py` (`_fetch_inner`) | Calls `record_response` for every 404 |
| `app/src/admin_shell.py` | Sidebar entry and badge |
| `www/worker.js` (`reportNotFound`) | Relays the static site's own 404s to the App |
| Tables | `not_found_paths`, `not_found_hours`, `not_found_ignores` |

## Grouping

One `not_found_paths` row per (site, method, path):

- **site**: request host, lower-cased, `www.` removed; relayed hits always
  record under the host of `WWW_ORIGIN`.
- **method**: `HEAD` counts as `GET`; unusual verbs are `OTHER`.
- **path**: no query; repository routes pass through
  `_privacy_safe_log_path` so private names never appear; id-like or
  token-like segments become `[id]`; cut at 300 characters.
- **referrer**: host and path for ForkMesh sites, host only otherwise.
- **last visitor** (`last_client`): IP, country and User-Agent of the most
  recent hit, sealed with `encrypt_row` so no plaintext IP is stored.

Each 404 is one upsert on the group and one on its hour in
`not_found_hours`, which backs a 24-hour chart and per-group sparklines.

## Exceptions

An exception (`not_found_ignores`, per site and path, optionally `prefix`)
stops a path from being logged, for content removed on purpose. A matching
request only bumps the rule's `hits` and `last_seen`. A prefix of `/` is
refused. Rules are cached per process for a minute; at most 500. Adding and
removing are CSRF-checked and audited.

## Alert

When a hit takes an unalerted group to 10, the row is claimed with
`UPDATE … SET alerted_at=? WHERE … AND alerted_at=0` (sent once), and each
administrator gets an `operational_alert` Ping "404 reached 10 hits: <path>".
At most 5 groups alert per hour; held-back groups stay "Pending".

## Admin page

KPIs, the 24-hour chart, and groups sortable by hits, recent activity or
recency (50 per page), each with source label, sparkline, referrer, last
visitor, alert state, and Ignore / Clear actions (Clear is audited).

## Bounds

Groups unseen for 90 days are deleted; at most 5000 groups; pruning runs on
the first and every 50th new group per process; hourly rows older than 48
hours are deleted; read-only mode records nothing.

## Static-site relay

When the static site's asset lookup answers 404 for GET or HEAD,
`reportNotFound` posts `{path, referrer, userAgent, fetchSite}` to
`POST /api/not-found` on `APP_ORIGIN` in the background. The endpoint is
unauthenticated by design: it can only record a 404 for a site path. The
visitor's IP arrives through the signed edge-client header.

## Auto bans

Every 404 is also passed to `probe_ban.observe`; see
[probe-auto-ban.md](probe-auto-ban.md).

## Tests

`app/tests/test_not_found_log.py`, `www/tests/not_found_relay.test.mjs`,
`app/tests/test_admin_shell.py`.
