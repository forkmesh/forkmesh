# Cloudflare Free-tier request audit

In production only `forkmesh-www` (forkmesh.com) runs on Workers; the App and
World run on the core nodes.

Rules for keeping a Cloudflare Workers deployment of ForkMesh inside
Free-plan request, subrequest and CPU limits. They apply to the App, WWW and
World Workers and to the mirror routing path.

## Request architecture

| Worker | Entry point | Bindings |
| --- | --- | --- |
| App | `app/src/entry.py` (built to `app/src_build/entry.py`) | D1 `DB`, KV `REPOSITORY_METADATA`, Workers AI, Durable Objects, Static Assets, one-minute cron |
| WWW | `www/worker.js` | Static Assets, `APP` service binding |
| World | `world/worker.js` | Static Assets, `APP` service binding |

- Static pages, hashed assets and dashboard chunks are served by Static Assets
  and excluded from `run_worker_first`, so they cost no Worker invocation.
- WWW and World reach the App through service bindings, not public HTTP.
- Mirror hostnames are not Workers. Repository bytes come from the
  [direct-HTTPS mirror gateway](direct-https-mirror-gateway.md), reached
  directly or through a Tunnel.

## Rules

- **No idle polling.** Browser pages refresh on push events, reconnect,
  visibility resume or explicit refresh; any remaining interval is at least
  five minutes and pauses while the tab is hidden.
- **Clients go direct.** The desktop reads saved hosts' signed `/health` and
  `/v1/public/repositories/...` directly and pulls over the host's SSH remote;
  the relay is the final fallback.
- **One socket per node.** A managed mirror node holds one event socket and
  fetches a collaboration inbox only for the kind that changed.
- **No per-request database writes on read paths.** Mirror selection uses
  stateless rotation with a short clone pin, not a stored cursor.
- **Cache verified proofs.** Signed repository proofs are cached per edge
  location for 60 seconds, keyed by node, public key, owner, repository and
  accepted refs. Authenticated responses, writes, private replicas and Actions
  output are never cached.
- **Bounded failover.** At most four mirror candidates, tried sequentially,
  with bounded timeouts; errors become bounded 404/503 responses.
- **Batch cron.** Due cron phases are packed into as few invocations as their
  measured subrequest reserves allow; `/api/accounts/admin-cron-report` shows
  the cost per tick.
- **Measure before tightening.** Apply rate-limit or WAF rules only after
  sampled logs identify abusive paths, and exclude Git clients and crawlers.
  A plan upgrade is headroom, not a fix.
