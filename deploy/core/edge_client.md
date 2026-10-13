# `x-forkmesh-edge-client`: the visitor identity the homepage Worker vouches for

On Cloudflare the App Worker saw every visitor's address in `cf-connecting-ip`
and country in `cf-ipcountry` (or `request.cf.country`). On Next the App runs
on the core nodes behind Caddy, and requests reach it two ways:

1. **Directly** (browser → `app.next.forkmesh.com` / `world.next.forkmesh.com`
   → Caddy on a core node). Caddy drops every incoming `Cf-*` header and sets
   `Cf-Connecting-Ip` to the TCP peer (`{remote_host}`), so the App's existing
   `cf-connecting-ip` reads give the real visitor. No country header is set
   (the DB-IP Lite lookup from PLAN.md Phase 4 is not built yet).
2. **Relayed by the homepage Worker** `forkmesh-www` (browser →
   `forkmesh.com` → Worker → `fetch(APP_ORIGIN + path)`). Here the TCP
   peer, and so `Cf-Connecting-Ip` as Caddy sets it, is a **Cloudflare egress
   address**, not the visitor. The Worker therefore sends the visitor in one
   signed header, and forwards no unsigned address or country header at all.

## Wire format

```
x-forkmesh-edge-client: v1,<ip>,<country>,<ts>,<mac>
```

| field     | value |
|-----------|-------|
| `v1`      | format version |
| `ip`      | the visitor address from Cloudflare's own `cf-connecting-ip` on the incoming request (IPv4 or IPv6; never contains `,` or whitespace) |
| `country` | ISO 3166 alpha-2 from `request.cf.country` (fallback `cf-ipcountry`), uppercase, may be empty |
| `ts`      | Unix seconds when the Worker signed |
| `mac`     | base64url (no padding) of HMAC-SHA256 over the message below |

```
message = "v1" "\n" ip "\n" country "\n" ts "\n" METHOD "\n" pathname
key     = WWW_EDGE_SECRET (UTF-8 bytes)
```

`METHOD` is the upper-case method of the request the Worker sends to the App
and `pathname` its URL path without the query string (for the attribution
relay: `POST` and `/api/attribution/capture`). Binding method and path means a
captured header cannot be replayed onto a different endpoint.

When `WWW_EDGE_SECRET` is unset the Worker sends no identity at all (the App
then sees only Caddy's `Cf-Connecting-Ip`, the Cloudflare egress address).
The Worker also deletes any `x-forkmesh-edge-client` a visitor sent.

The header is produced in `www/worker.js` (`edgeClientValue`, `appHeaders`)
and covered by `www/tests/app_origin.test.mjs` and
`www/tests/attribution_capture.test.mjs`.

## `x-forkmesh-edge-where`: place and network, for the log only (2026-09-28)

Alongside a signed `x-forkmesh-edge-client`, and only then, the Worker sends

```
x-forkmesh-edge-where: city=Ashburn&region=Virginia&asn=7922&org=Comcast+Cable
```

This is `request.cf.city`, `.region`, `.asn` and `.asOrganization`, URL-encoded
(`URLSearchParams`), control characters removed, each cut at 80 characters.
Empty fields are left out. It is **not signed** and grants nothing. Caddy's
access log records it (with the `ip` and `country` fields of
`x-forkmesh-edge-client`) only when the TCP peer is in Cloudflare's ranges,
and `client_headers` then removes it, so the App never sees it. The Worker
deletes any copy a visitor sent (`CLIENT_CLAIM_HEADERS`). The desktop shows it
on the access line (`docs/NODE_LOGS.md`, "Who the visitor really is").

## What the App must do (verification contract)

The App trusts the header **only** when all of these hold, and otherwise
ignores it entirely (never partially):

1. The value has exactly five comma-separated fields and the first is `v1`.
2. `ts` is an integer within **±300 s** of the App's clock.
3. `mac` equals the HMAC-SHA256 recomputed with the App's `WWW_EDGE_SECRET`
   credential over the message built from the header fields **and the
   request's own method and URL path**, compared in constant time
   (`hmac.compare_digest`).
4. `ip` parses as an IP address; `country` is empty or two letters A–Z.

When valid, the App uses `ip` in place of `cf-connecting-ip` and `country` in
place of `cf-ipcountry` / `request.cf.country` for that request (rate limits,
abuse checks, attribution skip-countries, audit logs). When invalid it keeps
the Caddy-provided `Cf-Connecting-Ip` (the Cloudflare egress address): a
forged header can at worst make a request look like it came from Cloudflare,
never from a chosen address.

A replay within the 300 s window re-asserts the same visitor for the same
method and path, which gains an attacker nothing they could not do by sending
the request themselves.

Secret handling: `WWW_EDGE_SECRET` is a Worker secret on `forkmesh-www`
(`wrangler secret put WWW_EDGE_SECRET --config wrangler.production.toml`) and the same value is the systemd
credential `WWW_EDGE_SECRET` of `forkmesh-app.service` on every core node
(from `~/projects/forkmesh/config/fleet/secrets.env`, pushed by
`tools/next_fleet.py secrets nN`). Rotate by updating both, core first: during
the gap the App simply ignores the old signatures.

## Where the App verifies it

`app/host/forkmesh_host/asgi.py` (`HostASGI._edge_client`) with the checks in
`app/host/forkmesh_host/request_guard.py` (`EdgeClientVerifier`), before the
Worker code sees the request:

- The header is **always removed** from the request the Worker sees.
- It is considered only when the TCP peer is loopback, that is, the request
  came through Caddy (the app listens on 127.0.0.1 only).
- Checks 1-4 above, with `WWW_EDGE_SECRET` from the unsealed app's env. The
  path is the request's raw path without the query.
- **Single use:** a MAC already accepted within the 300 s window is refused
  (the per-process cache holds up to 20 000 recent MACs), so a captured header
  cannot be replayed onto the same node. Each core has its own cache; a replay
  onto a different core within the window is still accepted, which at worst
  re-asserts the same visitor for the same method and path.
- When valid, `cf-connecting-ip` and `cf-ipcountry` are overwritten (an empty
  country removes `cf-ipcountry`). Otherwise Caddy's `Cf-Connecting-Ip`, the
  TCP peer, stays.

Tests: `app/tests/host/test_host_request_guard.py`, and
`app/tests/host/test_edge_client_crosscheck.py`, which has `www/worker.js`
sign three requests (the `cross-check vectors` case in
`www/tests/app_origin.test.mjs`) and checks that `EdgeClientVerifier`
accepts each header once and refuses it for another method, path or secret.
