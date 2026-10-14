# `x-forkmesh-edge-client`: the visitor identity the homepage Worker vouches for

The App runs on the core nodes behind Caddy and is reached two ways:

1. **Directly** (browser → Caddy on a core node). Caddy drops every incoming
   `Cf-*` header and sets `Cf-Connecting-Ip` to the TCP peer
   (`{remote_host}`), so the App's `cf-connecting-ip` reads give the real
   visitor. No country header is set.
2. **Relayed by the homepage Worker** `forkmesh-www` (browser → `forkmesh.com`
   → Worker → `fetch(APP_ORIGIN + path)`). The TCP peer is a Cloudflare egress
   address, so the Worker sends the visitor in one signed header and forwards
   no unsigned address or country header.

## Wire format

```
x-forkmesh-edge-client: v1,<ip>,<country>,<ts>,<mac>
```

| field     | value |
|-----------|-------|
| `v1`      | format version |
| `ip`      | visitor address from the incoming `cf-connecting-ip` (IPv4 or IPv6) |
| `country` | ISO 3166 alpha-2 from `request.cf.country` (fallback `cf-ipcountry`), uppercase, may be empty |
| `ts`      | Unix seconds when the Worker signed |
| `mac`     | base64url (no padding) HMAC-SHA256 over the message below |

```
message = "v1" "\n" ip "\n" country "\n" ts "\n" METHOD "\n" pathname
key     = WWW_EDGE_SECRET (UTF-8 bytes)
```

`METHOD` and `pathname` (no query string) are those of the request the Worker
sends to the App, so a captured header cannot be replayed onto another
endpoint. Without `WWW_EDGE_SECRET` the Worker sends no identity. The Worker
deletes any `x-forkmesh-edge-client` a visitor sent. The header is produced in
`www/worker.js` (`edgeClientValue`, `appHeaders`) and tested in
`www/tests/app_origin.test.mjs` and `www/tests/attribution_capture.test.mjs`.

## `x-forkmesh-edge-where`: place and network, for the log only

Only alongside a signed `x-forkmesh-edge-client`, the Worker sends

```
x-forkmesh-edge-where: city=<city>&region=<region>&asn=<asn>&org=<organization>
```

built from `request.cf.city`, `.region`, `.asn` and `.asOrganization`,
URL-encoded, control characters removed, each field cut at 80 characters, empty
fields omitted. It is unsigned and grants nothing. Caddy's access log records
it only when the TCP peer is in Cloudflare's ranges, then `client_headers`
removes it so the App never sees it. The Worker deletes any visitor-sent copy
(`CLIENT_CLAIM_HEADERS`). See `docs/NODE_LOGS.md`.

## Verification contract

The App trusts the header only when all of these hold, and otherwise ignores
it entirely:

1. Exactly five comma-separated fields, the first `v1`.
2. `ts` is an integer within **±300 s** of the App's clock.
3. `mac` equals the HMAC-SHA256 recomputed with the App's `WWW_EDGE_SECRET`
   over the header fields and the request's own method and URL path, compared
   in constant time (`hmac.compare_digest`).
4. `ip` parses as an IP address; `country` is empty or two letters A–Z.

When valid, `ip` replaces `cf-connecting-ip` and `country` replaces
`cf-ipcountry` for that request (rate limits, abuse checks, attribution,
audit logs). When invalid, the Caddy-provided address stays: a forged header
can at worst make a request look like it came from Cloudflare.

## Where the App verifies it

`app/host/forkmesh_host/asgi.py` (`HostASGI._edge_client`) with the checks in
`app/host/forkmesh_host/request_guard.py` (`EdgeClientVerifier`), before the
App code sees the request:

- The header is always removed from the request the App sees.
- It is considered only when the TCP peer is loopback (the request came
  through Caddy; the App listens on 127.0.0.1 only).
- **Single use:** a MAC already accepted within the window is refused
  (per-process cache of up to 20 000 recent MACs). Each core has its own
  cache, so a replay onto a different core within the window re-asserts the
  same visitor for the same method and path, which gains nothing.
- When valid, `cf-connecting-ip` and `cf-ipcountry` are overwritten (an empty
  country removes `cf-ipcountry`).

Tests: `app/tests/host/test_host_request_guard.py` and
`app/tests/host/test_edge_client_crosscheck.py`, which has `www/worker.js` sign
requests (the `cross-check vectors` case in `www/tests/app_origin.test.mjs`)
and checks that `EdgeClientVerifier` accepts each header once and refuses it
for another method, path or secret.

## Secret handling

`WWW_EDGE_SECRET` is a Worker secret on `forkmesh-www`
(`wrangler secret put WWW_EDGE_SECRET --config wrangler.production.toml`) and
the systemd credential `WWW_EDGE_SECRET` of `forkmesh-app.service` on every
core node (from `~/projects/forkmesh/config/fleet/secrets.env`, pushed by
`tools/next_fleet.py secrets nN`). Rotate by updating both, core first; during
the gap the App ignores old signatures.
