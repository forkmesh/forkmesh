# World development

Part of [World](README.md). How the World is built, served, tested and
deployed: the code map, the serving path on the core nodes, the
`/api/world/*` interfaces, configuration, the test suites and the
performance gate, and the traps that catch new code. How the scene renders is
on [rendering](rendering.md).

## Code map

| Path | Responsibility |
| --- | --- |
| `world/public/world/` | The static client (scene, Office, packages loader, vendored three.js) |
| `world/public/world/index.html`, `world-loader.js` | Shell, static loading card and module loader |
| `world/public/world/world.js` | Client runtime: session restore, sockets, HUD, chat, preferences. The build stages it as a small shell that imports `world-runtime.js` |
| `world/public/world/world-scene.js` | Re-export facade of the scene API; the code is in `scene/` |
| `world/public/world/scene/core.js` | `createWorldScene`: renderer setup, shared state, the scene context, district build order, the public API object |
| `world/public/world/scene/*.js` | Scene helpers by domain (avatars, boards, canvas plates, layout, quality, resources, office props, ...) |
| `world/public/world/scene/runtime/*.js` | Behaviour that runs after construction: movement, camera, rides, pointer input, network updates, the frame loop, diagnostics |
| `world/public/world/scene/districts/*.js` | One module per district or Office area: construction (`build`) and behaviour (`functions`) |
| `world/public/world/scene/figure-batches.js` | Members Circle figures and live peers drawn as shared instanced batches |
| `world/public/world/scene/render-stalls.js` | Render stall monitor: splits frames over 50 ms into compiles, uploads, shadow and reflection draws |
| `world/public/world/scene/gpu-pacer.js` | Program-link pacing and texture upload budget |
| `world/public/world/world-office*.js` | The Office tower, meeting rooms and task boards |
| `world/public/world/world-voice*.js`, `world-channels.js`, `world-avatar-puppets.js`, `world-reaction-pops.js`, `scene/avatar-rig.js`, `scene/runtime/avatar-voice.js` | Voice and camera in the World, avatars driven by face and hands, privacy bubbles, voice rooms, word badges, the one channel list, the live diagnostics chart, on-air signs, on-screen reaction pops ([voice.md](voice.md)) |
| `world/public/world/scene/genie-spider.js`, `scene/runtime/genie-spider.js` | The Genie spider in the 3D World: model, 100 acts with things nearby ([genie-spider.md](genie-spider.md)) |
| `world/public/world/world-floor-notes.js` | Floor notes: placement chip, floor cards, thread panel, @mention suggestions |
| `world/public/world/world-profiler.js` | Admin profiler: frame phases, per-callback cost, draw-call attribution, GPU timer queries, scene audit |
| `world/public/world/world-resource-cache.js` | Shared geometry/material caches, instanced items, instanced mirrors |
| `world/public/world/world-debug-log.js` | Admin debug log: counters, ring buffer, error/console/long-animation-frame/HTTP observers |
| `world/public/world/world-mesh.js` | World mesh: directory normalising and the watch/visit sockets to neighbouring worlds ([World mesh](mesh.md)) |
| `world/public/world/world-boot.js` | Load log: boot timeline, scene-element and shader-compile trace, load suggestions |
| `world/public/world/world-territory.js` | An instance's own hexagon: settings from `/api/instance`, the ForkMesh elements it hides, crossing URLs and arrival parameters, the bug list ([INSTANCES.md](../operations/instances.md#an-instance-world-is-its-own-hexagon)) |
| `world/public/world/scene/instance-territory.js`, `scene/territory-geometry.js` | The instance hexagon's scene (plaza, sign, buildings, bug terrarium, the ForkMesh hexagons next door) and the road-crossing geometry |
| `world/public/world/world-brand.js` | On a white-label instance, renames "ForkMesh" in canvas-painted scene text to the instance name ([INSTANCES.md](../operations/instances.md#the-world-on-an-instance)) |
| `world/public/world/world-marketplace-packages.js` | Loader for World packages from `marketplace/packages/*/content/world/` |
| `world/public/world/elements/` | Self-contained purchasable World elements; contract in its `README.md` |
| `world/public/world/vendor/` | Pinned three.js (`three-0.184.0`) and other vendored libraries |
| `world/src/*.py` | World and Office routes (`/api/world/*`), element store, events, community boards; staged into the App |
| `world/src/world_durable_objects.py` | `ForkMeshWorld` and `ForkMeshOfficeRoom` |
| `world/src/world_mesh.py` | World mesh directory, visitor visas and the cross-world origin rule |
| `world/wrangler.toml` | Not deployed. Its `run_worker_first` list and `[build]` command are read by `deploy/core/caddy_routes.py` and `tools/next_deploy.py` |
| `world/worker.js` | Not deployed. `caddy_routes.py` mirrors its `APP_ASSET_PATHS`; `app/host/forkmesh_host/world_health.py` reimplements `mobileWorldHealth()` |
| `deploy/core/caddy_routes.py` | Generates `world-routes.caddy` per release |
| `tools/next_deploy.py` | Builds World, injects the API origin meta, ships `world/dist`, writes the World deploy notice |
| `app/host/forkmesh_host/world_health.py` | Answers `/health/mobile` for the World host from the files in `world/dist` |
| `world/tools/build_worker_footprint.py` | Source footprint report and World module budgets ([footprint](../app/worker-footprint.md)) |
| `world/tools/world_dev_server.py` | Local frontend against the production backend |
| `world/tools/package.json`, `bundle_world_scene.mjs` | Pinned esbuild and the scene bundle for the staged build |
| `world/tests/` | Python tests (many read the JavaScript as text) |
| `world/browser_tests/` | Playwright suites, including the render workload gate |

## Architecture

- **Build.** `tools/next_deploy.py build` builds World
  (`world/wrangler.toml [build]`), injects the API origin meta, the deploy
  revision, version/build-time metas and an API preconnect link into
  `index.html` (`inject_world_meta`), and ships `world/dist` in the release.
  The production build bundles and minifies `scene/`; only
  `WORLD_BUNDLE_ENTRIES` (`app/tools/build_site_assets.py`) stay as files.
  Module URLs carry `?v=<hash>`; `.map` files sit next to each module.
- **Serving.** world.forkmesh.com resolves by DNS round robin to the core
  nodes, like app.forkmesh.com ([architecture](../architecture.md)). On each
  core, Caddy serves the built files from `world/dist` in the current app
  release and routes each request by the generated
  `world-routes.caddy`: `/`, `/world` and `/world/` get the shell
  (`/world/index.html`, `no-store`) from `world/dist`; `/health/mobile` goes
  to the App host; a path in `run_worker_first` goes to the App, except the
  App-owned scripts and assets World uses, which are served from the App's
  `dist`; any other existing file is served from `world/dist`; everything
  else goes to the App. Python never serves World's static bytes.
- **Backend.** `/api/world/*` is served by the App from `world/src`.
  Presence and chat run in the `ForkMeshWorld` object over `/api/world/ws`;
  Office meetings in `ForkMeshOfficeRoom`. These Durable Object classes run
  on the cores' emulation (`durable.py`), clustered across the core nodes. World chat is global: one `#general` for everyone.
- **Session.** World is a separate origin. It restores the App login from the
  `forkmesh_account` cookie within `WORLD_SESSION_RESTORE_BUDGET_MS` (250 ms)
  and otherwise starts as a guest, adopting the login when it arrives
  ([sessions](../app/auth-sessions.md)).
- **Packages.** Optional districts ship as marketplace packages of type
  `virtual.world` (for example `forkmesh-sky-office`).

## Protocols and interfaces

- `/api/world/*` HTTP routes (`world/src/world_routes.py`,
  `world_office_routes.py`, `world_events_api.py`, `world_community_api.py`).
- `/api/world/ws` WebSocket to `ForkMeshWorld`; Office rooms in
  `ForkMeshOfficeRoom`.
- `GET /health/mobile` on the World host: JSON with
  `x-forkmesh-worker: world`, required by the App's `world_mobile` status
  probe. It checks file sizes in `world/dist` (`world-scene.js` bound
  1 KB–4 MB).
- Element contract: `world/public/world/elements/README.md`; prices are set by
  `world/src/world_element_store.py`.
- `GET /api/start/instances/<id>/logo`: an instance logo, drawn on its
  hexagon.
- `GET /api/world/mesh`, `GET /api/world/neighbour-scene`,
  `GET /api/world/neighbour-icon`, `POST /api/world/visa`,
  `POST /api/world/visa/verify` and the `watch` / `visa` forms of
  `/api/world/ws`: the [World mesh](mesh.md).

## Configuration

`WORLD_ORIGIN` is `https://world.forkmesh.com`: in `deploy/fleet.toml`
`[production.vars]` (rendered as `FORKMESH_VAR_WORLD_ORIGIN` by
`deploy/core/render.py`), in `www/wrangler.toml` for the website Worker, and
in `app/wrangler.toml` for the optional Workers self-host. The served host is `world_host` in `deploy/fleet.toml`.
`WORLD_MESH_HUB` (default `https://app.forkmesh.com`) is
where an instance reads the World mesh directory; an App without
`INSTANCE_NAME` is the hub itself. `WORLD_NEIGHBOUR_SCENE` (optional, on an
instance) is the scene its single hexagon shows in the hub's World ([neighbour
scenes](mesh.md#neighbour-scenes)). The served stack reads only the `run_worker_first`
list and `[build]` command of `world/wrangler.toml`; it holds no secrets.

## Build, run, test

```sh
python3 world/tools/world_dev_server.py          # http://127.0.0.1:8788/world/
python3 -m pytest -q world/tests
cd world/browser_tests && npm ci && npm test     # @critical suite
npm run test:device-tiers                         # adaptive-rendering matrix
python3 tools/world_bench.py --gate              # performance gate (see below)
```

`world/tests/test_world_dev_server.py` drives `WorldDevHandler` over a
`socketpair`, whose buffer on macOS is 8 KB, smaller than `index.html`
(about 22 KB). The test runs the handler on a thread while the client reads,
with a 10 s timeout on both sockets; calling it inline blocks `sendall`
forever. The server is unaffected (`ThreadingHTTPServer` gives each
connection a thread).

The `@critical` suite has a 55 s global budget (`playwright.config.js`,
pinned by `app/tests/test_offline_ci_workflow.py`). The GitHub workflow runs it
with `npm test -- --global-timeout=0`, bounded by the job's `timeout-minutes`
(as `test:device-tiers` is): `ubuntu-latest` has 4 vCPUs and no GPU, so
Chromium draws with SwiftShader at 120–250 ms a frame and the six tests take
two to three minutes, well past 55 s.

On a software renderer the browser specs gate on frames, not milliseconds:

- The input-response test keeps its 100 ms budget on hardware; under
  SwiftShader it allows twelve measured frames (100 ms is six at 60 Hz,
  doubled for SwiftShader's frame-to-frame jitter) and drops its first two
  camera drags, one in each direction, because each carries about 500–700 ms
  of one-time work for the part of the town it brings into view (later drags
  stay under five frames).
- `world-device-tiers.spec.js` applies its calibrated per-tier floors only to
  a hardware run; under SwiftShader it asks that the world keeps presenting
  (0.25 fps: desktop-high at 1600×900 with shadows draws about one frame a
  second on four cores) and that input lands within twelve of its frames.
- The two-client sync test boots two worlds and is `test.slow()`.

To reproduce CI locally, unset `DISPLAY` and `WAYLAND_DISPLAY` (otherwise
headless Chromium cannot create a WebGL context in an agent shell) and pin to
four cores with `taskset -c 0-3`.

The desktop's World page prefers a local dev server at
`http://127.0.0.1:8788/world/` when one answers (`worldDevServerUrl` in
`desktop/src/ControlNode.cpp`; the setting value `off` disables it; only
loopback hosts are accepted).

## Performance gate

`tools/world_bench.py` measures the World at nine fixed camera poses on the
real GPU and fails when a pose gets slower, draws more, or looks different.
It drives headless Chromium through `tools/world_bench.mjs` and the in-page
library `tools/world_bench_page.js` (`window.__worldBench`) against
`world/tools/world_dev_server.py` with the upstream pointed at a dead
address. Each pose gets a fresh page with a seeded `Math.random`, a virtual
clock, fixture answers for every `/api/*` request and no socket; it settles,
freezes time, captures counts and a 320×180 golden frame, then measures live
frame cost. Poses: `spawn`, `office-lobby`, `office-rooftop`,
`office-engineering`, `beach`, `members-circle`, `arrival-stats`,
`repository-portals`, `town-overview`.

- Budgets live in `world/perf-budget.json`. Time ceilings are
  `max(baseline × 2.0, baseline + 5 ms)` and compared only on the same GPU;
  draws and triangles are exact; other counts have tolerances; a pose's
  `unbudgeted` list reports a noisy metric without gating on it.
- Golden frames live in `~/projects/forkmesh/cache/world-bench/golden/<gpu-slug>/`;
  a pose fails when more than 0.5 % of pixels differ by more than 32 (YIQ).
- `--gate` exits 0 with "skipped" when there is no GPU render node or Node.

```sh
python3 tools/world_bench.py --gate               # budget + golden frames
python3 tools/world_bench.py --check --pose beach # budget only, one pose
python3 tools/world_bench.py --report a.json --report b.json --update-baseline
python3 tools/world_bench.py --tighten            # lower ceilings this run beat
python3 tools/world_bench.py --bless              # write this run's frames as goldens
```

When a change is meant to cost more, rerun twice and `--update-baseline` from
both reports; when it is meant to look different, `--bless` after checking
the diffs. Pure-Python parts are tested by
`world/tests/test_world_perf_budget.py`.

## Operations

World is deployed with every app release
([docs/operations/fleet-tooling.md](../operations/fleet-tooling.md)): the release carries
`world/dist` and `world-routes.caddy`, and Caddy is reloaded when the release
switches. World API changes ship in the same release. `tools/next_deploy.py`
writes `world_deploy_status` for the "ForkMesh is deploying now" notice
([deploy/auto-deploy.md](../operations/auto-deploy.md#world-deploy-notice)).

## Security

- Elements get only `THREE`, validated params and an allowlisted
  same-origin `net()`; they have no other I/O.
- Chat content is encrypted by the chat client; the Office uses the same
  channel authorization as `/chat` ([sealed chat](../security/sealed-chat.md)).
- The local speech bridge: [world-local-speech-bridge.md](../security/world-local-speech-bridge.md).
- Optional elements that read data (for example the status board, which
  reads `/api/status/world`) declare the endpoint in their manifest and
  disclose it before purchase.

## Traps

- Editing `world/worker.js` changes nothing live; change `caddy_routes.py` or
  `world_health.py` too. `world/deploy.sh` is a legacy Workers deploy and does
  not deploy World to the core nodes; World ships with the app release.
- World migrations are numbered in `app/migrations/` and stay there; they run
  on rqlite.
- `vertexColors: true` needs a `color` attribute, or the mesh draws black.
  Per-instance colours from `setColorAt()` need no flag. `test_world_sky.py`
  checks the sky objects.
- **Render budget.** `world/browser_tests/tests/world.spec.js` fails when
  walking the default town exceeds 450 draw calls or 750,000 triangles.
- **Transmission materials** make three.js re-render every opaque object
  offscreen; `test_world_render_performance.py` pins them absent.
- **Point-light count.** Changing the visible count recompiles every lit
  shader; pad with dark lights instead of toggling visibility.
- `getWorldPosition()` marks every ancestor dirty; the settled matrix walk
  ignores that flag on auto-updating objects.
- **Frozen objects have `matrixAutoUpdate === false`.** Do not use the flag to
  mean "static" or set it yourself to drive `object.matrix`; move objects
  through `position`/`quaternion`/`rotation`/`scale` or call
  `updateMatrixWorld(true)`.
- **Out-of-view settled subtrees are `visible = false` during the main
  render.** Code in `onBeforeRender` must not read `visible` as "shown". A
  shader that displaces geometry outside its bounding sphere must set
  `frustumCulled = false`.
- **Construction-time canvases paint later.** A painter whose result the
  caller reads at once must use `canvasArtTexture`.
- **The main view is not drawn until the first view is compiled.** Do not
  read pixels or rely on a presented frame while it is held.
- **`createWorldScene` yields.** Listeners, timers and promise callbacks
  created during construction must not reach bindings declared later until
  construction ends (`worldBuilt`/`sceneConstructed`).
- **Parameterised geometries can be shared after a few seconds.** Do not
  mutate a `BoxGeometry`/`PlaneGeometry` in place long after building it;
  build a plain `BufferGeometry` or use `DynamicDrawUsage`.
- **Deployed modules are minified.** Map a `file.js:<offset>` with the `.map`
  next to the file. A module URL assembled at run time is not versioned by
  the build.
- CSS: new HUD rules belong in the `.fm-world` component layer at the end of
  `world.css`; some chat rules use `!important`. Never put `filter`,
  `transform` or `backdrop-filter` on an ancestor of the phone HUD's fixed
  avatar badge. An element given `display` in the component layer needs a
  `[hidden] { display: none; }` rule. Avoid `backdrop-filter` over the live
  canvas. Rules for retired UI leave with it; tests check that the chat
  terminal, quick channel strip, avatar action menu, `.world-loading-curtain`
  and HTML aquarium controls stay out of `world.css`.
