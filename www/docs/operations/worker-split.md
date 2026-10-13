# Cloudflare deployment boundaries

ForkMesh has three product Workers. The App owns its complete control plane;
there is no separate API or edge-control Worker.

| Directory | Script | Official hostnames | Responsibility |
|---|---|---|---|
| `app/` | `forkmesh-app` | `app.forkmesh.com` | Product UI, `/api/*`, health and release identity, public snapshots, WebSockets, Git Smart HTTP, ActivityPub, D1, KV, AI, Cron, migrations, and Durable Objects |
| `www/` | `forkmesh-www` | `forkmesh.com`, `forkmesh.com` | Landing pages, marketing, documentation, blog, and shared browser assets |
| `world/` | `forkmesh-world` | `world.forkmesh.com` | World shell, modules, models, soundtrack, avatars, and World media |

The canonical API base is `https://app.forkmesh.com/api`. The retired
`app.forkmesh.com` hostname has no DNS record, route, trust entry, or CORS
authority.

## Routing and snapshot contract

The App has one custom-domain route for `app.forkmesh.com`. It directly serves
the root redirect, `/health`, `/api/version`, and GET
`/api/mirrors/https`; registration POSTs and every other operational protocol
remain in the same App runtime. Identity responses are `no-store`.

Five exact anonymous GET routes (`/api/repositories`,
`/api/network/overview`, `/api/repo/forkmesh/forkmesh/mirrors`, `/api/status`,
and `/api/status/world`) read validated, fresh-only projections from the main
App `DB`. Authorization, every Cookie, query strings, cache-bypass directives,
HEAD, and non-GET methods bypass snapshots. Missing, expired, or canonical
release-mismatched rows use one no-store live fallback; corrupt rows and active
privacy-transition tombstones fail closed. Snapshot responses expose their
freshness, generated timestamp, and deploy fingerprint through the normal App
CORS policy.

`www` redirects exact `/api` to the App developer page and service-proxies
`/api/*`, Git Smart HTTP, ActivityPub, webhook bodies, and WebSocket upgrades to
`forkmesh-app`. Product routes redirect to App and `/world*` redirects to
World. `world` serves World assets, injects `APP_ORIGIN` so its HTTP and
WebSocket traffic reaches the configured App, and delegates non-World routes
to the App service binding so the App remains the canonical route owner. It
also service-proxies App-owned shell dependencies instead of duplicating them
in the World Static Assets upload.

## Asset and configuration ownership

- `app/public` owns login, signup, dashboard, repository, chat, notes, network,
  status, rewards, accounts, and file icons.
- `www/public` owns official marketing, browser docs, blog, favicons, fonts,
  video, and shared site chrome. Authored docs live under `www/docs`.
- `world/public` owns World code and World-only media.

The built World payload contains only `world/public` plus its generated
`_headers`. Shared fonts, favicons, logos, telemetry, and chat modules remain in
the App deployment and resolve through the World-to-App service binding.

`app/wrangler.toml` owns the App route, one-minute Cron Trigger, main D1
binding, KV, AI, migrations, and Durable Object classes. The checked-in WWW
and World service bindings point to `forkmesh-app`.
`app/tools/build_site_assets.py` creates ignored `app/dist`, `www/dist`, and
`world/dist` staging trees.

## Scheduled maintenance

The App scheduled handler is a doorbell: it rings the singleton
`scheduled-runner-v1` object, whose alarm runs every due phase of the minute
itself. Workers Free allows fifty subrequests per invocation, so when the
next phase's measured reserve no longer fits, the object persists its progress
(`tick_state_v5`) and re-arms its alarm for "now"; the following invocation
resumes on the same object with a fresh budget. A quiet minute is therefore one
alarm invocation on one object, and a busy one is a short chain of invocations
on that same object, never a fan-out to other objects that would be billed
while the coordinator waited for them. The status projection chain leads the
list and stays ordered, every other phase keeps its failure isolation, a phase
started by an invocation that died is reported incomplete rather than replayed
unless it is replay-safe, and the completion watchdog rides the invocation that
finishes a tick and is withheld whenever anything failed. Every tick writes one
bounded `cron_tick` row (its `batches` column now counts alarm invocations),
which is what `/api/accounts/admin-cron-report` and the desktop Log page's
Cron tab read.

The former `scheduled-batch-*`, `scheduled-phase-v2:*` and
`scheduled-status-v1:*` receivers are gone; those object names answer 404.

## Self-hosted topology

