# Cloudflare Free-tier request audit

Audit date: 2026-08-12. Production baseline supplied by the operator: about
55,000 Worker invocations, 108,000 subrequests, and 23,000 asset requests per
day, with 196 errors (178 uncaught) and CPU-limit failures.

## Request architecture

```text
Browser, desktop, Git client
  |
  +-- app.forkmesh.com API/dynamic page
  |     -> forkmesh-app Worker
  |          +-- D1 / KV / AI
  |          +-- room, node, note, world, cron and Discord Durable Objects
  |          `-- one selected healthy mirror (bounded failover, max 4)
  |                 -> mirrorXX.forkmesh.com
  |                      -> Cloudflare Tunnel
  |                           -> loopback Python mirror_gateway
  |                                -> local bare Git / release CAS
  |
  +-- forkmesh.com static page or asset
  |     -> Cloudflare Static Assets (no Worker for ordinary static paths)
  |
  +-- forkmesh.com API/protocol/profile
  |     -> forkmesh-www Worker
  |          -> APP service binding -> forkmesh-app Worker
  |
  +-- world.forkmesh.com shell/dynamic route
  |     -> forkmesh-world Worker -> Static Assets or APP service binding
  |
  +-- app hashed asset, status document, dashboard shell/chunk
  |     -> Cloudflare Static Assets (excluded from run_worker_first)
  |
  `-- desktop Mirrors / Pulls
        +-- saved active Hosts -> mirrorXX /health directly
        +-- public branch state -> mirrorXX /v1/public/repositories/... directly
        +-- Git pull ledger -> saved Host SSH remote directly
        `-- relay Git URL only as a final fallback

Managed mirror node
  +-- one EventSocket to /api/nodes/events
  +-- one signed inbox GET and exact DELETE per changed collaboration kind
  +-- native Go materialization into Git
  +-- state/catalog publication only when changed or its lease is due
  `-- no Qt/desktop process and no fallback inbox poll
```

### Worker entry points and bindings

| Worker | Entry point | Routes | Scheduled | Storage/services |
| --- | --- | --- | --- | --- |
| `forkmesh-app` | `app/src_build/entry.py` (generated from `app/src/entry.py`) | `app.forkmesh.com` custom domain | `* * * * *` | D1 `DB`, KV `REPOSITORY_METADATA`, Workers AI, eight Durable Object bindings, Static Assets |
| `forkmesh-www` | `www/worker.js` | `forkmesh.com`, `forkmesh.com` | none | Static Assets, `APP` service binding |
| `forkmesh-world` | `world/worker.js` | `world.forkmesh.com` | none | Static Assets, `APP` service binding |

There are no Queue or R2 bindings in the production Wrangler manifests. The
WWW-to-App and World-to-App hops use Service Bindings, not public HTTP. The App
owns the canonical redirect decision for every non-World route delegated by
World. App-owned World shell dependencies use the same binding, so the World
Static Assets upload does not duplicate the App's shared scripts, fonts,
favicons, images, or video.

The App cron packs each minute's due phases into stable batch Durable Objects.
Four phases are due every minute (`status-sample`, alert ping/email, and
`mirror-health`), with snapshot, compaction, installer, repository, and slower
projection phases staggered. Dispatching one object per phase cost a fixed
baseline of roughly 1,440 scheduled App invocations plus at least 5,760 phase
dispatches per day before staggered work; batching collapses the phase
dispatches into as few invocations as their measured subrequest reserves allow,
typically one or two a minute. `/api/accounts/admin-cron-report` reports the
observed cost per tick against the unbatched shape.

### Are mirror domains Workers?

No project configuration routes `mirror*.forkmesh.com` to a Worker. Live HEAD
requests on 2026-08-12 to mirror20, mirror21, and mirror23 returned
`server: cloudflare`, `cf-cache-status: DYNAMIC`, and no
`x-forkmesh-worker` header. Their `/health` bodies identify the
`direct-https` mirror gateway. This is consistent with a proxied Cloudflare
Tunnel terminating at the node, not another ForkMesh Worker invocation.

