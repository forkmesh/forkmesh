# ForkMesh World

This directory owns the World Worker (`world/worker.js`), the static three.js
client (`world/public/world/`), the World API and Durable Object source
(`world/src/`, staged into the App Worker), tools, and tests.

`world.forkmesh.com` is served by the three core nodes,
not by a Cloudflare Worker. Caddy serves the built static files from
`world/dist` in the current app release; the routing rules are generated from
this directory's `wrangler.toml` (`run_worker_first`), `world/dist/_headers`
and `world/worker.js` by `deploy/core/caddy_routes.py`, so `wrangler.toml` is
still the source of truth for which paths reach the App. The App-owned
scripts and assets World uses (`WORLD_APP_ASSET_PATHS` in
`caddy_routes.py`, mirroring `APP_ASSET_PATHS` in `world/worker.js`) are
served from the App's `dist`; the HTML shell and API delegation go to the App
on the same node; and `GET /health/mobile` is answered by the host
(`app/host/forkmesh_host/world_health.py`). `world/worker.js` itself is not
deployed.

World API and Durable Object source is staged into the App Python worker for
now. This preserves the existing `ForkMeshWorld` and `ForkMeshOfficeRoom`
classes, bindings, namespace history, and same-origin API at
`app.forkmesh.com/api/world/*` while keeping the source physically owned here.

The developer home page, with the code map, architecture, traps and recent
changes, is [`docs/world/README.md`](../docs/world/README.md).

Quick commands:

```sh
python3 world/tools/world_dev_server.py      # local frontend, production backend, http://127.0.0.1:8788/world/
python3 -m pytest -q world/tests
cd world/browser_tests && npm ci && npm test # @critical suite
npm run test:device-tiers                     # adaptive-rendering device matrix
```

The critical browser suite includes the renderer workload gate: while
walking the default town the scene must stay at or below 450 draw calls and
750,000 submitted triangles (`world/browser_tests/tests/world.spec.js`). Its
`world-render-workload.json` attachment records the measured workload.

The numbered World D1 migrations remain in `app/migrations`. They are
immutable entries in the shared database's applied migration sequence.
Moving them would change Wrangler's migration directory and break ordering
for existing installs. They can move only with a planned World database and
Durable Object namespace lifecycle migration that preserves every applied
version and production row.