A default self-host has one complete App at
`https://forkmesh.example.com`. Set `PUBLIC_BASE_URL`, `API_ORIGIN`,
`APP_ORIGIN`, `WWW_ORIGIN`, and allowed CORS origins to that origin, and set
`SINGLE_WORKER_SITE=true`. The bootstrapper creates one App Worker, one main D1
database, one proxied hostname, and one broad App route. It does not create or
delete sibling control scripts or exact overlay routes. Any legacy overlapping
route fails closed and must be removed in a separately authorized control-plane
cutover. World remains optional.

## Skipping unchanged Workers

`./deploy.sh deploy` and `./deploy.sh changed` fingerprint each target's inputs
and compare them with the fingerprint the live Worker reports. A target whose
live fingerprint already matches is skipped, so a repeated deploy is cheap.

Pass `--force` (or `-f`) to any deploy command to bypass that check and upload
the target anyway:

```bash
cd app
./deploy.sh changed --force   # every Worker
./deploy.sh app --force       # one target
```

Forcing is the recovery path when the live Worker is wrong in a way the
fingerprint cannot see: a partially applied upload, a manually edited script, or
a Worker variable changed outside `deploy.sh`. It is equivalent to setting
`FORKMESH_FORCE_DEPLOY=1`, and on `deploy`/`changed` it carries into every
per-target run. It skips only the change check; every migration, secret, and
post-deploy verification gate still runs.

The desktop control plane exposes the same flag: the **Force** toggle in
Control → Site deployment arms `--force` for the next deploy action and clears
itself once that run starts, so it cannot silently stay on.

## Safe official consolidation cutover

The old exact edge routes continue to win until the consolidated App and all
five current snapshot rows are ready. Before running prepare, disable the
legacy edge Cron Trigger and pre-expire every normal row in the legacy
snapshots D1 database. Then use the explicit prepare stage:

```bash
cd app
FORKMESH_APP_CONSOLIDATION_PREPARE=1 \
FORKMESH_APP_WORKER_NAME=forkmesh-relay \
FORKMESH_LEGACY_EDGE_QUIESCED=1 \
./deploy.sh app
```

The acknowledgement flag does not replace the read-only D1 proof: prepare
fails unless the old schedule is acknowledged quiesced, the legacy target is
named explicitly, and every legacy normal row is explicitly expired. It then deploys
the consolidated code and secrets to the existing script, applies main-D1
migrations, verifies `/api/mainnode`, and waits for all five current rows. It
does not require the still-overlaid public routes and does not mutate or delete
external edge resources.

Complete the control-plane cutover in this recoverable order:

1. Remove (drain) all nine old exact routes: App root, health, version, mirror
   identity, and the five snapshot paths. Do not delete the old script yet.
2. Run `./deploy.sh post-deploy-verify`. It must prove the root, health,
   version, identity, and every snapshot response carry App ownership and no
   legacy edge marker. Re-confirm the old schedule remains quiesced.
3. If any takeover check fails, restore all nine routes and the old schedule
   before doing anything destructive, then diagnose and retry.
4. Export the now-unbound legacy snapshots D1 database and retain the export
   under the operator's normal recovery policy. Terminate every active tail on
   the old script so no observer outlives resource ownership.
5. Delete the old control Worker, then delete its unbound snapshots D1 database.
6. Rename the existing `forkmesh-relay` script in place to `forkmesh-app`
   through Cloudflare's UUID-preserving PATCH. Do not create a second App
   script or recreate Durable Object namespaces.
7. Re-run App verification, deploy WWW with its checked-in `forkmesh-app`
   service binding, and verify the WWW service-proxy paths.

The edge schedule, export, tail, Worker, D1, and rename steps are external
control-plane operations; `deploy.sh` never deletes those resources itself.

Normal `./deploy.sh app` deploys `forkmesh-app`, publishes secrets to that exact
same target, waits for main-D1 snapshot rows, and verifies App ownership,
release identity, router identity, and every public snapshot route. `deploy`,
`changed`, `world`, `www`, `status`, and `dry-run` retain their target-specific
roles.

## Router identity and rollback

Generic secret publication preserves the matched router signing seed and
public key already on the App. Explicit rotation validates the local pair and
publishes it atomically to the App; no separate public-key-only script exists.

Migrations are roll-forward, so an application rollback must accept the newest
schema. A rollback across consolidation must preserve privacy tombstones and
the direct phase dispatcher. If that cannot be proven, roll forward instead of
reintroducing exact overlay routes, a separate snapshots database, or the
legacy `/kick` scheduler.