By contrast, `https://app.forkmesh.com/status` returned
`x-forkmesh-worker: app` and `cf-cache-status: HIT`. Mirror HTTP traffic still
uses Cloudflare's network and Tunnel, but it does not amplify into another
`forkmesh-app`, `forkmesh-www`, or `forkmesh-world` invocation.

## Phase 1 findings

### Highest invocation sources

1. Repeated browser polling: two `/network` polls per visible tab every minute,
   repository mirror fallback polls, and homepage online-count polling.
2. Desktop mirror/catalog/reachability reads routed through App even though the
   desktop already owns the active Host inventory.
3. Legacy Qt mirror publication and collaboration intake alongside the managed
   node, creating duplicate publication and idle inbox traffic.
4. WWW API requests: one WWW invocation plus one App service-binding
   invocation. This remains necessary for requests deliberately made against
   the marketing origin; App clients should use `app.forkmesh.com` directly.
5. The fixed one-minute cron and its Durable Object phases.

### Highest subrequest sources

1. A mirror-backed cache miss previously performed two D1 cursor operations,
   a signed `/health` proof fetch, the repository fetch, and a best-effort D1
   serve-counter write.
2. Mirror failover may try up to four endpoints. It is sequential and stops on
   the first valid response; there is no visitor-path broadcast fan-out.
3. The old mirror collaboration intake polled three inbox kinds every five
   seconds. Its theoretical idle load was 51,840 relay reads/day/node.
4. Deep cron probes read several repository objects. Their routing context and
   failed-endpoint quarantine are request-scoped and reused across the suite.
5. Catalog and health publication formerly came from both Qt and Go.

### Request amplification

| Path | Before | After |
| --- | --- | --- |
| App repository cache miss | 1 App invocation + 2 D1 cursor ops + proof fetch + mirror fetch + counter write; failover could repeat proof/fetch up to four times | 1 App invocation + stateless candidate rotation + edge-cached verified proof + normally one mirror fetch + counter write; failover remains bounded at four |
| API request to `forkmesh.com` | 1 WWW invocation + 1 App service-binding invocation | unchanged; this is an internal binding, not public HTTP |
| Desktop Mirrors tab | periodic App catalog/reachability calls plus node probes | zero App calls for saved Hosts; one signed health and one branches GET per active host per five-minute visible-tab refresh |
| Desktop Pulls | App/relay Git read first | saved Host SSH remotes first; App/relay only final fallback |
| Managed collaboration | three five-second inbox polls per node, plus Qt helper process | one socket; one GET + one exact DELETE only for the changed kind |
| Visible `/network` tab | 2 requests/minute = 2,880/day/tab | 2 requests/5 minutes = 576/day/tab, paused while hidden (80% reduction) |
| Open repository Mirrors tab | 288 fallback polls/day/tab | push/reconnect/manual refresh only (100% steady-state reduction) |
| Homepage online count | 288 interval reads/day/tab | initial read and visibility resume behind a five-minute floor |

### Cache and static-asset findings

- App's hashed assets, dashboard chunks, `/status/`, login/signup, network, and
  other listed documents bypass `forkmesh-app` through Static Assets.
- WWW ordinary static pages and immutable assets use Static Assets. Only API,
  protocol, feeds, health, MCP, and the two public-profile dashboard resources
  run the WWW Worker first.
- World static asset paths bypass its Worker; the World shell and redirect
  routes run first because they inject App origin or route product pages.
- Public deterministic status and repository metadata already use
  `caches.default`; repository metadata is content-addressed by attested refs
  and persisted in KV.
- Verified repository proofs now use a 60-second per-colo Cache API entry and a
  bounded isolate memo. The key binds node, public key, owner, repository, and
  accepted refs, so key rotation or state movement cannot reuse an old proof.
- Authenticated responses, writes, private replicas, Actions output, and
  collaboration mutations remain uncached.

