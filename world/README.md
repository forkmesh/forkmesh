# ForkMesh World

This directory owns the static three.js client (`world/public/world/`), the
World API and Durable Object emulation source (`world/src/`, staged into the
App), tools and tests. The developer home page, with the code map,
architecture and traps, is [docs/world/README.md](../docs/world/README.md);
the served stack is described in
[Architecture and availability](../docs/architecture.md).

`world.forkmesh.com` is served by the three core nodes, reached by DNS round
robin. Caddy serves the built files from `world/dist` in the current app
release. `deploy/core/caddy_routes.py` generates the routing rules from this
directory's `wrangler.toml` (`run_worker_first`), `world/dist/_headers` and
`world/worker.js`, so `wrangler.toml` is still the source of truth for which
paths reach the App. App-owned scripts and assets World uses
(`WORLD_APP_ASSET_PATHS` in `caddy_routes.py`, mirroring `APP_ASSET_PATHS` in
`world/worker.js`) come from the App's `dist`; the HTML shell and API go to
the App on the same node; `GET /health/mobile` is answered by the host
(`app/host/forkmesh_host/world_health.py`). `world/worker.js` itself is not
deployed.

The World API keeps the `ForkMeshWorld` and `ForkMeshOfficeRoom` classes,
their bindings and namespace history, and the same-origin API at
`app.forkmesh.com/api/world/*`, while the source stays here.

```sh
python3 world/tools/world_dev_server.py      # local frontend, production backend, http://127.0.0.1:8788/world/
python3 -m pytest -q world/tests
cd world/browser_tests && npm ci && npm test # @critical suite
npm run test:device-tiers                     # adaptive-rendering device matrix
```

The critical browser suite includes the render workload gate: walking the
default town must stay at or below 450 draw calls and 750,000 submitted
triangles (`world/browser_tests/tests/world.spec.js`); the
`world-render-workload.json` attachment records the measurement.

The numbered World migrations stay in `app/migrations`: they are immutable
entries in the shared database's applied migration sequence, and moving them would
break ordering for existing installs. They can move only with a
planned World database and namespace migration that preserves every applied version and
production row.