### Retry, polling, CPU and error findings

- Mirror fetch timeouts are bounded (6 seconds for control, 20 seconds for
  repository/Git response headers); failover is capped at four.
- Native node socket reconnect uses bounded exponential backoff. Collaboration
  failures are requeued by kind; there is no recursive or idle fallback poll.
- The installer monitor incorrectly carried a request-scoped quarantine from
  a missing optional release CAS object into its source fallback. That made a
  healthy source build read report down. The fallback now starts a fresh
  candidate suite.
- The previous per-request D1 routing cursor was a CPU/network serialization
  hot path. Stateless random rotation removes one D1 read and one D1 write per
  uncached repository request while preserving Git's two-request sticky pin.
- Mirror/network exceptions are converted to bounded 404/503 responses. Native
  intake bounds response bodies, JSON parsing, Git commands, worktrees, and
  HTTP timeouts. No normal failure path intentionally raises past the request
  boundary.
- Historical `uncaught` and CPU-limit counts cannot be attributed one-for-one
  without Cloudflare tail-event samples from that exact period. Post-deploy
  metrics must confirm zero; the primary code-level families (unbounded Qt
  intake, repeated D1 routing work, repeated proof verification, and monitor
  failover exhaustion) are removed or bounded.

## Phase 2 priority plan

### P0 — quota/outage risks (implemented)

1. Remove Qt as a server, publisher, and mirror intake process.

   - Expected impact: eliminates duplicate catalog writes and all idle Qt inbox
     traffic.
   - Risk: medium; ownership moved to native Go and is covered by real Git and
     signed-ack tests.
   - Files affected: `desktop/`, `server/`, `www/public/install.sh`.
   - Behavior change: none for users; the managed node is now the sole server.
   - Estimated request reduction: 100% of legacy desktop publication/intake.

2. Remove per-request D1 routing cursor and edge-cache verified proofs.

   - Expected impact: removes two D1 operations per uncached repository read
     and collapses repeated proof traffic per colo/state generation.
   - Risk: low; endpoint choice remains balanced and clone stickiness remains.
   - Files affected: `app/src/entry.py`.
   - Behavior change: none.
   - Estimated request reduction: 40–60% of mirror-proxy subrequests on typical
     multi-object repository page loads.

3. Fix installer fallback quarantine.

   - Expected impact: source fallback stays green when optional release CAS
     bytes are unavailable.
   - Risk: low; the fallback still performs fresh signed health and refs checks.
   - Files affected: `app/src/entry.py`.
   - Behavior change: removes a false outage only.
   - Estimated request reduction: avoids repeated failing monitor/fallback
     chains and continual outage alert work.

### P1 — major request reduction (implemented)

1. Direct desktop Mirrors and Pulls to active saved Hosts.

   - Expected impact: near-100% App Worker reduction for those views when a
     direct host succeeds.
   - Risk: low/medium; public read route is GET/HEAD-only and private/mutating
     operations remain router-capability protected.
   - Files affected: `desktop/src/MainWindowReleases.cpp`,
     `desktop/src/MainWindowPulls.cpp`, `tools/mirror_gateway.py`.
   - Behavior change: views continue working during a relay outage.
   - Estimated request reduction: 80–100% of desktop relay reads on these tabs.

2. Remove or widen browser polls and pause hidden tabs.

   - Expected impact: 80–100% reduction on the affected page loops.
   - Risk: low; account/repository changes already have push channels and
     reconnect catch-up.
   - Files affected: dashboard modules, `app/public/network.html`, homepage
     documents.
   - Behavior change: none; explicit refresh and push remain immediate.
   - Estimated request reduction: 2,304 requests/day per continuously visible
     network tab, plus eliminated idle homepage/repository-tab requests.

3. Keep active Hosts authoritative for Status.

   - Expected impact: no monitoring work for retired/stale nodes.
   - Risk: low; signed freshness and active-host membership are both required.
   - Files affected: `app/src/entry.py` and status tests.
   - Behavior change: retired mirror17, mirror19, and mirror22 disappear.
   - Estimated request reduction: three retired health targets plus their
     status sampling/history work.

### P2 — follow-up after production measurement

1. Profile cron phase dispatches and consolidate only phases whose CPU and
   replay rules safely fit one invocation. **Implemented** first as the
   `scheduled-batch-v3:*` objects, then (2026-09-13) folded into the
   coordinator's own alarm with alarm continuation, because a coordinator
   waiting on batch objects was billed for every second they worked.

   - Expected impact: up to several thousand Durable Object executions/day.
   - Risk: medium/high; isolation currently prevents one phase from exhausting
     another's CPU/subrequest budget. Mitigated by reserving each phase's
     measured high-water subrequest cost before it is packed, by giving a
     never-measured phase the whole budget so it can only lead a batch, and by
     handing the rest of a claim back to a fresh object the moment a budget is
     spent.
   - Files affected: App cron runner and status scheduling.
   - Behavior change: none intended. Phase order inside the status chain,
     per-phase failure isolation, replay rules and the completion heartbeat are
     reproduced inside the coordinator's tick.
   - Estimated request reduction: measurement-dependent; the observed figure is
     reported per tick by `/api/accounts/admin-cron-report`.

2. Consider serving the two public-profile dashboard assets directly from WWW
   instead of proxying them through WWW -> App.

   - Expected impact: removes one App service invocation for each such asset on
     a `forkmesh.com/@user` visit.
   - Risk: medium; asset fingerprints must stay exactly synchronized.
   - Files affected: WWW/App asset build pipeline and CSP tests.
   - Behavior change: none intended.
   - Estimated request reduction: two App invocations per uncached profile
     asset load.

3. Apply Cloudflare rate-limit/WAF rules only after sampled logs identify
   abusive paths or agents. Reject malformed paths before D1/mirror work.

   - Expected impact: traffic-dependent.
   - Risk: legitimate Git clients and search crawlers must be excluded.
   - Files affected: Cloudflare zone configuration, not application behavior.
   - Behavior change: abusive/malformed traffic only.
   - Estimated request reduction: unknown until bot attribution exists.

### P3 — cleanup

Rename legacy in-memory `catalogMirrors*` desktop members now that they cache a
direct Host fleet, and retire the unused D1 `edge_route_cursor` table in a later
backward-compatible migration. Neither cleanup should block rollout.

## Production verification and projection

After deployment, mirror20, mirror21, mirror23, mirror24, and mirror25 all
returned valid signed `forkmesh/forkmesh` proofs with `integrity=ok`, fourteen
advertised operations, and the same refs digest prefix `1ea332d1021c`. The
native service and direct gateway were rolled out to those five saved Hosts;
each direct branches endpoint returned HTTP 200 with the same 53 branches and
the same `main` commit. The live Status API listed exactly those five mirror
systems as operational; mirror17, mirror19, and mirror22 were absent.

The live installer monitor is operational because the signed installer and its
independently verified source fallback are reachable. The release manifest's
older authenticated prebuilt CAS objects were not republished from this dirty
development worktree: publishing desktop binaries remains intentionally gated
on a clean, exact source commit. Do not describe the new desktop build as
released until it is committed and the release publisher has generated,
signed, and replicated its new CAS objects.

```text
Current observed:
  ~55,000 Worker invocations/day
  ~108,000 subrequests/day

Projected after deployment (traffic-mix dependent):
  ~15,000–25,000 Worker invocations/day
  ~25,000–40,000 subrequests/day

Projected reduction:
  invocations: ~55–73%
  subrequests: ~63–77%
```

The upper end meets the 70% target if the supplied production traffic is, as
the code paths suggest, dominated by the removed polling and mirror-proxy work.
Confirm with a 24-hour before/after comparison split by route, cache hit/miss,
selected mirror, retry count, upstream duration, and failure reason. Do not
treat a plan upgrade as the fix; it is only optional operational headroom.
