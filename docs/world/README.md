# World (`world/`)

ForkMesh World is the three.js town at `world.forkmesh.com` where accounts,
repositories, mirrors and agents appear as places and avatars, with global
chat, the Office (encrypted meeting rooms) and community boards. The browser
code is static. It is served by the three core nodes: Caddy serves the built
files from `world/dist` in the current app release, and the App on the same
node answers `/api/world/*`, the World Durable Objects and `/health/mobile`.
The desktop embeds the same page.

## Code map

| Path | Responsibility |
| --- | --- |
| `world/public/world/` | The static client (scene, Office, packages loader, vendored three.js) |
| `world/public/world/index.html`, `world-loader.js` | Shell and module loader |
| `world/public/world/world.js` | Client runtime: session restore, sockets, HUD, chat, preferences. The build stages it as a small shell that imports `world-runtime.js` |
| `world/public/world/world-scene.js` | Re-export facade of the scene API; the code is in `scene/` |
| `world/public/world/scene/figure-batches.js` | Members Circle figures and live peers drawn as shared instanced batches with per-member texture layers, near/far detail, hide-during-render |
| `world/public/world/scene/render-stalls.js` | Always-on render stall monitor: splits frames over 50 ms into program compiles, texture uploads, shadow and reflection draws; records first-sight compiles with their World element |
| `world/public/world/scene/*.js` | Scene helpers by domain (avatars, boards, canvas plates, layout, quality, office props, ...) |
| `world/public/world/scene/core.js` | `createWorldScene`: renderer setup, shared state, the scene context, district build order, the public API object |
| `world/public/world/scene/runtime/*.js` | Behaviour that runs after construction: movement, camera, rides, pointer input, network and repository updates, the frame loop, diagnostics |
| `world/public/world/scene/districts/*.js` | One module per district or Office area: its construction (`build`) and its own behaviour (`functions`) |
| `world/public/world/world-office*.js` | The Office tower, meeting rooms and task boards |
| `world/public/world/world-floor-notes.js` | Floor notes: tap-to-place chip, floor cards, thread panel, @mention suggestions |
| `world/public/world/world-profiler.js` | Admin performance profiler: frame-phase timing, per-callback cost, draw-call attribution, GPU timer queries, three.js scene audit |
| `world/public/world/world-resource-cache.js` | Shared geometry/material caches with a no-op `dispose`, instanced items with `instanceId` hit mapping, instanced mirrors of pickable meshes, repeated-mesh collection |
| `world/public/world/world-debug-log.js` | Admin debug log: category counters, ring buffer, error/console/long-animation-frame/HTTP observers |
| `world/public/world/world-boot.js` | Load log: timestamped boot lines from time zero, scene-element and shader-compile trace, load suggestions, panel visibility |
| `world/public/world/world-marketplace-packages.js` | Loader for World packages from `marketplace/packages/*/content/world/` (for example `forkmesh-sky-office`) |
| `world/public/world/elements/` | Self-contained purchasable World elements; contract in its `README.md` |
| `world/public/world/vendor/` | Pinned three.js (`three-0.184.0`) and other vendored libraries |
| `world/src/*.py` | World and Office routes (`/api/world/*`), element store, events, community boards, satellites; staged into the App |
| `world/src/world_durable_objects.py` | `ForkMeshWorld` and `ForkMeshOfficeRoom` |
| `world/wrangler.toml` | Not deployed. Its `run_worker_first` list and `[build]` command are read by `deploy/core/caddy_routes.py` and `tools/next_deploy.py` |
| `world/worker.js` | Not deployed. `caddy_routes.py` mirrors its `APP_ASSET_PATHS`, and `app/host/forkmesh_host/world_health.py` reimplements its `mobileWorldHealth()` |
| `deploy/core/caddy_routes.py` | Generates `world-routes.caddy` per release |
| `tools/next_deploy.py` | Builds World, injects the API origin meta, ships `world/dist` and writes the World deploy notice |
| `app/host/forkmesh_host/world_health.py` | Answers `/health/mobile` for the World host from the files in `world/dist` |
| `world/tools/build_worker_footprint.py` | Python source footprint report and World module budgets ([footprint](../app/worker-footprint.md)) |
| `world/tools/world_dev_server.py` | Local frontend against the production backend |
| `world/tools/package.json` | Pinned esbuild for the staged World build (minify, source maps) |
| `world/tools/bundle_world_scene.mjs` | Bundles the scene module graph for the staged build |
| `world/tests/` | Python tests (many read the JavaScript as text) |
| `world/browser_tests/` | Playwright suites, including the render workload gate |

## Architecture

- **Build.** `tools/next_deploy.py build` builds World
  (`world/wrangler.toml [build]`), injects the API origin meta, the deploy
  revision and an API preconnect link into `index.html`
  (`inject_world_meta`), and ships `world/dist` in the release.
- **Serving.** Caddy routes each request by the generated
  `world-routes.caddy`: `/`, `/world` and `/world/` get the shell
  (`/world/index.html`, `no-store`) from `world/dist`; `/health/mobile` goes
  to the App host; a path in `run_worker_first` goes to the App, except the
  App-owned scripts and assets World uses, which are served from the App's
  `dist` (or the App); any other existing file is served from `world/dist`;
  everything else goes to the App. Python never serves World's static bytes.
- **Backend.** `/api/world/*` is served by the App from `world/src`.
  Presence and chat run in the `ForkMeshWorld` object over `/api/world/ws`;
  Office meetings in `ForkMeshOfficeRoom`. The Durable Objects run clustered
  across the core nodes. World chat is global: one `#general` for everyone.
- **Session.** World is a separate origin. It restores the App login from the
  `forkmesh_account` cookie within `WORLD_SESSION_RESTORE_BUDGET_MS` (250 ms)
  and otherwise starts as a guest, adopting the login when it arrives
  ([sessions](../app/auth-sessions.md)).
- **Packages.** Optional districts ship as marketplace packages of type
  `virtual.world`. The Sky Office moved into the opt-in
  `forkmesh-sky-office` package on 2026-09-13 (`88a29238d`).

## Protocols and interfaces

- `/api/world/*` HTTP routes (`world/src/world_routes.py`,
  `world_office_routes.py`, `world_events_api.py`, `world_community_api.py`).
  The floor-note routes are described under Floor notes.
- `/api/world/ws` WebSocket to `ForkMeshWorld`; Office rooms in
  `ForkMeshOfficeRoom`.
- `GET /health/mobile` on the World host: the same JSON and headers the
  World Worker used to send (`x-forkmesh-worker: world`), which the App's
  `world_mobile` status probe requires.
- Element contract: `world/public/world/elements/README.md`; prices are set by
  `world/src/world_element_store.py`.

## Configuration

`WORLD_ORIGIN` is `https://world.forkmesh.com`: in `deploy/fleet.toml`
`[production.vars]` (rendered as `FORKMESH_VAR_WORLD_ORIGIN` by
`deploy/core/render.py`), and with the same value in `app/wrangler.toml` and
`www/wrangler.toml`. The served host is `world_host` in `deploy/fleet.toml`
`[production]`. `world/wrangler.toml` still carries the old Worker settings
(compatibility date `2026-09-22`, `APP` service binding, `APP_ORIGIN`, route
`world.next.forkmesh.com`); the served stack reads only its
`run_worker_first` list and `[build]` command. It holds no secrets.

## Build, run, test

```sh
python3 world/tools/world_dev_server.py          # http://127.0.0.1:8788/world/
python3 -m pytest -q world/tests
cd world/browser_tests && npm ci && npm test     # @critical suite
npm run test:device-tiers                         # adaptive-rendering matrix
python3 tools/world_bench.py --gate              # performance gate (see below)
```

The desktop's World page prefers a local dev server at
`http://127.0.0.1:8788/world/` when one answers (`worldDevServerUrl` in
`desktop/src/ControlNode.cpp`; the setting value `off` disables it; only
loopback hosts are accepted).

## Performance gate

`tools/world_bench.py` measures the World at nine fixed camera poses on the
real GPU and fails when a pose gets slower, draws more, or looks different.
Rebuilt 2026-09-29 after the first version was lost with a deleted checkout.

**Pieces.**

- `tools/world_bench.py`: pose table, budget maths, CLI. Starts
  `world/tools/world_dev_server.py` on a free port with the upstream pointed at
  a dead address, so nothing reaches production.
- `tools/world_bench.mjs`: the Playwright driver. Headless Chromium with
  `--use-gl=angle --use-angle=gl-egl --ignore-gpu-blocklist` (the real GPU;
  `--software` switches to SwiftShader and skips timing checks). Playwright is
  resolved from `$FORKMESH_PLAYWRIGHT`, then `app/browser_tests` and
  `world/browser_tests` in this checkout (`npm ci --prefer-offline` there),
  then the same two in the primary checkout (found through the repository's
  common git directory). `node` falls back to the newest
  `~/.nvm/versions/node/*/bin/node`.
- `tools/world_bench_page.js`: the in-page library, injected with
  `addInitScript` before any World code (clock, frame pump, placement,
  capture, scene audit, golden diff). It exposes `window.__worldBench`.
- `world/perf-budget.json`: per-pose baselines and ceilings, plus the policy
  and golden-frame thresholds.
- Golden frames: `~/projects/forkmesh/cache/world-bench/golden/<gpu-slug>/<pose>.png`,
  320×180. The slug is the WebGL renderer string lowercased with every run of
  other characters turned into `-`, so each GPU keeps its own set. Reports,
  frames and diff images go to `~/projects/forkmesh/cache/world-bench/runs/`.

**Poses** (in run order): `spawn`, `office-lobby`, `office-rooftop`,
`office-engineering`, `beach`, `members-circle`, `arrival-stats`,
`repository-portals`, `town-overview`. Each is a player placement (town
`setSpawn`, Office `enterOfficeLobby` + floor, or `enterBeachScene`) plus an
explicit camera position and target.

**How a pose runs.** Every pose gets a fresh browser context and a fresh page
load, so shader and texture counts do not depend on the poses before it.

1. An init script takes over the clock and the frame loop before any World
   code runs: `Math.random` is a seeded generator, `Date` and
   `performance.now` read a virtual clock anchored at a fixed date (UTC, noon
   at the frozen instant), and `requestAnimationFrame` queues callbacks that a
   pump either forwards to real frames ("live") or runs on demand ("manual").
2. Every `/api/*` request is answered from generated fixtures: 180 directory
   members, 48 idle avatars, 72 repositories, 12 mirror nodes, arrival stats.
   The socket is intercepted and never connects. Root scripts and `/assets/`
   come from `world/public`, `www/public` or `app/public`; `analytics.js` is
   blanked.
3. Load milestones come from `window.__forkmeshWorldBoot.measure`:
   `firstFrameMs`, `interactiveMs`, `interactiveFrameMs`, and the long
   animation frames during load (`loadLongTasks`).
4. The pose is placed through the scene API on
   `document.querySelector("forkmesh-world").world` (`window.__world` as a
   fallback). Nothing reads World source text, so splitting `world-scene.js`
   into modules does not break the bench. Quality is pinned
   (`setQualityTier("desktop-high", {adaptive: false})`), daylight forced to
   day, light level 100, theme `world`.
5. The camera is held at the pose by wrapping `camera.updateMatrixWorld`.
   Overriding it only inside `renderer.render` was not enough: the settled
   group culler hides groups outside the follow camera's frustum before the
   render call, which emptied the Members Circle and the arrival boards. The
   follow camera's yaw and pitch are also pointed along the pose so
   distance-based detail picks the same objects.
6. Live settle: sample scene object count, geometries, textures, programs and
   draws once a second until five samples match (60 s cap). The idle-time GPU
   warm-up keeps adding programs for up to a minute on the Office poses.
7. Freeze: switch to manual time at a fixed instant (one hour of virtual
   time), hide the objects in `GLOBAL_MASK`, step 20 ms frames until draws
   and triangles repeat, then read counts from that frame and downscale the
   canvas to the golden size in the same task. The mask holds objects whose
   pose integrates real frame deltas from the live phase, so no fixed clock
   makes them repeat: `forkbot-rolling-droid`, the lobby's spinning
   `forkmesh-reflective-fm-cube` (1.09 % of the office-lobby frame in one
   run), and `reward-pool-fireball-sun`/`-corona` over the reward pool
   (0.34 % of arrival-stats). `--probe file.js` runs a script in each page
   after capture; projecting named objects to screen space near the red
   pixels is how these were found.
8. Live measure: back to real frames with the camera still held for four
   seconds. `renderMsP50`/`renderMsP90` are the time spent in all frame
   callbacks (update plus render submit); the report also keeps submit-only
   time, frame intervals and long animation frames.

**Budget.** For time metrics `ceiling = max(baseline × timeFactor,
baseline + timeAbsoluteMs)` with 2.0 and 5 ms. Counts use
`max(baseline × factor, baseline + absolute)` rounded up: draws and triangles
are exact (identical across runs), geometries 1.03/+4, textures 1.15/+32,
programs 1.5/+16 and `loadLongTasks` 2.0/+8. The frozen frame also runs
`world.auditScene()` and budgets its duplicate geometries, duplicate
materials, instancing candidates and meshes with frustum culling off at
1.05/+3. These audit counts were identical in nine runs everywhere except
members-circle `duplicateGeometries`, which read 2, 230, 271 or 448
depending on whether the avatar geometry deduper had finished its idle
slices. A pose's `unbudgeted` list keeps such a metric in the report and out
of the budget. Programs, textures and load timings vary from run to run because the
warm-up compiles in idle slices, textures stay resident or not depending on
timing, and the box is shared. Back-to-back runs saw programs 59–166 per pose,
members-circle textures 990 in one run and 1109 in the next (1.05/+16 failed
on that), and first frame 1.2–6.2 s. Timings are only compared when the report's GPU string matches
the budget's; counts are always compared.

**Golden frames.** Pixels are compared with the YIQ perceptual distance
(pixelmatch's formula) scaled to 0–255; a pixel differs above `tolerance`
(32), and a pose fails when more than `maxChangedPct` (0.5 %) of pixels
differ. The diff image greys the baseline and marks changed pixels red. With
the clock and random generator pinned, repeated runs give 0 % changed.

**Commands.**

```sh
python3 tools/world_bench.py                      # one run, print the table
python3 tools/world_bench.py --gate               # budget check + golden frames; exit 1 on regression
python3 tools/world_bench.py --check --pose beach # budget only, one pose
python3 tools/world_bench.py --runs 2 --write-budget
python3 tools/world_bench.py --report a.json --report b.json --write-budget
python3 tools/world_bench.py --report a.json --report b.json --update-baseline
python3 tools/world_bench.py --tighten            # lower ceilings this run beat
python3 tools/world_bench.py --bless              # write this run's frames as goldens
```

`--gate` prints "skipped" and exits 0 when the box has no GPU render node
(`/dev/dri/renderD*`) or no Node, so a CI or deploy step can call it
unconditionally. `--timeout` (default 1800 s) abandons a stuck driver. A full
run takes 6–8 minutes: nine fresh page loads, each waiting up to a minute for
the GPU warm-up to stop compiling.

`--report` merges existing reports by maximum instead of running.
`--write-budget` writes a fresh budget with the default policy;
`--update-baseline` keeps the file's policy and golden thresholds and
refreshes baselines (only the named poses with `--pose`). A failing check
prints `FAIL  <pose> <metric> <value> > <ceiling> (baseline …)` and golden
failures print the diff path. `--bless` copies any existing golden to
`golden/<slug>/archive/<timestamp>/` before overwriting it.

When a change is meant to cost more (a new district, more avatars), rerun
twice, `--update-baseline` from both reports, and commit the budget with the
change. When it is meant to look different, `--bless` after checking the diff
images.

The committed budget was recorded on 2026-09-29 before main's load-path work
(world-boot, world-scene, world.js) was merged. A gate on the merged tree
passed, with `interactiveMs` down from 3.0–6.1 s to 1.7–2.3 s and textures from
about 1109 to about 600 per pose; `--tighten` after a couple of runs will pull
those ceilings down to the new level.

Load round 2 (2026-09-29): the gate on main before the change passed on the
RX 580 with every golden. Afterwards the render node was not accessible to
the agent's user (see "Load speed round 2"), so the gate ran on llvmpipe for
both main (46eb4ea873) and the branch: identical draws and triangles in every
pose, fewer programs and textures on the branch, the same goldens passing
(members-circle 0.41–0.42 %), and the same `repository-portals` failure
(2.3 %) against a stale llvmpipe golden on both. Timing checks are skipped on
a GPU mismatch, so the RX 580 ceilings were not tightened; run `--runs 2
--tighten` on the RX 580 once it is reachable.

The pure-Python parts (ceilings, merge, check output, CLI) are covered by
`world/tests/test_world_perf_budget.py`; no browser needed.

The goldens that existed before the rebuild were made by the lost harness
with different fixtures (idle avatars wandering the spawn, another player
skin) and could not be reproduced; they are kept under
`golden/<slug>/archive/`.

**The recovered first version** (branch `recovered/perf-world-gate`, carved
from disk after the checkout was deleted) built the scene itself: a blank
`__bench.html` imported `/world/world-scene.js`, called `createWorldScene`
with a subclassed `WebGLRenderer`, and fed fixtures straight into
`updateMemberLounge`, `setRemotePlayers` and similar calls. That tied the bench to
the module path and the constructor signature, so the planned split of
`world-scene.js` into `world/scene/*.js` would have broken it, and it could
not measure page load. This version loads the real `/world/` page through
the dev server instead. From the recovered code it keeps the separate page
library, the Playwright and Node lookup, the no-GPU skip, `--tighten`, the
scene-audit counts and the camera hold in `updateMatrixWorld`. It uses the
YIQ distance instead of the recovered per-channel maximum. The recovered budget
formula, `baseline × factor + absolute`, is replaced by the larger of the two.

The recovered branch also held an advisory deploy step in
`tools/next_autodeploy.py`: `world_bench.py --gate` after verification,
bounded to 300 s. It is not merged. A full run takes 6–8 minutes, so the step
would report "timed out" on every World deploy. It needs a shorter deploy
profile (fewer poses, shorter settle) first.

## Operations

World is deployed with every app release
([docs/deploy/README.md](../deploy/README.md)): the release carries
`world/dist` and `world-routes.caddy`, and Caddy is reloaded when the release
switches. World API changes ship in the same release. `tools/next_deploy.py`
writes `world_deploy_status` for the "ForkMesh is deploying now" notice
([deploy/auto-deploy.md](../deploy/auto-deploy.md#world-deploy-notice)).
`world.next.forkmesh.com` 308-redirects to `world.forkmesh.com`.

## Security

- Elements get only `THREE`, validated params and an allowlisted
  same-origin `net()`; they have no other I/O.
- Chat content is encrypted by the chat client; the Office uses the same
  channel authorization as `/chat` (`www/docs/security/sealed-chat.md`).
- The local speech bridge: `www/docs/security/world-local-speech-bridge.md`.

## Traps

- Editing `world/worker.js` changes nothing live; change `caddy_routes.py` or
  `world_health.py` too.
- `world/deploy.sh` runs the old wrangler deploy (`app/deploy.sh world`); it
  does not deploy World to the core nodes.
- World migrations are numbered in `app/migrations/` and stay there; moving
  them would break the applied-migration order. They run on rqlite.
- **Mobile World health bounds follow the built World.** `/health/mobile`
  (`app/host/forkmesh_host/world_health.py`, mirrored in `world/worker.js`)
  checks file sizes in `world/dist`. Round 3 made `world-scene.js` the esbuild
  bundle (~780 KB) and folded `scene/core.js` into it, but the check still
  capped it at 64 KB and required `scene/core.js`, so /status reported
  "world-scene.js returned an unexpected byte length" (HTTP 503) for a working
  World. The bound is now 1 KB–4 MB with no scene dependency, and
  `test_world_bundle_entries.py` checks the real bundle against it.
- **A top-level World module may only import scene modules that are bundle
  entries.** The production build folds `scene/` into bundles and keeps only
  `WORLD_BUNDLE_ENTRIES` (`app/tools/build_site_assets.py`) as files. On
  2026-09-29 `world-sky.js` started importing `./scene/resources.js`, which
  was not an entry, so the live page 404ed on it and the World never started
  while every source-level test passed. `scene/resources.js` is now an entry
  (esbuild splitting keeps one shared instance), and
  `world/tests/test_world_bundle_entries.py` bundles the real tree and fails on
  any unresolved scene import.
- **`vertexColors: true` needs a `color` attribute.** A material with it
  multiplies by the geometry's per-vertex color; a geometry without one (every
  built-in `SphereGeometry`, `RingGeometry`, ...) reads (0, 0, 0) and draws
  black. Per-instance colors from `InstancedMesh.setColorAt()` need no flag.
  The sun, moon and planets (`world-sky.js`) and their glow ring drew black
  until 2026-09-29 for this reason; `test_world_sky.py` now fails on any sky
  object that sets the flag without the attribute.
- **Render budget.** `world/browser_tests/tests/world.spec.js` fails when
  walking the default town exceeds 450 draw calls or 750,000 triangles.
- **Transmission materials double the frame.** Any `transmission` material in
  view makes three.js re-render every opaque object offscreen;
  `world/tests/test_world_render_performance.py` pins it absent.
- **Changing the visible point-light count recompiles every lit shader.**
  `world-scene.js` pads the light slots with dark lights instead of toggling
  visibility.
- **`getWorldPosition()` marks every ancestor dirty**; the settled matrix walk
  in `world-scene.js` ignores that flag on auto-updating objects.
- **Initial module budget.** `initialWorldModuleBytesSoft` in
  `world/tools/build_worker_footprint.py` fails the build when the first-load
  module graph grows; load optional features lazily.
- A new HUD rule placed above the component layer at the end of `world.css`
  loses to it; add chrome styles to that layer. Some legacy chat rules use
  `!important`, so overrides of chat surfaces need it too.
- Never put `filter`, `transform` or `backdrop-filter` on an ancestor of the
  phone HUD's fixed avatar badge; it re-anchors the badge to that ancestor.
- An element given `display` in the component layer no longer honours the
  `hidden` attribute on its own; pair it with a `[hidden] { display: none; }`
  rule (the settings tabs and chat counters already do).
- **Frozen objects have `matrixAutoUpdate === false`.** The settled matrix
  walk sets it on objects that have not moved for 30 frames and restores it
  on the first real change. Do not use the flag to mean "this object is
  static", and do not switch an object to manual matrices by setting it to
  `false` and writing `object.matrix`: a frozen object's subtree is skipped
  and `matrixWorldNeedsUpdate` is not read. Move objects through
  `position`/`quaternion`/`rotation`/`scale` or call `updateMatrixWorld(true)`
  on them.
- **Out-of-view settled subtrees are `visible = false` during the main
  render.** `settledCuller.render()` hides them only inside that call and
  restores them in `finally`; code that runs during `renderer.render` (an
  `onBeforeRender`) must not read a group's `visible` to mean "shown". Anything
  that changes an object's drawn bounds without a transform, parent, geometry
  or `frustumCulled` change (a vertex shader that displaces far outside the
  geometry's bounding sphere, say) must set `frustumCulled = false`, which
  three.js needs anyway.
- **Deployed World modules are minified.** A `file.js:<offset>` in the load
  log or a stack trace from production points into minified code; map it
  with the `.map` next to the file. Module URLs carry `?v=<hash>`; a new
  relative import is versioned by the build automatically, but a module URL
  assembled at run time from pieces is not.
- **Construction-time canvases paint later.** While `createWorldScene` runs,
  `canvasTexture` queues its painter instead of calling it; the first view's
  plates paint before the reveal and the rest in idle slices. A painter whose
  result the caller reads right away (a colour, a measured size) must use
  `canvasArtTexture`, which still paints inline.
- **The main view is not drawn until the first view is compiled.**
  `firstViewHoldUntil` holds the animation loop's render (and the
  construction-time resize render) until `prepareFirstView()` returns, or
  4 s for an embedder that never calls it. A warm pass
  (`shaderCompileGuard.warm`) creates programs without drawing anything; do
  not read pixels or rely on a frame having been presented while it is held.
- **`createWorldScene` yields.** Between slices other tasks run while later
  `const`/`let` bindings of the build are still uninitialized. A listener,
  timer or promise callback created during construction must not reach
  code that reads bindings declared further down until construction ends
  (`worldBuilt`/`sceneConstructed`).
- **Parameterized geometries can be shared after 5-10 s.** Do not mutate a
  `BoxGeometry`/`PlaneGeometry`/... in place long after building it; build a
  plain `BufferGeometry` for anything animated, or give the attribute
  `DynamicDrawUsage`.

## Recent changes

- 2026-10-07: the ForkMesh coaster circles the town hexagon with a double loop, a ramp jump into a freefall onto a landing ramp, airtime hills, a corkscrew, a helix and a tunnel; the drone rings now weave over and under it; quadcopters reach top cruise speed in 1.2 s instead of 3 s (see "ForkMesh coaster", "Drone race course over the town" and "Quadcopter flight").
- 2026-10-07: World Code Workshops were removed because nobody used them. The four `world_workshop_*` tables had no rows (the 2026-09-10 database audit counted zero), and the World panel that started a workshop had already been retired, which left the analyzers and the save, share and comment calls in `world.js` unreachable. Removed: `world/src/world_workshops.py` and its `/api/world/workshops` route, the schema statements (migration 0238 drops the tables; its down migration recreates them empty), the blind-index entries in `entry.py` and `tools/next_rekey.py`, the dashboard's `?workshopSession=` agent deep link, the repo/run-scoped `#world-workshop` room in `dashboard-chat.js`, and `www/docs/world-code-workshops.md`. The media room's form layout class, which it shared with the workshop panel, is now `world-media-form`. The Security Workshop landmark in the scene is a different thing and stays.
- 2026-10-01: the system status board is back as an optional World store element, `elements/status-board.js` ("System status board", 0.05 SOL in `world/src/world_element_store.py`), instead of a built-in billboard. It reads only `/api/status/world`, declared in its manifest and disclosed before purchase, and paints the same rows as `/status`: each system's 24h and 30d uptime and its last 60 minutes, 24 hours and 30 days of checks, day colours derived from `checks`/`failures`. It refreshes on its own `refreshSeconds` (default 180, the snapshot cadence; 60–900), skips the read while the tab is hidden, and keeps the last painted board when a read fails. Nobody who has not bought it fetches status.
- 2026-09-30: post-load round 6: program links paced one per frame after the
  first view, large canvas uploads in row stripes with a shared per-frame
  upload budget, kiosk and banner repaints swapped in when uploaded, repaints
  beyond the budget deferred a frame, resumed first-view passes skip warmed
  draws, the canvas resize without forced layout, merged aquarium corals and
  sand (lobby 296 → 188 draws), a cheaper logo reflection warmed ahead, shared
  desktop-agent materials, Discord appends in slices, "Optimization
  suggestions" first in the copied log, split scene-update stalls, owned
  mirror batches and attributed unscripted long frames (see "Post-load round
  6").
- 2026-09-30: load round 5: closed HUD panels out of the first layout, HUD
  fonts preloaded, performance bar pinned during boot, sky programs compiled
  before the bake, scene and first-view slices yield to painted frames,
  budgeted first-view warm passes, resizes and figure copies wait for
  compiling programs, post-reveal work one task at a time, own contributions
  and the chat after the load, preferences written only when newer, timer
  attribution, API server time, critical-path APIs and regressions in the load
  log (see "Load speed round 5").
- 2026-09-29: the Office aquarium's floating HTML reef controls are replaced
  by a physical controller on the tank's front base: FEED, TAP and FOLLOW
  buttons, a SCHOOL dial (auto, species, colour, scatter), BACKDROP and GLASS
  switches, a LIGHT lever and a status screen, all clicked through the world
  pointer. Fish school, scatter, feed at the surface and follow or flee taps
  on the glass through a typed-array boids step (see "Office aquarium reef
  controller").
- 2026-09-29: the built-in system status board is gone from the World (port of
  branch 524): the status billboard on the hub circle, the hub-circle
  proximity watch that fetched `/api/status/world`, the `status-update` socket
  refresh, the one-second status stand timer and the admin status light in the
  top tools. It kept going stale between snapshots; system status now lives
  only on `/status`. The diagnostics dots keep their own three-minute cycle
  (`WORLD_DIAGNOSTICS_REFRESH_CYCLE_MS`). The server still publishes
  `/api/status/world` and the `status-update` frame for `/status`.
- 2026-09-29: quadcopter race mode: lit rings with the next one highlighted,
  instanced chevrons and a guide chevron, a checkered start/finish gate with
  lap timing, a local best lap with a ghost, other pilots' rings and lap times
  over the existing move frames, and a compass, crosshair and velocity marker
  HUD; the parachute sinks twice as fast (see "Drone racing" and "Parachute
  descent").
- 2026-09-29: "Vultr server" and "SSH server" in the Start your world panel
  set a node up without the desktop: point DNS, paste one script or run one
  command (see "Hexagonal city and desert plots" and
  [release/self-setup-node.md](../release/self-setup-node.md)).
- 2026-09-29: photo faces without a click, a Custom avatar ring by the fire,
  ring labels, and a sky glow fix (see "Photo faces, Custom avatar ring and
  ring labels").
- 2026-09-29: the drone race course flies only over the town hexagon (radius
  290 from the town centre, crest over the zip-line cable), and a faint
  ForkMesh mark is shaded into the town grass that fades in with camera height
  (see "Drone race course over the town" and "ForkMesh logo on the town
  grass").
- 2026-09-29: load round 2: roster and profile prefetched at boot, compact
  roster with WebP face thumbnails, interactive at reveal with data after the
  first interactive frame, sliced scene build, one canvas allocation,
  first-view programs primed, coalesced member lounge, minified and
  hash-versioned World modules with source maps and brotli (see "Load speed
  round 2").
- 2026-09-29: load round 3: first-view programs compiled by warm passes before
  the reveal (no uniform reads behind compiling programs, one light-count
  variant), construction canvases painted first-view-first, sliced Office far
  proxy, WebGL renderer created while the scene modules stream, post-load
  lounge/aquarium/seat-label work sliced, cached HUD face, per-frame hover
  picks, faster World boot endpoints on the server (see "Load speed round 3").
- 2026-09-29: load round 4: shared modules preloaded with the scene bundle,
  lighter populate, QA card repainted in place, fresh arrivals seated once the
  benches exist, deterministic node-yard hiding (see "Load speed round 4").
- 2026-09-29: the city is a rounded hexagon, the sky edge walk is removed, and
  twelve desert plots with "Start your world here" signs connect an instance
  by Cloudflare key, Vultr key or SSH key (see "Hexagonal city and desert
  plots").
- 2026-09-29: one static loading modal on a blank page (no second curtain, no
  HUD behind it) and a floating load log with optimization suggestions for
  admins and debug-panel users (see "Load experience").
- 2026-09-29: the quadcopter flies where the camera looks (mouse pan and tilt
  steer, the resting chase view flies level) and speeds up while forward is
  held (see "Quadcopter flight").
- 2026-09-29: the Office walkway is one town path to the door (the raised
  bridge and approach decks and their side strips are gone), and a build stamp
  under the Office sign shows the running version, commit and deploy age (see
  Distance detail).
- 2026-09-29: jank pass: deferred shadow-pass shader compiles, dirty-only
  avatar part and detail sync, staggered static mirror sets, idle-sliced scene
  diagnostics, quieter presence and clock ticks (see "Jank pass").
- 2026-09-29: the node-delete tests in `test_world_admin_node_delete.py`
  follow the desktop Nodes page after runners and relays became Nodes rows:
  the Actions column is also shown to non-admins when a row has a link action,
  the Delete button stays admin-only, and the status line returns early while
  a delete is in progress so its progress text stays visible.
- 2026-09-28: a World session that ended while hidden is reported as closed in
  the background and no longer counts toward safe mode (see World crash
  reports after 2026-09-24).
- 2026-09-28: group frustum culling for settled subtrees in the main render,
  and the Members Circle figures registered as a World element (see "Render
  walk: group frustum culling").
- 2026-09-28: shared resources are never disposed by one owner; avatar pins,
  rings and patches, portal parts, quadcopter rotors and repeated static
  meshes drawn through instanced mirrors; Members Circle figures and Office
  participants registered (see "Shared resources and instanced mirrors").
- 2026-09-28: issue 714 frame budget fixes: frozen subtrees in the matrix
  walk, idle geometry de-duplication, instanced System Capacity bars with an
  atlas label mesh, shared avatar ring/pin materials, lower tessellation,
  indexed logo panel (see "Frame budget fixes from the audit").
- 2026-09-28: full-width performance row under the World with square chart
  tiles, admin profiler tiles, desktop-style log counters and an OPTIMIZE
  drawer that audits the scene against three.js practice (see "Performance bar
  and optimization audit").
- 2026-09-27: changed-file landings point at the files. Each verified changed
  path is matched to the deepest size-map wedge that holds it (the file itself,
  or its nearest drawn folder when the file is new or deeper than four rings),
  on the open sunburst or the portal's miniature map. The tile dives into that
  wedge, the wedge flashes and keeps breathing with a rim ripple, and the tile
  springs out to a callout column beside the ring with a leader line back to
  its wedge; tiles land 150 ms apart and carry the wedge colour. Paths with no
  wedge keep the old spread around the portal. Reduced motion drops the
  stagger, bob and ripple. The spawn lives in `scene/runtime/effects.js`, the
  per-frame flight, settle and wedge glow in `scene/runtime/animate.js`; the
  sunburst layer (`repository-records.js`) and the miniature map
  (`repository-catalog.js`) carry their ring geometry in
  `userData.repositorySizeMap` so the landing can address a wedge.
- 2026-09-27: the phone status bar keeps every button and count inside it,
  between the logo and the avatar (see UI chrome).
- 2026-09-27 (ported 2026-09-29 from branch 504): `world/tests` is green
  again (20 failures that were already on `main`). The tests still pinned
  source that had moved. Route checks now read
  the route-dispatching `Default` class in `app/src/app_runtime.py`, not the
  `WorkerEntrypoint` wrapper in `entry.py`. Organization role checks go through
  `organization_rbac` capabilities. The `#general` toast link is handled in
  `MainWindowSettings.cpp`. The homepage redesign (2026-09-10) dropped the
  header's live World count, so that test now checks the current World and App
  navigation. The rooftop zip-line signs are the only hovering sprites the
  Office build allows. The worker footprint changes are in
  [worker-footprint](../app/worker-footprint.md#budget-bookkeeping-2026-09-27).
- 2026-09-27: tapping the avatar on a phone opens settings again. The
  expanded top-actions row had a `filter: drop-shadow`, and a filter makes an
  element the containing block for its `position: fixed` children. So the
  fixed avatar dropped 78 px into the row as soon as the rail opened, the tap
  landed on the canvas, and the click handler collapsed the rail. The phone
  layout now uses no filter there, because the links carry their own shadow.
  Browser tests: the portrait test pins `?quality=mobile-mid&adaptive=0`
  (headless Chromium's SwiftShader GPU boots `mobile-low`). The two-client
  test keeps its mock sockets from sending `welcome`, because BroadcastChannel
  peers render only while there is no server session. `members-circle`
  uses `.size` instead of the non-existent `toHaveSize` matcher.
- 2026-09-27 (ported 2026-10-06 from branch 515 onto the `scene/` modules):
  no pop-in for phone districts: compact renderers keep a district drawn
  while it is in the camera view, not only within its radius (see Distance
  detail).
- 2026-09-26: frame-rate pass on the 3D scene: compositor-only name tags,
  fewer raycasts on wheel/double click/ground height, far remote avatars
  animate at the visual cadence, cached canvas rect and enclosure walk (see
  Frame-rate pass).
- 2026-09-26: load and paint pass: no backdrop blur over the live canvas,
  cheaper canvas panel proxy, cached floor-note sprites, module preload from
  the loader, API preconnect, immutable vendor files (see Load and paint
  pass).
- 2026-09-26: the "ForkMesh is deploying now" notice works again.
  `next_deploy.py` writes `world_deploy_status`, which only the Cloudflare
  `app/deploy.sh` used to write
  ([deploy/auto-deploy.md](../deploy/auto-deploy.md#world-deploy-notice)).
- 2026-09-26: floor notes: tap the floor to leave a public note, reply in
  its thread, and tag people with `@name` to ping them (see Floor notes).
- 2026-09-26: light UI for everything except the 3D scene: light tokens and
  converted colors, a `.fm-world` component layer, SVG line icons, one fixed
  settings frame with left navigation (Work no longer widens it), a card-style
  chat with segmented channels, green offset-edge buttons with blue inputs,
  links and progress (see UI chrome).
- 2026-09-26: in-world panels painted light through the canvas adapter;
  name tags, speech bubbles, the thread window and the message toolbar
  restyled; QA "pass" uses a true green so it stays green.
- 2026-09-24: no pop-in for the Office furniture, quadcopters and mirror
  nodes; the green island disc under the Office removed (see Distance
  detail).
- 2026-09-24: `world.js` is the staged shell; mobile health also requires
  `world-runtime.js` (25c00b1).
- 2026-09-24: `/health/mobile` answered by the host (ac7c6d3).
- 2026-09-23 `7cc28ee45`: Office rim and waterfall removed; the X and Reddit
  banner boards are opt-in packages (`install.default` false) loaded only
  after a visitor installs them.
- 2026-09-23 `06e58f88e`: frames no longer wait behind shader compiles.
- 2026-09-23 `9fe13756b`: render cost overhaul (no transmission water,
  settled matrix walk, idle shader warm-up, steady light count, glass-tower
  far LOD).
- 2026-09-23 `12317aa01`: World proxies `analytics.js` and `analytics-sdk.js`
  from the App.
- 2026-09-13 `1a3315b89`: zip line from the Office roof to the beach.
- 2026-09-13 `88a29238d`: Sky Office moved into the `forkmesh-sky-office`
  package.
- 2026-09-13 `bd21e0d2f`: World adopts an App login that restores after the
  startup budget.

## Notices are inline, no toasts (2026-10-09)

The user asked for no toasts anywhere, with information shown inline where
it belongs. Until then `this.toast()` in `world.js` wrote every notice into a
floating, self-dismissing card stack at the bottom right
(`[data-world-activity-stream]`, `activityNotice`, with
`toastPriority`/`toastLockUntil`/`toastTimer` throttling) and into the chat
transcript. The card stack, its CSS and the helper are removed.

- **Feedback for an action** goes through `notice(message, { kind, at })`.
  A capture-phase listener on `document` (`recordNoticeOrigin`: click,
  submit, change, Enter/Space) remembers the element the visitor last used,
  ignoring the 3D canvas. `notice` finds the closest form, fieldset, section,
  article, aside, details or dialog around it (`WORLD_NOTICE_HOSTS`) and
  writes a `<p class="world-inline-status" data-world-inline-status
  role="status">` into it, right after the block that holds the control. The
  line is in normal flow, coloured by `data-tone` (error red, success green,
  status muted) and never times out. `at` can name an existing status element
  instead (`[role=status]`), which is then reused, or a different origin.
- **Re-rendered panels.** Most panels rebuild their markup after an action.
  At click time the host chain is also stored as unique selectors
  (`worldNoticeHostKey`: id, first `data-world-*` attribute, or
  `aria-labelledby`), so a result that arrives after the rebuild still finds
  its panel. A `MutationObserver` (only while a line is live) puts the line
  back when a host is rebuilt, and drops it when the host closes or the
  detail panel switches to another landmark (`data-open-landmark`). That is
  the "until it is replaced or the view changes" rule.
- **Actions with no panel** (avatar actions, the jetpack, swings, the gym,
  camera mode, 3D board and sign clicks, keyboard shortcuts, the tour end, a
  panel that closed on success) use `hudNotice`: one line under the island's
  action bar (`[data-world-hud-notice]`) with a dismiss button. Status and
  success lines there clear after 12 s; errors, and the "avatar moved to your
  other device" line (`sticky`), stay until dismissed or replaced. If the
  island is hidden the line goes to the chat transcript.
- **Existing status lines are used where they exist.** Sign-in prompts go to
  the account panel (`setWorldAccountStatus`, after it opens). The Store tab
  keeps its message in `storeStatus` across re-renders (`setStoreStatus`);
  Save no longer reports "Saved." after a failed save. Revoking a session
  reports on the Sessions line. Avatar upload, the ActivityPub publish and
  the mirror agent form already had their own line, so their duplicate
  notice was dropped.
- **Unsolicited notices** (new pings, World announcements, knocks,
  handshakes, home declines, reward events, member/desktop/instance
  celebrations, a new World build, a verified code landing) go to the chat
  transcript through `notifyChatArea`, which `dashboard-chat.js` renders as a
  system line. The ping and announcement caps and the join grace
  (`activityNoticesSettled`) still apply. The update notice and the Pings
  board still show their own state.
- **Office tasks** (`world-office-tasks.js`) take `notify` instead of
  `toast`. `report` writes to the Office panel's status line when the
  visitor acted there, next to the Work tab control otherwise, and to the HUD
  line for the physical task wall. The Work tab's quick-add form reports only
  on its own line (`silent` mutate).
- **Floor notes** (`world-floor-notes.js`) have no `toast` option any more.
  "Sign in to leave a note." shows in the note panel, and a deleted or
  missing note leaves the panel open with "Note deleted." / "That note was
  removed." (`showGone`).
- **QA verdicts** keep the pass confetti and the failed red frame, but the
  "QA PASSED!" card is gone; the verdict is a notice where it was given.
- **Celebrations** (new member, fresh desktop install, new federated
  instance) keep their fireworks and confetti, now `aria-hidden`, but the
  announcement cards ("Welcome <name>", "NEW FORKMESH DESKTOP", "PORTAL
  ONLINE", with their links) are gone: they popped up and removed themselves
  after 10 to 12 s. The news is a chat-feed line instead: "🎉 <name> just
  joined ForkMesh. Say hello in #general.", "🎆 A new ForkMesh desktop was
  just installed (<os · arch>).", "🎆 <label> joined the ForkMesh federation:
  <origin>/world/". `app/public/member-welcome.js` (used by `/chat` and the
  dashboard chat) does the same: `celebrateNewMember` draws only the
  particles and returns true when it put them on screen, and the caller then
  writes `NEW_MEMBER_GREETING(name)` into its own feed. The shared
  `data-forkmesh-member-welcome` marker still makes the World and the
  embedded chat celebrate once, so the greeting line is written once too.
  The desktop's own celebration is separate.
- **The `forkmesh:world-activity` message is gone.** `dashboard-chat.js`
  posted every system line to the World so it could float it as a card; with
  the card stack removed nothing listened, so `emitWorldActivity` and the
  `emit` argument of `appendSystem` were removed.
- Kept on purpose: the World update notice and the runner attention pill.
  They are persistent state with their own controls (Refresh, a link), not
  self-dismissing reports.

## UI chrome (light theme)

Everything that is not the 3D scene (HUD, panels, chat, popovers, dialogs,
inline status lines, the loading card) is a light, low-chrome UI. It is built in three
layers, all in `world/public/world/world.css` unless noted:

- **Tokens.** The two `:root` blocks (the top block and the "Primer product
  UI" block) hold light values: white surfaces, `#e4e7eb` borders, gray-900
  text, and the ForkMesh Sans (Geist) stack in `--font-sans`. The earlier
  dark literals across the file were converted to their light equivalents in
  place, so older feature rules render light without being rewritten.
- **Accent split.** Green is reserved for buttons: primary actions use
  `--button-primary-bgColor-*` with a solid offset edge
  (`box-shadow: 3px 3px 0 var(--button-primary-edge)`, pressed state
  translates by 2px); secondary buttons are white with a 2px
  `--button-secondary-edge`. Inputs, focus rings, links, sliders, toggles,
  selection states, progress bars and percentage/status readouts use blue
  (`--focus-outlineColor`, `--fgColor-accent`, `#2563eb`/`#3b82f6`).
  Semantic states keep their meaning: the online presence dot, "Done" chips
  and success status lines stay green, errors stay red.
- **Component layer.** The rules from the `.fm-world` font block to the end
  of the file restyle the HUD (status pill bar, logo button, avatar, icon
  dock, right rail), chat, settings, details, account, office and task
  surfaces. They are prefixed with `.fm-world` so they win over the older
  stacked layers above them; add new chrome rules there rather than in the
  older sections.

Panel sizes are fixed so switching views never resizes the frame:

- Settings is one centered modal (`min(1040px, 100vw - 32px)` by
  `min(740px, 100dvh - 32px)`) with a left navigation column for every tab,
  including Work. The old per-tab width (`data-active-tab="work"`) and the
  drag-to-resize grip are no longer used; the Account and Discord entries sit
  at the bottom of the navigation column, and the Discord bridge replaces the
  pane while it is open. Phones get a full-screen sheet with a scrolling tab
  row.
- Workspace-style details (Pings, Errors, repository review, issue
  workbench) open in the same centered frame; ordinary landmark details stay
  a right-hand drawer.
- The HUD logo button has no tile: since 2026-10-07 the hexagon mark fills
  the whole 48px button with no white box, border or box shadow behind it
  (the mark's own white outline and a drop shadow keep it readable over the
  scene). The presence dot stays on its lower-right corner, and keyboard
  focus draws a blue outline because there is no box shadow to change.
- The top status bar sits between the logo button and the avatar. It holds
  only the online-people count and its roster dropdown. The Prompt, pings,
  Errors and Tasks buttons were removed on 2026-09-29 (see "HUD island"
  below), and the phone rule that squeezed five items into 36px columns went
  with them.

In-world panels (signs, boards, leaderboards, kiosk screens, placards,
elevator buttons and avatar chest cards) are painted onto canvases in
`world-scene.js`, and they follow the same light theme through one adapter
rather than per-painter colors: `canvasTexture()` and
`repaintCanvasTexture()` hand each painter a proxy of its 2D context
(`worldPanelContext`). The proxy maps colors by drawing operation: text
becomes dark ink (bright accents become deep accents), filled shapes become
white, light-gray or pastel surfaces, strokes become soft borders or mid
accents, glows are dropped, the mint accent hue becomes blue, and the
"ForkMesh Mono"/"ForkMesh Favorit" fonts become "ForkMesh Sans" at 700 or
below. Art textures that must keep their colors (faces, shirts, dirt, wood,
bike lane, aquarium, globe, table logos, member engagement bands, stone
plaque, repository star, agent bot screens, avatar back names) are painted
through `canvasArtTexture()`, which marks the canvas `data-world-art` so
repaints skip the proxy as well. A new painter is light by default; call
`canvasArtTexture()` only for art. Terrain, buildings and lighting are
untouched.

The chat thread window and the image preview are `<dialog>` elements that
`app/public/dashboard-chat.js` appends to `<body>` with Tailwind classes the
World does not load, so `world.css` styles them by their data attributes
(`dialog[data-chat-thread-dialog]`, which `ensureThreadDialog()` sets). The per-message actions (Reply,
reaction, Issue, Edit, Delete) show only while the island feed is expanded:
on hover with a fine pointer, and on focus (a tap) on touch devices.

Icons are inline SVG line icons from `WORLD_UI_ICON_PATHS` / `worldIconSvg()`
in `world.js`, replacing the emoji glyphs in the dock, status bar, top tools,
rail and chat channels. The Notes panel (`world-notes.js`) styles its own
shadow DOM, and the import and team dialogs in `world.js` are styled
inline; both follow the same tokens by hand.

## Distance detail (no pop-in)

Nothing in Town is hidden or swapped for a different-looking stand-in by
camera distance; far views stay cheap by drawing the same things in fewer
calls.

- **Office.** `forkmesh-office-distance-lod` is switched by hand
  (`autoUpdate = false`) in `syncOfficeDistanceLevel()`, from the same
  decision as the compact `furnished` level: inside the Office, or Town within
  `OFFICE_EXTERIOR_LOD_DISTANCE × lodDistanceScale`, draws the full tower
  (`forkmesh-office-high-level` wraps the high group so compact residency and
  the LOD never fight over one `visible` flag). Beyond that the far tier is
  `rebuildOfficeFarProxy()`: `mergeObjectBatches()` merges every visible mesh
  of the furnished tower (all floors, furniture, landscaping) into batches
  keyed by material state. Colour, opacity, roughness, metalness and emissive
  go per vertex (a patched standard material), so ~640 meshes / ~450 draws
  become ~20 draws. Plain canvas signs share one atlas per material state
  (see "Office aquarium on demand and far sign atlas" below); signs with a
  lit material or a repeated texture keep their own.
  Textures stay shared with the high tier and are skipped by compact
  residency release. The proxy is rebuilt when an Elements toggle or object
  delete/restore marks it dirty, and in an idle slice after each near-to-far
  switch so structural changes made up close are picked up.
  `OFFICE_DISTANT_DRAW_BUDGET` bounds it.
- **Office ground.** The Office stands on the city lawn. The raised
  `#356d58` island disc (read as a green circle around the building) is gone;
  the lobby slab keeps the 0.38 deck, and landscaping sits at
  `CONTINUOUS_CITY_SURFACE_Y`.
- **Office walkway.** One path, `forkmesh-town-path-north-office`, runs from
  the Town junction to the Office door at the town-path height. It used to
  stop at the bridge, where a raised bridge deck and a separate approach deck
  with glowing side strips took over: that read as a second walkway stacked on
  the first. The garden's narrow centre strip under it is gone too. The path
  lives in the `forkmesh-office-bridge` group rather than the Town landscape
  so it stays drawn while the visitor is inside the Office.
- **Office build stamp.** A plate under the `FORKMESH OFFICE` sign shows the
  running World build: version, commit and how long ago it was deployed
  (`v0.9.3 · 5bb466e5b6a1 · updated 5m ago`). `tools/next_deploy.py` writes
  `forkmesh-world-version` and `forkmesh-world-built` (Unix seconds) beside
  the existing `forkmesh-world-deploy` commit meta; the shell reads all three
  from the page, so the plate describes the bundle actually running rather
  than the separately deployed App. Without the meta tags (local runs) it
  falls back to `/api/version` when that has been fetched, otherwise
  "build pending". The age is redrawn at most every 30 seconds and only when
  the text changes.
- **Quadcopters.** One airframe at every distance (no far clone). The static
  frame is one merged batch; the four rotors stay separate meshes so they spin.
- **Mirror nodes.** Cabinets are built from `loadFountainData()`, which the
  shell now also calls once in the post-boot idle slice. Polling still only
  runs while the visitor is at the fountain.
- Frustum culling is untouched: every batch has its own bounding sphere.
- **Phone districts.** Compact renderers (the phone tiers) still release the
  GPU buffers of districts in `COMPACT_DISTRICT_RESIDENCY` (repositories,
  leaderboards, members, Office interior and front garden, beach), but a
  district is now held by either of two conditions: the visitor is within
  its enter/exit radius, or its bounds are in the camera view. The radius
  alone let a district appear in plain sight, since the fog only starts at
  280 and the districts sit 130 from the square.
  `updateCompactDistrictView()` (`scene/districts/compact-districts.js`)
  runs right before every render in `scene/runtime/animate.js`, ahead of the
  settled-group culler: it tests each district's `Box3` (the union of its
  roots, rebuilt when a root is registered or removed or on a forced
  residency pass) against the camera frustum, grown by
  `COMPACT_DISTRICT_VIEW_ENTER_MARGIN` to come into view and
  `COMPACT_DISTRICT_VIEW_EXIT_MARGIN` to leave it (`scene/quality.js`), so a
  camera swaying at the edge does not upload and release the same buffers
  over and over. Because the check runs in the same frame as the draw, a
  district the camera turns to is drawn in the first frame it is on screen,
  never a frame later. The Office high-fidelity tower is flagged
  `compactDistantProxy`: its far view is the merged far proxy, so only the
  Office radius or the furnished level keeps it. A visibility change for a
  root the enclosure sweep has hidden goes into `enclosureHiddenWorldRoots`
  instead of `root.visible`, so a residency change never shows the World
  through a sealed room for a frame. Diagnostics `residency.active` still
  lists the districts held by radius; `residency.inView` lists the ones in
  view. The trade-off: pulling the camera back to an aerial view keeps every
  district in frame resident on a phone.
  The browser spec "a phone never draws a frame with a paged-out district on
  screen" sweeps a phone-sized camera around the square every frame and fails
  if any released root projects into view (the Office tower counts as
  covered while its far proxy stands in).
- **Office aquarium.** Built when the camera comes within the Office detail
  distance, released well beyond it on every renderer (next section).

## Office aquarium on demand and far sign atlas (2026-09-30)

**Why.** A probe of the scene at spawn on `main` counted 1,939 objects, 474
meshes and 44.9k triangles in `forkmesh-office-interior`. The lobby reef
(`forkmesh-office-marine-aquarium`) alone was 1,473 objects, 141 meshes,
26.9k triangles, 83 geometries and 3 point lights, built in every scene
construction although spawn and the town overview both draw the Office's far
tier, where the interior is not shown. Everything else in the interior is
small (elevator, doors, chairs, reception, floor shells: 30 to 125 objects
each) and is read by runtime code as fixed objects, so it stays eager.

**Residency.** `districts/office-floors.js` owns it.

- Scene construction no longer builds the tank: `build()` keeps only
  `aquariumControlLocalPosition`. The `"office-aquarium"` scene slice is gone.
- `syncOfficeAquariumResidency(cameraDistance, detailDistance)` runs from
  `syncOfficeFloorVisibility()` with the same distance the exterior detail
  switch uses (`OFFICE_EXTERIOR_LOD_DISTANCE × lodDistanceScale`). It builds
  the tank when the visitor is inside the Office or the camera is within that
  distance, the same moment the furnished tower replaces the far tier.
- Release has hysteresis: the camera must be in Town, more than
  `OFFICE_AQUARIUM_RELEASE_MARGIN` (80) units beyond the build distance, for
  `OFFICE_AQUARIUM_RELEASE_MS` (15 s) without interruption. Walking back or
  a build in flight resets the timer, so zooming or circling at the edge
  never rebuilds.
- The build runs `createOfficeMarineAquariumSliced` in 8 ms idle slices
  (50 ms idle timeout inside the Office, 500 ms in Town), applies the saved
  backdrop/light/glass toggles and the last directory roster
  (`setOfficeAquariumUsers` keeps it while the tank is absent), instances its
  repeated meshes (`instanceRepeatedMeshesWithin`, the construction-time
  `instanceRepeatedStaticMeshes` pass applied to the new subtree; without it
  the lobby drew 218 instead of 184), warms its programs off-scene with
  `warmBeforeReveal`, and only then joins the interior. Its animation
  callbacks and the reef panel's pick targets join `animated` and
  `interactive` at that point too.
- Release removes those callbacks and targets, drops the tank's static
  instanced mirrors and its sources from any wider mirror
  (`releaseStaticMirrorsWithin`), detaches it, and disposes owned geometries,
  materials, instanced buffers and textures (every texture slot except
  `envMap`, which is the scene environment). Resources from the resource
  authority are marked shared and keep their no-op `dispose`.
- `whenOfficeAquariumReady()` (scene API) starts or joins the build and
  resolves with the tank. The aquarium browser specs await it after
  `waitForWorld`. `feedOfficeAquarium()` returns `false` and
  `getOfficeAquariumState()` returns `null` while the tank is not resident;
  the frame loop, exits, lobby entry and proximity pass skip it.

**Point lights stay constant.** The tank's three point lights are hidden
before the warm-up and before it is attached, then added to
`localPointLights` and budgeted by `updateLocalPointLightBudget()` (now on
`fns`), which pads with dark slots. On release they are hidden and removed
from the set before the same budget call, in the same task. A runtime probe
(town-overview pose, build, then 15 s far) read one drawn point light before,
while resident and after release, and the program count returned from 63 to
55.

**Far proxy.** The aquarium group carries `userData.farProxySkip`, so the far
tier never includes the tank, resident or not, and never holds references to
its materials. Seen from the far tier it was a few pixels behind tinted glass.

**Far sign atlas.** `mergeObjectBatchesSteps(..., { atlasScale })` packs
plain `MeshBasicMaterial` canvas signs into one `CanvasTexture` per
material state (transparency, side, depth, blending, colour space, render
order), shelf-packed with a 4 px edge-extended gutter, at `atlasScale` of the
source resolution (`OFFICE_FAR_ATLAS_SCALE` = 0.5: the far tier starts
beyond 227 units). Colour and opacity move to a vertex colour, UVs are
remapped into the cell. A sign qualifies when its map is a canvas or
`ImageBitmap` with `flipY`, no repeat, offset or rotation, UVs inside 0..1,
no other texture slot and at most 256 vertices; at most 64 sources share one
atlas. Each cell remembers its source `texture.version`;
`syncOfficeDistanceLevel()` calls `refreshBatchAtlas()` while the far tier is
drawn, so a repainted sign (door status, build stamp) is redrawn into its
cell. `finishOfficeFarProxy()` disposes the previous proxy's atlases.

**Measured** (`tools/world_bench.py`, headless Chromium on SwiftShader on
this box, so timings are not comparable with the RX 580 budget; counts are):

| pose | draws | triangles | geometries | textures |
| --- | --- | --- | --- | --- |
| spawn | 173 → 152 | 57,696 → 46,314 | 171 → 150 | 104 → 91 |
| town-overview | 210 → 188 | 110,802 → 99,418 | 175 → 153 | 106 → 92 |
| office-lobby | 184 → 184 | 45,708 → 45,708 | 214 → 206 | 48 → 47 |
| office-engineering | 98 → 98 | 24,508 → 24,508 | 256 → 143 | 54 → 45 |

Initial load at the spawn pose, three runs each, medians (`main` →
branch): scene construction work 475 → 378 ms in 22 → 17 slices (the
aquarium element alone was 67-73 ms), first frame 1,840 → 1,688 ms, first
interactive frame 3,393 → 3,172 ms, long animation frames during load 12 →
13 (unchanged within noise).

The Office far tier went from 42 children to 20 (4 of them atlas batches).
Frames match `main` within the golden threshold (spawn 0.24 % of pixels
changed, the other poses ≤ 0.02 %). The RX 580 budget in
`world/perf-budget.json` was not re-measured here; spawn and town-overview
ceilings can be lowered with `--tighten` on that box.

## Frame-rate pass (2026-09-26)

Per-frame and per-input work in `world/public/world/world-scene.js` trimmed so
walking around Town holds frame rate. Behaviour is unchanged unless noted.

- **Name tags move on the compositor.** Town player and remote-avatar labels
  are placed by `updatePlayerScreenLabel()` with
  `transform: translate3d(x, y, 0) translate(-50%, -100%)` (the CSS
  `translate(-50%, -100%)` anchor folded in) instead of `left`/`top`, which
  forced layout for every label every 34 ms. `left`/`top` are pinned to 0
  once; the write is skipped when the rounded pixel position is unchanged.
  Remote labels farther than 60 units (`REMOTE_LABEL_MAX_DISTANCE_SQ`) from the
  visitor get `display: none` (not `hidden`, which marks guest labels). Office
  participant labels, bubbles and the aquarium panel still use
  `updateScreenLabel()` with `left`/`top`, because bubble clamping and the
  aquarium browser test read those values.
- **Wheel over record boards.** `handleWheel()` no longer raycasts every
  interactive object. The 12.5 Hz proximity pass sets
  `repositoryRecordBoardNearby` (record desk within 40 units), and only then
  is the ray cast against the record-board meshes, collected once per desk
  layer by `repositoryRecordWheelTargetList()`.
- **Double click casts once.** `handleDoubleClick()` intersects `interactive`
  once and runs each bench/swing/bike/quadcopter/gym `.find()` over the same
  hits; the bench and swing checks still come before the dash.
- **Ground height cache.** `groundHeightAt()` returns the last height when
  (x, z) moved less than 1e-3, with the same `fromY`, fallback and surface list
  (identity and length). Standing still no longer raycasts the ground every
  frame; the probe also reuses one hits array.
- **Remote avatars.** The position and turn blend factors (`Math.pow`) are
  computed once per frame. Avatars more than 45 units from the camera
  (`REMOTE_DETAIL_NEAR_DISTANCE_SQ`) or outside the view frustum run the look,
  limb and activity animation at the quality profile's `visualAnimationMs`
  cadence with the accumulated delta (`remoteAvatarDetailDelta()`); position
  and heading still ease every frame, and swing occupancy is still re-derived
  every frame.
- **Canvas rect cache.** `pointerCoordinates()` reads a cached canvas rect.
  It is invalidated by `resize()`, window scroll (capture), orientation change,
  visual-viewport scroll, and at the start of each pointer press, so an
  embedded World that moved on the page still maps taps correctly.
- **Enclosure visibility.** `syncEnclosureSceneVisibility()` reuses one keep
  set and skips the walk over World roots when the enclosure scene, its kept
  roots and `world.children` are identical to the last walk (it only re-hides
  the roots it already hid). The Office "is this exterior root costly" verdict
  (triangle count plus `Box3.setFromObject`) is cached per root until it moves
  half a unit or the enclosure scene changes.
- **Animate loop.** Directory-bot hover phases are stored in
  `robot.userData.hoverPhase` at spawn instead of hashing the id every frame.
  The three repository-portal passes (halo/orbit pulse, birth overshoot,
  sparkle motes) are one loop over the portals calling
  `animateRepositoryPortalPulse()`, `animateRepositoryPortalBirth()` and
  `animateRepositoryPortalSparkle()`.
- **Office doors.** `updateOfficeSlidingDoors()` snaps to the target once the
  opening is within 1e-4 and stops rewriting the panel positions and the
  status opacity while settled.
- **Fewer per-frame allocations.** Agent-bot status colours come from the
  `AGENT_BOT_STATUS_VISUALS` map instead of fresh object literals per bot per
  frame; `tryOfficeFloorWarpDoorway()` and `constrainOfficeInteriorWalls()`
  reuse scratch vectors instead of cloning while walking the Office; the
  aquarium panel passes a scratch vector to `updateScreenLabel()` instead of
  allocating the default one every proximity pass.

## Load and paint pass (2026-09-26)

- **No backdrop blur over the live canvas.** `backdrop-filter` makes the
  compositor re-blur the WebGL canvas under the element on every frame. The
  light-UI change put it on every player label and on the always-on HUD
  (status actions, avatar actions, the former activity stream, update notice,
  diagnostics, brand nav, online dropdown, placement and fediverse cards,
  gym and aquarium panels), where the 0.94 to 0.97 opaque backgrounds made
  the blur invisible anyway. One rule at the end of `world.css` turns it off
  there. The three translucent surfaces (avatar actions, and the status and
  avatar action buttons) go to 0.94 opacity instead. Modals and dialog
  backdrops keep their blur, since the scene is idle behind them.
- **Canvas panel proxy** (`worldPanelContext`): function properties are
  bound once per proxy instead of on every access (`moveTo`, `arcTo`,
  `measureText` and so on allocated a bound function per call). `fill`,
  `stroke` and text calls write `fillStyle`/`strokeStyle` only when the
  mapped colour changed, tracked across `save`/`restore`. A new proxy is made
  for every paint and repaint, so the tracking never outlives a canvas reset.
  `worldPanelColor` already memoized the mapping.
- **Floor notes** (`world-floor-notes.js`): notes that leave the 48-unit
  radius are hidden, not disposed. A texture stays cached (at most 48, the
  farthest hidden ones evicted first), so walking along the edge no longer
  repaints and re-uploads a 256² canvas each second. The 1 s timer
  (`syncVisibilityTick`) returns while the tab is hidden, and when the avatar
  has not moved a unit and nothing else changed. Code that changes notes still
  calls `syncVisibility()` directly. Picking (`noteAt`) casts only against
  visible sprites, because three.js raycasting does not skip invisible
  objects.
- **Module preload.** `world-loader.js` adds `modulepreload` links for
  three.module, three.core and `world-scene.js` right after its first-paint
  frame, before it imports the runtime. Before this, those downloads started
  only after the 1.3 MB runtime and its ten static imports had loaded. A
  comment in `world.js` claimed `index.html` preloaded them; it never did.
  Since the 2026-09-29 load speed pass `index.html` preloads them itself (see
  "Load speed pass" below); the loader's links are now duplicates the browser
  ignores.
- **Preconnect.** `inject_world_meta` also adds `<link rel="preconnect"
  href="<API origin>" crossorigin>`, so the session restore (250 ms budget)
  does not pay DNS and TLS to the API origin.
- **Immutable vendor files.** `www/public/_headers` ends with a
  `/world/vendor/*` rule (`Cache-Control: public, max-age=31536000,
  immutable`) after the `/world/*` `no-store` rule. Later rules override
  earlier ones (`caddy_routes.header_block`), so only the vendor folder, whose
  names carry the version (`three-0.184.0`, `satellite-js-7.1.0`), changes;
  every other `/world/*` file stays `no-store`, so a refresh never mixes
  modules from two builds. Caddy's `@world_versioned` matcher
  (`deploy/core/caddy_routes.py`) also covers `/world/vendor/*`. About 1 MB
  of three.js is no longer revalidated on every visit.

## Performance bar and optimization audit (2026-09-28)

Issue 700 ("the world is still being super laggy"). Admins had no way to see
where a frame went, so the diagnostics moved into one full-width row under the
World and gained a profiler and a scene audit.

- **Layout.** `worldPerfBarMarkup()` in `world.js` renders
  `[data-world-perf-bar]` after the `.fm-world` root. When it is shown the host
  gets `data-world-perf-layout="on"`; a `ResizeObserver` writes the bar's height
  to `--world-perf-bar-height` and `.fm-world` shrinks by that much, so the
  canvas and every HUD element sit above the row instead of under it.
  `.fm-world` also gets `contain: layout` there, which makes the
  `position: fixed` HUD pieces (avatar actions, jetpack, gym, Discord) anchor
  to the World instead of the viewport.
- **Who sees it.** Visitors with Settings → Debug → "Show debug panel" get the
  sixteen chart tiles. Accounts with `isAdmin` always get the bar plus eight
  profiler tiles (GPU, JS, RNDR, SHDW, PROG, GEO, TEX, LOAF), the log counters,
  LOG and OPTIMIZE. `syncPerfBar()` runs every diagnostics tick, so an admin
  ticket arriving later turns it on without a reload.
- **Tiles.** Each chart is a 44 px square like the desktop debug bar
  (`ResourceSparkline`): label, current value and a right-aligned area trace of
  the last 60 one-second samples (`diagnosticsChartPoints(..., { slots: 60 })`
  plus `diagnosticsChartArea()`). Colours are the existing health grades;
  values stay in ink. Hovering a tile shows the value and the 60-second
  low/high. The collapsed debug pill now shows only its 3×3 health orb.
- **Profiler** (`createWorldProfiler`, on only while an admin bar is shown).
  `animate()` in `world-scene.js` calls `worldProfiler.mark(phase)` after each
  block (housekeeping, per-frame animations, simulation, avatars and bots,
  effects, camera/sky/weather, proximity, mirror, ambient animations, DOM
  labels, matrices, render). While profiling, the scene matrix walk runs before
  `renderer.render` with `scene.matrixWorldAutoUpdate` switched off for the
  call, so it can be timed on its own. `runCallbacks()` times each
  `animated`/`frameAnimated` callback and labels it with its first assigning
  statement. A wrapper around `renderer.renderBufferDirect` (outside the shader
  compile guard) counts main-view, shadow-pass (`scene === null`) and
  other-camera draws, attributing main-view draws to the nearest registered
  World element root. `EXT_disjoint_timer_query_webgl2` times the main pass on
  the GPU where the browser exposes it; otherwise GPU reads N/A.
- **Audit** (`auditWorldScene`, `window`-free and unit-tested with plain
  objects). One walk of the scene finds: instancing candidates (same geometry
  and material drawn as separate meshes), duplicate geometries and materials,
  meshes flagged transparent at opacity 1, meshes drawn at opacity 0,
  frustum culling off, static objects with `matrixAutoUpdate`, large hidden
  subtrees, over-tessellated small shapes, non-indexed geometry, texture and
  buffer re-uploads per second (version deltas between audits), GPU resources
  outside the scene and whether that gap grows (leaks), program count, PBR
  material share, local light count, sprites, oversized and non-power-of-two
  textures and drawing-buffer size. With a profile it also flags the frame
  budget (CPU- or GPU-bound), any phase over its threshold with the heaviest
  callbacks, and shadow-pass draw counts. Findings carry evidence, why (from
  three.js practice) and the fix, with the World elements to change first.
  It runs 10 s after the bar appears, then every 60 s, and every 5 s while
  OPTIMIZE is open; each run is logged under AUDIT with its own cost, so a
  spike it causes can be told apart.
- **Log counters** (`world-debug-log.js`), like the desktop activity lights:
  ERROR (console.error, uncaught errors, rejections), WARN, JANK (seconds with
  frames over 34 ms), LONG (Long Animation Frames with the blocking script,
  function, character offset and forced layout; `longtask` fallback), INPUT,
  SHADER, GPU (geometry/texture count changes), GC (heap drops ≥ 4 MB),
  QUALITY (adaptive tier changes), SOCKET, NET (HTTP ≥ 400 from Resource
  Timing) and AUDIT. Each counter blinks on increment; clicking one opens the
  Log tab filtered to it. LOG shows the newest five lines under the bar.
- **OPTIMIZE drawer** tabs: What is wrong (ranked findings and scene
  statistics), Frame budget (one bar per phase against 16.7 ms, plus the GPU
  pass and the heaviest callbacks), Draw calls (per World element), Main-thread
  blockers (long-frame scripts aggregated) and Log. "Copy report" puts all of
  it on the clipboard as Markdown for an issue or an agent prompt. Nothing is
  transmitted; crash reports still pick only their named fields.
- **Tests.** `world/tests/test_world_profiler.py` runs the audit, profiler and
  log in Node against a synthetic scene; `test_world_orb_hud.py` covers the tile
  table and layout.

## Frame budget fixes from the audit (issue 714, 2026-09-28)

The admin OPTIMIZE drawer reported CPU-bound frames (17.9 ms of 16.7):
`renderer.render` 9.6 ms and the scene matrix walk 7.9 ms, with 10k of 10k
objects auto-updating their matrices, 972 duplicate geometries, 658 duplicate
materials, 27 over-tessellated shapes and one large non-indexed extrusion.
A headless run against production data reproduced it (10.2k objects, ~1.76k
draw calls, idle matrix walk 8.6-17 ms).

- **Frozen subtrees in the settled matrix walk** (`settleMatrixWorld`,
  `installSettledMatrixUpdate`). The walk already skipped the arithmetic for
  unmoved objects but still visited all 10k of them every frame. An object
  whose transform has not changed for 30 walks is now frozen: its
  `matrixAutoUpdate` is set to `false`, its `position` and `scale` get
  enumerable `x/y/z` accessors that write through to hidden fields, and its
  `quaternion` and `rotation` change callbacks are wrapped. Any real change
  (a different value, not a same-value write) thaws the object, sets
  `matrixAutoUpdate` back to `true` and clears the "settled subtree" mark up
  the parent chain. A subtree whose root and descendants are all frozen is
  skipped entirely, so an idle town walks only the scene root's children
  (0.4 ms instead of 17 ms in the headless run). `Object3D.add` clears the
  new parent's chain, `remove` forgets the child's settled parent (so a
  re-added child is recomputed), and `copy`/`clone` of a frozen object gives
  the copy `matrixAutoUpdate = true`. The accessors keep `{ ...position }`
  and `JSON.stringify(position)` returning `x/y/z`, and `clone()` still
  returns a plain `Vector3`. Cameras and skinned meshes are never frozen.
- **One shared geometry per identical shape** (`createGeometryDeduper`,
  swept from `scheduleGeometryDedupe` every 5 s in idle time with a 2-8 ms
  budget). Only built-in parameterized geometries with static, non-instanced,
  non-morph attributes are considered. Each is hashed over its actual index,
  attribute bytes, groups and draw range (so a translated box is not the same
  as an untranslated one), must keep the same attribute versions across two
  sweeps (anything animated in place, like the aquarium water surface, is
  marked dynamic and left alone), and is compared byte for byte before a
  mesh is pointed at the canonical copy. The canonical copy is marked
  `forkmeshSharedResource`, and orphans no mesh in the scene still uses are
  disposed. The headless run went from 140 duplicate shape groups to 11.
- **System Capacity tables** (`updateSystemCapacity`, admin data on the
  Infrastructure floor). Up to 256 tables each had their own box geometry,
  material and a 768x768 canvas label (about 600 MB of textures at 256
  tables): 512 draws. Bars are now one `InstancedMesh` per colour tier over a
  unit box scaled per table, with an invisible `raycastProxy` mesh per table
  that still carries `tableName`/`rowCount` for the click. Labels are painted
  into one atlas (cells of 128-512 px, painted at full resolution because the renderer-wide canvas reduction would leave 128 px cells unreadable) and drawn as one merged mesh named
  `system-capacity-row-counts`. At most four draws.
- **Avatar ring and pin materials.** Every avatar had its own activity-ring
  and verified-pin material (and its own pin texture). They now come from
  `avatarRingMaterial(THREE, color, phaseSlot)` and
  `avatarPinMaterial(THREE, verified)`. Pulsing rings share one material per
  colour and one of four pulse phases (quarter turns), so a crowd still does
  not blink in unison; a still ring uses phase slot -1.
  `syncAvatarActivity` now takes `THREE`. Achievement patches share one plane
  through `cloneSharedPlane`.
- **Tessellation.** Small shapes with more than 48 segments were lowered
  (avatar activity ring 64 -> 40, fountain rings, core orbits, beach portal
  torus, fire glow, Office table logo, fountain foundation). Campfire engagement
  band rings and rims scale their segments with radius (8 per unit, 48-96).
  The audit's segment rule now only flags shapes under 6 world units across,
  matching its "small shapes" title; large ground discs keep 96 segments.
- **Indexed logo panel.** `indexGeometry(THREE, geometry)` merges identical
  vertices (all attributes, rounded to 1e-5) into an indexed geometry and
  keeps material groups; the Office FM cube's extruded logo panel uses it.
- **Not changed.** MeshStandard -> MeshLambert for matte decor was left
  alone: the GPU pass was 0.5 ms, and Lambert ignores `scene.environment`, so
  the swap would darken image-lit scenery. The 141 shader programs are mostly
  real variants; the idle warm-up already compiles them off the critical
  path. Per-person shirt, face, chest card and name-board textures still cost
  one draw each per avatar.

## Render walk: group frustum culling (2026-09-28)

After issue 714 froze the matrix walk (0.6 ms), the admin profile showed
`renderer.render` at 12.44 ms per frame (worst 26.4 ms), CPU-bound (GPU
1.04 ms). three.js visits every visible object each frame (`projectObject`),
frustum-tests every drawable one by one and sorts what is left. A headless run
with a populated town (10.4k objects: 160 member figures, 64 away visitors, 60
repositories) visits 6.7k visible objects per frame, of which 2.96k are
drawables and only 1.57k are in view.

- **`createSettledGroupCuller(THREE, scene)`** (`world-scene.js`, next to
  `installSettledMatrixUpdate`). Once a second (`SETTLED_CULL_REFRESH_MS`,
  after the matrix walk, inside the "matrices" profiler phase) it collects the
  largest settled subtrees (root and every descendant frozen by the matrix
  walk) whose ancestors up to the scene are frozen too, and gives each one with
  at least `SETTLED_CULL_MIN_DRAWABLES` drawables a world bounding sphere: the
  union of each member's `geometry.boundingSphere` in its `matrixWorld`, the
  same sphere three.js tests per object. For the main view the animate loop
  now calls `scene.updateMatrixWorld()` and then `settledCuller.render()`,
  which sets `visible = false` on the root of every group whose sphere is
  outside the camera frustum, renders, and restores `visible` in a `finally`.
  Because every member sphere sits inside the group sphere, a hidden group only
  holds objects three.js would have culled itself: the draw list is the same,
  in the same order.
- **What keeps a sphere honest.** A group is skipped (and rebuilt at the next
  refresh) if its root is no longer marked settled, if any clear of that mark
  happened since it was built (`SETTLED_REVISION`, bumped by
  `invalidateSettledBranch` and whenever the walk unmarks a subtree), if any
  ancestor thawed or was refrozen (`SETTLED_EPOCH`, stamped by
  `freezeSettledObject`), if the root changed parent, or if a member's
  geometry, geometry bounding sphere or `frustumCulled` changed. A thaw happens
  on the write itself (the frozen objects' observed `position`/`scale`
  accessors and rotation callbacks), so a member or ancestor that moves this
  frame is never hidden with a stale sphere. `Object3D.add` clears the mark,
  so a new child disables the group until the next refresh.
- **Never grouped:** subtrees with lights (a hidden light changes the light
  count and recompiles every lit shader), cameras, sprites (their sphere
  depends on `center` and the camera), skinned, instanced or batched meshes
  (instance bounds change without a transform), and objects with
  `frustumCulled = false`. Moving avatars are not grouped either: their roots
  thaw as they walk.
- **Shadow frames render everything.** When `shadowMap.needsUpdate` (or
  `autoUpdate`) is set, the same `renderer.render` call also draws the shadow
  map, and an object outside the camera can still cast a shadow into view, so
  nothing is hidden on those frames.
- **Measured** (headless Chromium on llvmpipe, draws replaced by a no-op so
  only the CPU walk is timed, median of 7×20 renders): spawn view 12.8 ->
  10.0 ms, campfire close-up 6.4 -> 3.0 ms, town overview 6.2 -> 1.6 ms,
  looking out to sea 4.1 -> 0.7 ms. A sampling profile of 200 renders went
  from 11.7 to 6.4 ms per render. 235 groups cover 2.95k drawables; at the
  spawn view 75 groups (1.1k drawables) are skipped. Draw lists were compared
  object by object and in order for 34 camera poses: identical. Every object's
  `matrixWorld` was also checked against a fresh composition after each of 945
  frames of walking, Office entry, travel and a quadcopter ride: no mismatch.
- **Looked at and left alone.** Shadow casters: 3.07k meshes have
  `castShadow`, but the sun's 180-unit, 512 px shadow camera covers about 760
  of them (124 smaller than half a shadow texel), and the map is rebuilt only
  every 10 s when input is idle, about 2.8 ms of CPU per rebuild; dropping the
  tiny casters would change the image for almost no average gain.
  `renderer.sortObjects` stays on: 449 of the drawn objects are transparent
  and need back-to-front order, and turning the sort off changes opaque
  overdraw order too (sort is about 6% of the walk). A dirty flag for the
  shadow map would miss visibility and material changes, which also move
  shadows. The next largest items in the walk are the avatar part-instance
  `sync` in `scene.onBeforeRender` (about 6%, it re-writes every instance
  matrix each render) and the in-view avatar rig groups.
- **Unregistered objects.** The profiler's 6.1k "Unregistered objects" were the
  Members Circle figures (`updateMemberLounge`): about 28 objects per member.
  They are now registered as the `member-figures` element ("Member circle
  figures") with a liveness probe, so audits and draw attribution name them.

Tests: `world/tests/test_world_render_culling.py` (real three.js in Node:
out-of-view groups hidden and restored, never hiding an in-view mesh after a
member or ancestor moves, a geometry swap or an added child; shadow frames
hide nothing; lights, sprites and instanced meshes are never grouped; source
contracts for the render path and the element registration).
## Shared resources and instanced mirrors (2026-09-28)

Follows "Frame budget fixes from the audit" and reuses its geometry deduper
(`createGeometryDeduper`) and `indexGeometry`; there is one of each.
`world/public/world/world-resource-cache.js` is window-free and takes `THREE`
as an argument.

- **Shared means never disposed.** `markSharedResource()` sets
  `userData.forkmeshSharedResource` (the flag `disposeOwnedGeometry`,
  `disposeOwnedMaterial`, `disposeObject3D` and compact-district release read)
  and replaces the object's own `dispose` with a no-op. The ~40 direct
  `child.geometry?.dispose?.()` paths (layer rebuilds, cabinet and avatar
  removal, element toggles) therefore cannot free a buffer another mesh still
  draws. The deduper's canonical geometries now go through it too.
- **`sharedGeometry(THREE, type, ...params)` / `sharedMaterial(THREE, type,
  parameters)` / `makeSharedMaterial(THREE, color, options)`** cache by
  constructor and normalized parameters (numbers to 1e-6, colours as hex,
  textures by uuid; object parameters are never cached; 2048/1024 entries).
  Used only where the result is never changed per object: Office patio
  chairs, tables, desks, racks, lobby chairs, aquarium rock, fish eyes,
  leaderboard and arrival stat signs, ground plaques, billboard spinners,
  campfire stones, benches, beach portals and car wheels, SOL fountain rings,
  System Capacity plinths. Kept unique because they are changed per object:
  avatar skin and shirts, campfire coals and logs, node cabinets, doorway
  frames and elevator buttons (`frameMaterial.color.set`), portal sparkles.
- **Instanced mirrors** (`createInstancedMirrorSet(THREE, parent)`). Each
  source mesh stays in the scene with its own material, `userData` and
  transform, but is moved to layer `INSTANCED_SOURCE_LAYER` (30), which the
  camera and shadow cameras do not render. The World raycaster and the ground
  probe enable that layer, so clicks, avatar selection and ground
  height still hit the original mesh. `sync()` runs from
  `scene.onBeforeRender` (after the renderer's matrix walk) and draws every
  visible source as one `InstancedMesh` per geometry+material under `parent`,
  re-bucketing each sync, so `ring.material = avatarRingMaterial(...)`,
  `pin.material = avatarPinMaterial(...)` and the deduper swapping a geometry
  keep working. Batches have `raycast` disabled and `userData.instancedMirror`,
  and `mergeObjectBatches` skips them (the Office far proxy bakes the sources
  instead). A matrix is uploaded only when it changed; static sets
  (`interval`) re-check every 20 frames, and deletions, restores and element
  toggles call `invalidateStaticMirrors()`.
- **Where.** The avatar crowd batch (`createAvatarPartInstances`) now also
  mirrors every avatar's verified pin, activity ring and achievement patches
  (`details`, one draw per material instead of one per avatar; this was the
  audit's 212 circles, 132 + 16/14/14/13 rings and 14 planes). Repository
  portal discs, outlines, bases and expanded rings, and the quadcopter rotors
  have their own sets. `instanceRepeatedStaticMeshes()` runs after
  construction and after each complete dedupe sweep over the Office tower and
  interior, Office landscaping, the leaderboard district, the arrival box and
  the campfire: `collectRepeatedMeshes()` groups opaque meshes that share
  geometry and material (at least 4, avatars skipped), and each group is
  mirrored under its nearest common ancestor (patio chair legs, seats and
  backs, aquarium rock and starfish, sign posts and frames, campfire stones,
  landscaping rocks, racks).
- **Instanced items** (`createInstancedItems`, `resolveInstancedHits`) map an
  `InstancedMesh` hit's `instanceId` to a stand-in `Object3D` with the item's
  name and `userData` (parent = the instanced mesh). Press, click and
  double-click resolve hits through it. System Capacity keeps the raycast
  proxies from issue 714.
- **Registered roots.** Members Circle figures (`member-circle-figures`),
  Office meeting participants (`office-participants`) and the crowd batch
  group (`avatar-crowd-batches`) are now World elements, so the audit names
  them instead of "Unregistered".
- **Audit.** Objects not on layer 0, and meshes whose materials are all
  `visible: false`, are not counted as drawables. `getInstancedMirrorStats()`
  on the scene API lists every set with draws, instances and average sync ms.
- **Not done.** No MeshLambert swap: it adds shader programs, and the program
  count was the concern. Faces, chest cards and name boards keep per-avatar
  textures (one draw each).

Measured with the headless harness (real GPU, `--use-gl=angle
--use-angle=gl-egl`; 160 Members Circle figures, 48 remote visitors, 17
repositories and 256 capacity tables fed in; audit after 20 s), main at
9fd809b0f4 against this branch:

| | main | branch |
| --- | --- | --- |
| instancing candidates (draws) | 601 | 30 |
| visible drawables | 3,017 | 2,295 |
| town overview draws | 809 | 648 |
| town (0, 60, 90) draws / triangles | 734 / 139k | 627 / 168k |
| Members Circle view draws | 416 | 372 |
| portal ring view draws | 232 | 192 |
| Office interior views draws | 129 / 75 / 381 | 128 / 69 / 301 |
| geometry objects | 636 | 583 |
| duplicate geometries | 60 | 12 |
| material objects | 1,550 | 1,381 |
| duplicate materials | 250 | 82 |
| shader programs | 105-125 | 129 |

Triangles rise because a batch is culled as a whole. `renderer.render` on this
box did not change measurably: the town view took 22.7-24.7 ms (median of 60)
on main and 22.2-23.7 ms on the branch; `scene.onBeforeRender` rose from about
1.3 ms to 2.5-2.9 ms, of which the mirror syncs themselves measure 0.35 ms (the
rest is timing noise between runs). The harness box's per-draw cost is low,
so the saving should be larger on the reporting machine, where the render was
draw-bound. Frames from the same poses match main (portal, Office, capacity:
0 pixels off by more than 24/255; town 63, all animated objects).

## Jank pass: long frames and periodic spikes (2026-09-29)

The live page measured 46 fps with p90 33.4 ms, p99 66.6 ms and 28 long
animation frames in 25 s. The average frame was fine; the hitches came from
work that ran all at once on some frames. They were found with the Long
Animation Frame API (script attribution), a CDP CPU profile, per-frame
profiler phases and a wrapper around the WebGL calls that can block.

- **Shadow-pass shader compiles.** The first shadow-map refresh after a new
  depth variant appears (instanced tree crowns, quadcopter rotors, avatar
  batches) compiled that program synchronously: three.js reads its uniforms
  on first use, and that read waits for the link. Single frames of 0.3 to
  1.4 s were traced to `getProgramParameter` under `getUniforms` in the
  shadow pass. The compile guard (`installShaderCompileGuard`) now defers
  shadow-pass draws too (`drawScene === null`) and counts them in
  `deferredShadowDraws`. When a refresh deferred any draw, the animate loop
  schedules the next refresh `SHADOW_MAP_DEFERRED_RETRY_MS` (400 ms) later
  instead of 10 s, and skips the stall cooldown for it. A caster whose depth
  program is still compiling is missing from the shadow map for that
  fraction of a second; before, the whole page froze.
- **Avatar part instances only write what changed** (`sync` in
  `createAvatarPartInstances`, which runs in `scene.onBeforeRender`). Each
  batch remembers which rig node sits in each slot and the last bulb colour
  string. An avatar whose root `matrixWorld` equals the last sync and whose
  subtree the matrix walk marks settled skips `setMatrixAt`; only the slot
  range that changed is uploaded (`addUpdateRange`), and nothing is uploaded
  when nothing changed. Colours are parsed only when the string changes. The
  avatar detail mirror (pins, rings, patches) is skipped when every visible
  avatar is still and the list of detail sources, their materials,
  geometries and shadow flags are the same as last time. Only the per-frame
  update path changed; batch creation is untouched.
- **Static mirror sets are staggered.** The idle geometry pass creates one
  `createInstancedMirrorSet` per shared ancestor with `interval: 20`; they
  all started on the same frame count, so every twentieth frame paid for all
  of them. Each set now gets `phase: staticMirrorSets.size`, so they spread
  across the 20 frames. The repository portal mirror (360 sources, synced
  every frame, about 0.43 ms) is left as is: its sources animate and a
  visibility change must show on the same frame.
- **Scene diagnostics walk in idle slices.** `getDiagnostics` recomputed the
  scene complexity (a walk of all ~10k objects plus `listWorldElements()`)
  synchronously every 5 s, and the crash guard polls it every second even
  when no debug surface is open: 25-70 ms every 5 s. The walk is now the
  generator `sceneComplexitySteps()`, advanced in `requestIdleCallback`
  slices of 400 objects while more than 2 ms of idle time remains; the first
  call still computes synchronously. The heaviest-elements list is refreshed
  at most once a minute (`DIAGNOSTICS_TOP_ELEMENTS_MS`).
- **Presence tick.** The 5 s same-device presence tick called
  `pruneLocalPeers()`, which re-rendered every peer (`renderPeers`: remote
  players, faces, the member lounge, capacity metrics, the roster) even when
  nothing was pruned: 9-15 ms each time. The tick now passes
  `onlyIfChanged: true`; a BroadcastChannel message still re-renders.
- **Local time badge.** The 1 s tick built a new `Intl.DateTimeFormat` and
  reassigned the composer avatar `src` and the badge title every second; the
  formatter is cached and unchanged values are not written.

Measured headless on the RX 580 with a populated town (160 members, 64 away
visitors, 60 repositories), main against this branch in the same session:

| Item | main | branch |
| --- | --- | --- |
| `scene.onBeforeRender` (part sync + mirrors), avg / p99 / max | 1.67 / 1.9 / 2.1 ms | 1.10 / 1.3 / 1.4 ms |
| Presence tick | 9-15 ms | 0-0.1 ms |
| `getDiagnostics` when stats are stale | up to 26-71 ms | 0.2-0.5 ms (walk moved to idle slices) |
| Local-time tick | 0.5-1.3 ms | 0.4-0.5 ms |
| Blocking `getProgramParameter` time in the first ~60 s (two runs) | 13.7 s, 10.2 s | 5.6 s, 8.1 s |

The whole-frame numbers (fps, p90/p99, long frames) did not separate the two
builds on this machine: the box ran at a load average of 37-52, so every
frame was CPU-starved (22 fps, p50 33-50 ms on both). The periodic timers
(diagnostics/crash guard 37-56 ms, presence 15-20 ms) no longer show up in the
branch's long-animation-frame attribution. Re-measure on an idle machine with
the coordinator's `worldperf.cjs`.

Tests: `world/tests/test_world_jank.py` (shadow deferral in the guard, the
retry wiring, staggered mirror sets, idle-sliced diagnostics, the presence
tick) and `world/tests/test_world_avatar_part_sync.py` (real three.js: a
settled crowd writes and uploads nothing, only a moved avatar's slots are
rewritten and uploaded, hidden parts and colour changes stay exact, avatar
details rest when still and follow a move).

## Hexagonal city and desert plots

- **City edge.** The city lawn (`forkmesh-continuous-city-land`) is a
  hexagon with rounded corners instead of a circle: apothem
  `CONTINUOUS_CITY_RADIUS` (365), so everything that fit the old circle
  still fits, and corner radius `CONTINUOUS_CITY_CORNER_RADIUS` (70). Flat
  edges face east and west. `roundedHexagonGeometry()` builds the fan
  (the mainland horizon proxy uses the same shape with one segment per
  corner), and `cityWalkSurfaceContains()` uses
  `roundedHexagonContains()`, the exact signed distance to the same shape,
  so the drawn edge and the walkable edge match.
- **Edge walk removed.** The eastern "World edge walk" (two white line
  rails running into the sky with no ground under them) is gone, with its
  walk surface and the `void-walk-rails` element.
- **Desert hex ring (2026-09-29).** Six desert hexagons (`DESERT_HEXES`)
  the same size and shape as the city (apothem 365, corner 70, flat edges
  east/west) tile around it, one across each city edge, at centre distance
  `WORLD_HEX_PITCH` = 2 × 365 + `WORLD_ROAD_WIDTH` (10). With the same
  orientation, neighbours across an edge are exactly one pitch apart, so the
  seven hexes tessellate and every pair of neighbours is separated by one
  10-unit road. The sand is still one mesh (`forkmesh-desert-plot-sand`, 16
  segments per corner to match the city outline) plus two instanced cactus
  meshes: the 36 original cacti and 48 more scattered over the open desert,
  kept off the inner side where the signs and the beach zip-line anchor are.
- **Plot slots kept.** `DESERT_PLOTS` still holds twelve slots, two per city
  edge in the same side-major order (so `forkmesh-start-world-sign-1` … `-12`
  keep their ids), at the same positions relative to the city edge as the old
  small plots, moved outward by the road width. They now sit inside the big
  hexes; each slot's three cacti and its sign are placed exactly as before.
- **Roads around every hexagon.** The circular bike lane inside the town is
  gone. `createHexRoadNetwork()` builds the network in three draws: the
  asphalt (`forkmesh-world-bike-lane`, one merged mesh: a 10-unit strip
  around each of the seven hexes, the rounded hex offset outward by the road
  width, plus a triangle at each of the six inner three-way junctions where
  the rounded corners leave a hole), the edge lines
  (`forkmesh-world-road-edge-lines`, one merged mesh) and the centre dashes
  (`forkmesh-world-bike-center-groove`, one `InstancedMesh`, deduplicated
  where two hexes share a road and left out of the junctions). The asphalt
  never overlaps a hex top, so there is no coplanar z-fighting; the markings
  use polygon offset. The strips overlap each other only in the gaps, where
  they have the same colour. The element id stays `bike-lane`.
- **Bike loop.** The bikes ride the road ring around the city: the rounded
  hexagon of apothem 370 and corner 75 (the road centreline).
  `roundedHexagonLoopPoint(distance)` gives position and heading by arc
  length (straight edges and exact corner arcs, so turns are smooth), and
  `roundedHexagonLoopLength()` is 2,515 units. Parked bikes are placed by
  loop distance; riding follows the road graph (see "Bike junction
  steering" below), whose default route is this loop.
- **Walkable area.** `desertWalkSurfaceContains()` is the union of the seven
  hexes grown by the road width (the hexes plus their roads) and a 16-unit
  disc at each inner junction. The outer road edge is the World's edge.
  `WORLD_RADIUS` (the ±box for remote avatars, spawns and flight) is 1200
  (was 620) to cover the ring, whose farthest road edge is about 1135 from
  the city centre; the saved-view check in `app/src/entry.py` uses the same
  bound. The relay's own limit (`WORLD_COORD_LIMIT`, 16000) is unchanged.
  Since 2026-10-13 `WORLD_RADIUS` is 1900, to cover the second ring of instance hexagons
  (see "Instance hexagons"); the saved-view check moved with it.
- **Landmarks.** The beach, its road and the zip line did not move: the beach
  is a separate scene that hides the town, so it overlapping the east desert
  hex is invisible; the zip line's beach anchor stands on the east hex's
  sand. The mainland horizon proxy used by remote districts still draws only
  the city hexagon (its 120-triangle budget has no room for six more).
- **Measured** (headless Chromium, RX 580 via ANGLE GL, same camera poses,
  animation loop stopped): overview of the whole ring 2,134 → 2,135 draws,
  491.9k → 503.3k triangles (the road strips, dashes and 48 cacti); junction
  close-up 28 → 31 draws; desert close-up 27 → 30 draws. The circular lane's
  textured material is gone, so the road adds no shader program.
- **Performance gate.** `tools/world_bench.py` saw the same change at every
  pose that draws the outdoor World (the Office poses keep the exterior):
  +1 draw (three road draws replace the lane and groove) and +11,206
  triangles (about 5k for the 48 extra cacti, 3.4k for the dashes, 2.9k for
  the road and edge-line strips), with the beach unchanged. The goldens for
  town-overview, office-lobby, office-rooftop, office-engineering and
  repository-portals changed (desert now fills the horizon; the lane ring is
  gone) and were re-blessed; `world/perf-budget.json` was rewritten from two
  RX 580 runs. A second run in the same `--runs 2` call fell back to
  llvmpipe, so the budget was rebuilt with `--report` from the two GPU
  reports only.
- **Start your world here.** Each plot has a sign facing the city. Clicking
  it (`start-world-sign`, `onStartWorldSignSelect`) calls
  `openStartInstance()` in `world.js`, which opens the "Start your own
  instance" page (`<site>/start?plot=<n>`) in a new tab: the same form as the
  homepage's, where ForkMesh's servers set up the visitor's own instance on
  their VPS ([INSTANCES.md](../INSTANCES.md)). Since 2026-10-07 the World has
  no setup panel of its own: the "Connect your instance" panel (Cloudflare
  key, Vultr Cloud-Init script, SSH command) and the Cloudflare instance
  launcher it led to were removed with their CSS. The treasury's **Launch
  mirror** button (`launchVultrMirrorFromWorld()`) is a mirror node, not an
  instance, and stays.
  The sign's texture (`startWorldSignTexture` in `scene/town.js`) now reads
  "YOUR OWN INSTANCE · YOUR NAME" and "CLICK TO START" instead of
  "CLOUDFLARE · VULTR · SSH KEY" and "CLICK TO CONNECT", and each sign board
  carries its plot number (`userData.plot`, `scene/desert-plots.js`).
## Load experience: one loader, blank first page, load log (2026-09-29)

**One loader.** In a deployed build the World showed two loaders. The staged
entry shell (`world-loader.js`, served as `/world/world.js`) painted a small
curtain at 4 %, and ~0.3-1 s later the runtime's `worldTemplate()` replaced the
host's markup with a second, taller curtain (the step ledger) at 8-28 %. Behind
it the whole HUD was already drawn, and from the first frame the scene showed
through as well. Headless screencasts of `main` show the swap. Now the one
loading modal is static markup in `index.html`, outside `<forkmesh-world>`:

- `[data-world-boot]` is a fixed, opaque, page-coloured backdrop holding the
  one `[data-world-loading]` card: the stage line (`role="status"`), the bar
  (`role="progressbar"` with `aria-valuenow`/`aria-valuetext`), the percentage,
  the seven ledger rows and the elapsed counter. The rows are static
  (`test_world_load_experience.py` checks that they match `WORLD_BOOT_STEPS`),
  so the card keeps its shape from first paint to the end.
- Its styles are inline in `index.html`. `world.css` loads without blocking and
  arrives later, so the old curtain rules in it would have restyled the card
  mid-load. They are deleted, and `world.css` no longer mentions
  `.world-loading-curtain`.
- An HTML parser can paint a partly parsed document. The card stays
  `visibility: hidden` until the inline script after the log panel sets
  `data-parsed="true"`, so the card is never painted without its ledger rows.
- The loader no longer creates markup. It sets the static card to 6 %
  ("Downloading the World runtime…") and starts `import("./world-boot.js")`
  alongside the runtime import. The runtime writes progress into the same
  card: `bootCurtain()` looks it up in the document, not in the host.
  `paintBootStep` and `renderBootSteps` read the rows the same way.

**Blank first page.** `<forkmesh-world>` ships with `data-world-booting inert`.
The inline CSS sets it to `opacity: 0` (children's `visibility: visible`
overrides cannot undo that), and `inert` keeps the half-built HUD away from
focus and assistive tech. At 100 % `revealWorld()` fades the backdrop, removes
both attributes (the World fades in over 280 ms), then `removeBootOverlay()`
drops the backdrop, `WORLD_BOOT.interactive()` is recorded and
`forkmesh:world-interactive` fires. The WebGL fallback, the loader's
runtime-import failure and the 10 s watchdog in `index.html` all remove
`[data-world-boot]` as well. The site-footer band embeds `/world/` in an iframe,
so it uses the same shell. No other page mounts `<forkmesh-world>`.

**Load log** (`world-boot.js`, one instance per page on
`window.__forkmeshWorldBoot`, installed by whichever of loader and runtime runs
first):

- It records from time zero. Navigation timing (HTML received, domInteractive)
  and the buffered `paint`, `resource` and `long-animation-frame` entries are
  read back when the module starts, so nothing before it is lost. Each resource
  line shows its path, size, how it was served (`cache` when `transferSize` is
  0, `revalidated` when the transfer is smaller than the body, otherwise
  `network`) and its duration. Resources are classed as module, API (with HTTP
  status), texture, font or style. Long frames reuse
  `describeLongAnimationFrame` from `world-debug-log.js` for their attribution.
- The runtime adds `performance.mark`/`measure` pairs (`world:<step>`) for every
  ledger step through `startBootStep`/`finishBootStep`. It also logs progress
  changes, the three.js and scene modules resolving, the WebGL context and GPU
  string (`rendererGpu()`), the first rendered frame and the first frame after
  the loader closes. Those two frames are found by watching
  `renderer.info.render.frame` from `requestAnimationFrame`, and only until the
  second one arrives.
- Scene construction per element: `createWorldScene` takes `bootTrace`, and
  `registerWorldElement` reports each registration. The time since the previous
  registration is charged to the element (the first element also carries the
  renderer setup), with its object and mesh counts. Elements under 4 ms are
  summed into one line.
- Shader compiles: the compile guard counts programs and times each program's
  first `getUniforms()` call, where three.js waits on the link.
  `shaderCompileStats()` exposes this, and it is logged when load settles.
- 1.5 s after the first interactive frame the load settles: every
  PerformanceObserver disconnects, the rAF watch has already stopped, and
  `computeWorldLoadSuggestions()` ranks what was measured. That covers the
  slowest foreground phases (the background `session` and `data` steps are
  skipped), the heaviest scene elements, large and uncached downloads,
  revalidations, request count, the slowest API call, long frames, shader
  totals, a slow reveal frame and a long gap from first frame to interactive.
  The existing `auditWorldScene` (via `world.auditScene()`) adds its
  high/caution findings. The audit walks the whole scene, so it runs in an idle
  callback, and only while the panel is visible.
- Cost: nothing runs per frame after load. The buffer is capped at 400 lines,
  and DOM writes are batched into one rAF flush. The list is a
  `column-reverse` flex box with the newest line first in the DOM, so it stays
  pinned to the bottom without reading `scrollHeight` (that forced layout cost
  up to 100 ms per flush while the scene built). A hidden panel does no DOM
  work at all.

**Visibility** (`worldBootLogVisible`): the panel is shown to accounts whose
ticket says `isAdmin` and to anyone with Settings → Debug → "Show debug panel".
For everyone else it is recorded but hidden. Turning the debug panel on later
shows the full log and the suggestions, and `window.__forkmeshWorldBoot.text()`
returns the log as text. The panel is static markup and starts hidden. An
inline script unhides it at first paint when the stored settings have
`debugPanel` or when `forkmesh.world.bootlog.v1` remembers that the last
resolved ticket was an admin one. After that, `syncBootLogAccess()` (on ticket
resolution and on every `syncPerfBar`) is the authority, so a non-admin ticket
hides it again. All storage access is wrapped in try/catch. Collapse persists
in `forkmesh.world.bootlog.v1`. Dismiss turns the panel into a small "Load log ·
N s" pill, and clicking the pill brings it back. "Copy log" copies the
timestamped text and the suggestions. The panel is `role="log"`
`aria-live="polite"`, sits bottom-left above the performance bar
(`--world-boot-log-offset` on `<html>` follows the bar height), and stays
after load until dismissed.

**Hooks in `world-scene.js`** (kept small for the module split): the
`bootTrace = null` parameter of `createWorldScene`; the first line of
`registerWorldElement`, `bootTrace?.sceneElement?.(id, label, roots);`;
`compiledPrograms`/`firstUseMs` in `installShaderCompileGuard`'s `stats` plus
the timed first `getUniforms` call; `shaderCompileStats: () =>
shaderCompileGuard,` in the returned API.

**Measured** (real RX 580 through ANGLE/GL-EGL, headless Chromium, staged
layout served locally, a visitor with no session. Two sets of six interleaved
runs per side, medians, on a box at load average ~50, so single runs vary by
seconds):

| | main | branch |
| --- | --- | --- |
| first contentful paint | 340 / 140 ms | 318 / 294 ms |
| first rendered frame | 3697 / 2789 ms | 3041 / 2578 ms |
| interactive (`forkmesh:world-interactive`) | 5066 / 3993 ms | 4176 / 3650 ms |

First frame and interactive did not regress. The gap between them is inside
this machine's noise, so do not read it as a speed-up. First contentful paint
is not a like-for-like number. On `main` it is text painted before any loader
exists (the loader card appears only after the shell module runs, about 280 ms
into the screencast). On the branch it is the loader card, which waits for the
parse gate. On a less loaded run (`probe-fcp`, five loads each) the medians
were 84 ms and 104 ms. Screencasts: `main` shows the small 4 % card, then the
taller ledger card over the HUD, then the scene behind the card. The branch
shows one card of fixed shape on a blank page from its first painted frame,
the log panel for debug and admin viewers, and the suggestions after load.

Tests: `world/tests/test_world_load_experience.py` covers the source contracts
(one loader, static ledger rows, hidden shell, reveal order, accessibility,
scene hooks) and runs Node tests of the log buffer, suggestion ranking,
visibility rules, preferences with blocked storage, resource classification
and the controller: it records while hidden, disconnects and stops after load,
and audits only when visible. The loader contracts in `test_world_frontend.py`,
`test_world_initial_load_performance.py`, `app/tests/test_blog_social_reach.py`
and the `progressive-first-paint` browser spec now assert the single static
loader.

## Load speed pass: draggable log, member circle, restyles (2026-09-29)

An admin's load log on an RX 580 showed the World interactive at 14.85 s,
with 10.4 s of long frames. The worst were a 2.4 s `Response.json.then`
handler, a 2.2 s scene-construction frame and 1.3 s of forced layout while the
runtime evaluated. `world-runtime.js:<offset>` in the log is a character offset
into the served runtime, which is `world.js` byte for byte, so offsets map
straight back to the source.

**Draggable load log** (`world-boot.js`, `index.html`):

- The header (`data-world-boot-log-handle`) is a pointer-event drag handle
  for mouse, touch and pen. `touch-action: none` and `user-select: none` sit on
  the header only. The panel takes pointer capture, so neither the loader card
  nor the canvas sees the drag, and nothing is added to `window`.
- Moves are coalesced into one `left`/`top` write per animation frame. Buttons
  in the header still click. A double-click on the header resets the panel to
  its bottom-left dock.
- `clampBootLogPosition()` keeps the whole panel inside the viewport, with an
  8 px margin. It runs on window resize, and a `ResizeObserver` re-clamps when
  the panel grows or collapses. Neither runs per frame.
- The position is stored with the collapse flag in
  `forkmesh.world.bootlog.v1` (`position: {left, top}`). All storage access is
  in try/catch. The first-paint inline script applies a stored position before
  the module loads, so the panel does not jump.
- The log list no longer rebuilds itself when a resource entry arrives with an
  earlier timestamp than lines already shown. It used to `replaceChildren()`
  and re-render every line, up to 120, several times during boot. Late lines
  are now inserted in place (`data-world-boot-log-at`). `syncChrome()` writes
  an attribute or text only when the value changes.

**Member circle** (the 2.4 s `Response.json.then`). The handler is
`loadNearbyMemberData()`. `/api/accounts/users` resolves, then
`applyWalletBadges` → `renderPeers` → `syncMemberLounge` →
`updateMemberLounge` builds one avatar per member.

- 1.9 s of it was `emojiPixels()`: a `getImageData` read-back of a 256² GPU
  canvas, once for every member with a country flag, inside
  `countryShirtTexture`. `cachedFlagShirtPalette(flag)` now samples each glyph
  once, on a shared `willReadFrequently` canvas (CPU-backed, so the read-back
  is cheap), and caches the palette by glyph (at most 512).
- Figure construction is time-sliced. The roster loop still runs synchronously
  for seat labels, `seen` and stale-figure removal, but it queues each member
  into `loungeJobs`. `scheduleLoungeFigures()` sorts the jobs by distance from
  the player to their seat and dresses them in slices of
  `LOUNGE_FIGURE_SLICE_MS` (8 ms). Figures that already exist update inline.
  The rest continue in `requestIdleCallback` slices (120 ms timeout). A newer
  directory bumps `loungeBuildGeneration` and cancels the old queue.

**Restyles** (the "~810 elements restyled" the user saw):

- `html:has(body.world-active)` marked `<html>` as affected by `:has()`, so
  any DOM insertion anywhere could restyle it. The rule is now
  `html.world-active`, with the class added and removed next to the body's.
- `.fm-world:has(.world-detail[…])`-style rules used a descendant argument.
  Every text change inside the ~1,400-element HUD (perf tiles, status preview,
  boot steps) scheduled `:has()` invalidation on `.fm-world` and restyled its
  whole subtree. Every target is a child of `.world-hud`, so the arguments are
  now fixed depth: `.fm-world:has(> .world-hud > .world-detail[…])`.
- Redundant writes are skipped: `updateIdentityUI` (runs every second with
  "Show local time"), `updateWorldStatusUI` and the office aquarium controls
  (every few frames from `animate`).
- Result, 3 s idle trace after load: 38 style recalcs touching 3,429 elements
  dropped to 3 recalcs touching 79. During boot, recalcs over 50 elements went
  from 25 (18.6k elements, 608 ms) to 16 (8.6k elements, 420 ms).

**Forced layout at startup.** `connectedCallback` read `visualViewport`/
`innerWidth` (`applyViewportMetrics`) right after inserting the HUD template,
which forced a full style and layout of the new subtree inside the task. The
read now happens before `innerHTML = worldTemplate(…)`. It touches only the
host's custom properties and data attributes. The next forced reader,
`bindDetailResize` → `detailWidthLimit` (`clientWidth`, ~320 ms in a trace),
now restores the stored width in an idle callback. That slider's ARIA values
matter only once a detail panel opens. Forced layout inside the startup
`Window.fetch.then` task fell from ~245 ms to ~3 ms (median).

**Scene construction:**

- The Office link kiosk, the rewards board and the task and done bulletins
  paint nine large canvases (1080×660 and up). `canvasTexture` records the
  paint instead while `deferredCanvasPaints` is an array. That is set only
  around the kiosk-to-bulletin block. `paintDeferredCanvases()` paints the
  queue in 8 ms idle slices after construction. `repaintCanvasTexture` drops
  any paint still pending for its canvas, so a data update is never
  overwritten by the stale first paint.
- Renderer setup + lights (~200–400 ms) is mostly the PMREM sky environment
  (`fromScene`, three filter programs). It stays synchronous on purpose.
  Setting `scene.environment` later would change every PBR material's program
  key and recompile them all. `applyRendererResolution()` now makes one
  `setDrawingBufferSize` call and skips unchanged sizes. `setPixelRatio` +
  `setSize` reallocated the drawing buffer twice. Sizing the renderer at
  creation was tried and reverted: the first allocation costs the same
  (~300 ms here) wherever it happens.
- Shader first use: `installShaderCompileGuard` also caps first uses at
  `PROGRAM_FIRST_USE_FRAME_BUDGET_MS` (24 ms) per `renderer.info.render.frame`.
  Past the cap, further newly ready programs defer their draw to the next
  frame (`budgetDeferredDraws`). The link wait is spread over frames instead
  of one multi-second reveal frame. On this box that frame is dominated by
  CPU contention with other builds. The user's own log showed only 251 ms.

**HTML → first module** (the 7.3 s gap in the user's log). A fresh-profile
headless load of production shows no gap: `world.js` arrives at 149 ms. Every
subresource of the user's page started after 7.7 s, so the renderer did not
issue requests for 7 s after the HTML arrived. That points at the embedding
browser or network, not the document: `index.html` has no render-blocking
resource (the stylesheet is `media=print` + `onload`, and scripts are
`defer`/`module`). Changes that still shorten the critical chain:

- `index.html` modulepreloads `world-boot.js`, `world-debug-log.js`,
  three.core, three.module and `world-scene.js`. Build staging
  (`stage_world`) adds `<link rel="modulepreload"
  href="/world/world-runtime.js?v=<hash>">`, using the same hash as the
  loader's `import()`.
- `/world/world-runtime.js` is `immutable` in `www/public/_headers`. The rule
  sits after `/world/*`, so it wins. Every request for it carries the content
  hash, and the no-store loader decides which hash to ask for, so a cached
  runtime can never pair with a newer loader. Everything else under `/world/*`
  stays `no-store`. Versioning `world-scene.js` and the other relative module
  imports would let them be cached too, but staging does not rewrite relative
  `import` specifiers yet.
- `analytics.js` and `membership-prompt.js` are `<script type="text/plain"
  data-world-after-interactive src=…>` placeholders. They keep the `src`, so
  the `?v=` stamping still applies, but the browser does not fetch them.
  `forkmeshWorldLoadDeferredScripts()` turns them into real scripts in an idle
  callback after `forkmesh:world-interactive`, and the 10 s load-failure
  watchdog calls it too, so client error reporting still loads when boot
  fails.
- `dashboard-chat.js` (52 KB plus its history replay) loads on
  `forkmesh:world-interactive` rather than inside `bindUI()`. The collapsed
  CHAT bar is behind the loader until then.

**`/api/accounts/users` (1.27 s).** The public roster is 1.47 MB for 234
members, and 97 % of it is `avatarPng`: inline base64 PNGs, which do not
compress (1.32 MB with brotli). The public response is already edge-cached
(`_users_directory_cache_get`), the World reads it without credentials, and
the admin projection is never served to it. There is no cheap server fix that
keeps member faces. Dropping the field would blank the faces in the Members
Circle. What it needs: a per-account avatar URL with a content hash (for
example `/api/accounts/<name>/avatar.png?v=<sha>`, `immutable`) in place of
`avatarPng` in the directory (`_account_chat_user_payload`).
`memberDirectoryAvatar()` in `world.js` already accepts a URL in `avatar`, so
the World would then fetch faces lazily, and only for figures that are
actually built.

**Measured** (RX 580 through ANGLE/GL-EGL, headless Chromium, local dev server
proxying production APIs, a visitor with no session, 1600×900. Five
interleaved runs per side, medians. The box ran at load average 20–47, so
absolute numbers vary by seconds and only the paired medians are comparable):

| | main | branch |
| --- | --- | --- |
| interactive (quiet box, 4 runs each) | 7.33 s | 2.52 s |
| interactive (load ~22) | 7.67 s | 2.99 s |
| interactive (load ~40) | 15.8 s | 4.4 s |
| member-directory `Response.json.then` | 3.35 s | 0.18 s |
| forced layout in the startup fetch task | 245 ms | 3 ms |
| worst long frame during load | 5.15 s | 2.53 s |
| long frames during load | 9 | 7 |
| fps / p99 frame after load | 53 / 83 ms | 48 / 100 ms |

The fps and p99 numbers after load are inside this machine's noise. Across
both paired sets the medians were 50 and 48 fps.

Tests: `test_world_boot_log_drag.py` covers the clamp and the preferences
round trip, including blocked and corrupt storage, with Node. It also drags
the real panel in headless Chromium with mouse and touch pointers. That run
checks that the canvas sees no pointer events, that collapse still clicks,
and that the position survives a reload and is re-clamped on a smaller
viewport. It is skipped when Playwright is not installed.
`test_world_load_speed.py` covers the per-frame shader budget (Node) and the
source contracts for the palette cache, lounge slicing, deferred plates,
`:has()` scoping and write guards, the startup layout order and the deferred
chat. `test_world_initial_load_performance.py` covers the preloads, the
deferred scripts and the header rule. `app/tests/test_split_workers.py`
checks that the staged runtime preload uses the loader's hash.

**Chat-feed forced layout (2026-10-07).** An optimization report from a
mid-tier laptop (RX 580, 60 fps, GPU 0.6 ms) showed a burst of 80–110 ms long
frames. The cause was the chat feed, not the scene. Each history slice
appended rows to `#fullChatMessages`, then `pinHudIslandFeed()` (`world.js`)
read `scrollHeight` to pin the collapsed island. That set `scrollTop`, and the
feed's scroll handler (`dashboard-chat.js`) then wrote the scroll-rail
attributes and read `scrollHeight` again. Each read forced a full style
recalc. In the headless World harness one recalc after appending a row cost
23–35 ms.

Removing `:has()` rule groups one at a time in that harness found two groups
that made every DOM insertion expensive:

- The 44 thread-dialog rules, `dialog:has(> [data-chat-thread-list])`. Chrome
  has to re-check that argument on every insertion anywhere under `<body>`.
  `ensureThreadDialog()` now marks the dialog with `data-chat-thread-dialog`,
  and `world.css` selects `dialog[data-chat-thread-dialog]`.
- Five `.world-top-link:has(> .world-tool-count:not([hidden]))` compact-HUD
  counter rules. They were dead: no top link holds a counter any more (only
  the online button does), and nothing sets `data-world-tooltip`. They are
  deleted.

One descendant-argument rule, `.fm-world:has(.world-detail:is(…))
.world-detail-resize`, had escaped the earlier depth fix. The grip is the
detail panel's next sibling, so it is now `.world-detail:is(…) +
.world-detail-resize`, with no `:has()`.

The scroll handler now does all its reads (`fullLogAtNewest()`, `scrollTop`,
`historyPreloadThreshold()`) before `syncChatScrollThumb()` writes, and calls
`revealOlderHistory()` last.

Result in the same harness: style after a chat append went from 23–35 ms to
0.8–1.1 ms, and append plus pin from ~34 ms to 1.7 ms. The same recalc cost
explains the report's `pruneExpiredChat` and other forced-layout entries.
`test_world_load_speed.py` guards against both selector shapes coming back,
and checks the reads-before-writes order.

The report's one audit finding (1.1k MeshStandard meshes) was left alone. The
GPU main pass was 0.64 ms against a 4.2 ms CPU `renderer.render`, so cheaper
fragment shading would not move the frame.

## Scene modules (2026-09-29)

`world-scene.js` was one 44k-line file whose `createWorldScene` closure was
25k lines. It is now a facade that re-exports `scene/`, and the closure is
split into modules of at most about 2,800 lines (`core.js`; the largest
domain module, `office-aquarium.js`, is about 2,400). The split was mechanical (an AST tool moved declarations
without editing them), so behaviour and frame output match main: the same
poses render pixel-identical except for the harness's known nondeterminism,
and every public API function called with the same arguments returns or
throws the same thing on main and on the branch.

Layout:

- `scene/<domain>.js`: module-level helpers from the old file (textures,
  materials, avatar parts, layout constants). They import each other; the
  graph has no cycles (`avatar-shared.js` holds the avatar constants that
  used to tie `avatars.js`, `avatar-badges.js`, `avatar-outfits.js`,
  `avatar-instancing.js` and `treasury.js` into a ring).
- `scene/core.js`: `createWorldScene`. It sets up the renderer, camera,
  lights and the shared state (`let` bindings the whole scene reads and
  writes), installs every module's functions, builds the districts in their
  original order, wires window events, and returns the public API.
- `scene/runtime/<area>.js` and `scene/districts/<area>.js`: what used to be
  the closure's inner functions and construction code.
- Layout pieces that change together live in one module each: every layout
  constant (hex tiles, roads, junctions) in `layout.js`, the desert plots in
  `desert-plots.js`, the hex road network (its builder and the element that
  places it) in `districts/roads.js`, and all bike behaviour (the loop route,
  riding, dismounting) in `districts/bikes.js`.

### The module contract

A module exports up to two functions:

- `functions({ THREE, <callbacks>, ctx, fns })` declares the area's inner
  functions and returns the ones other code calls. `core.js` calls every
  `functions()` first thing in `createWorldScene` and merges the results into
  `fns`, so any function can call any other at any time, as the hoisted
  declarations in the old closure could. Inside a module, calls to its own
  functions are direct; calls into another module go through `fns.name(...)`.
  State used by only one module's functions (a cache, a timer id) is declared
  inside that module's `functions()` instead of in `core.js`.
- `build({ ...values, ctx, fns })` holds a district's construction statements
  and runs at the same point in `createWorldScene` as the code it replaced, so
  meshes, `Math.random()` draws and registrations happen in the old order.
  It returns the objects later code needs, and `core.js` destructures them.
  A section that contains `await sceneSlice()` becomes an `async build` that
  `core.js` awaits, so the time-sliced construction yields at the same points.

`ctx` is the scene's shared state as seen from a module: one getter per
`core.js` binding a module reads, plus a setter for each one it assigns
(`ctx.currentSpace = "beach"`). It is an instance of a class declared inside
`createWorldScene`, not an object literal: V8 puts an object literal with
accessors in dictionary mode (about 25 ns per read in Node 22), while a class
prototype with up to about 500 accessors stays in fast mode (about 6 ns, and
inlinable). Values that exist before the section runs (materials, `world`,
`interactive`) are passed to `build` directly instead.

Two traps the split had to handle, and new code must respect:

- **A district's build may call a module function that reads the district's
  own objects.** `addWorldBike` reads `ctx.bikeStates` while the bikes build
  is still running, before its return value is destructured. Such bindings
  are published as they are created: `core.js` declares `let bikeStates;`
  before the build, and the build assigns `ctx.bikeStates = bikeStates` right
  after its declaration.
- **An imported binding cannot be assigned.** Module-level state shared with
  `createWorldScene` (`canvasTextureScale`, `deferredCanvasPaints` in
  `canvas.js`) is changed through `setCanvasTextureScale()` and
  `setDeferredCanvasPaints()`.

Where to put new code: a new district or Office area gets its own
`districts/<name>.js` with a `build` (and `functions` if it has behaviour)
and a call in `core.js` at the point in the build order where it belongs.
Behaviour for an existing area goes in that area's module. Anything every
module needs belongs in `core.js` state and `ctx`. Every file under `scene/`
must be listed in `world-loader.js`'s modulepreload array
(`test_world_scene_modules.py` fails otherwise).

Source-text tests read the scene through
`app/tests/world_scene_source.py`: `world_scene_source()` concatenates the
facade and every scene module, and strips the `ctx.` and `fns.` prefixes so a
contract written against the old closure (`currentSpace = "beach"`) still
matches `ctx.currentSpace = "beach"`.

## Resource authority (2026-09-29)

`scene/resources.js` is the one place scene code builds geometries and
materials:

- `resources.primitive(THREE, "BoxGeometry", 1, 2, 3)`: a built-in
  parameterised geometry, cached by type and arguments. Every mesh that asks
  for the same box gets the same `BufferGeometry`.
- `resources.geometry(THREE, key, factory)`: any other geometry, cached under
  an explicit key.
- `resources.material(THREE, params, type)`: a shared material, cached by type
  and parameters.
- `resources.uniqueMaterial(THREE, params, type)`: a material the caller owns
  and may change (fades, tints, blinking lights, swapped maps).

Cached resources get the no-op `dispose` from `markSharedResource`, so tearing
down one mesh cannot free a buffer the rest of the scene still draws.
`makeMaterial` and `makeSharedMaterial` in `materials.js` go through
`uniqueMaterial` and `material`.

**When to share.** Share a geometry only when its arguments are constants:
arguments computed at run time (a random size, a per-record width) would grow
the cache without bound, so those stay `new THREE.XGeometry(...)` and are on
the allow-list. Share a material only when no code changes it after it is
built. The rule used when the calls were converted: the material is built
inline as a mesh's material, its arguments are literals, and the mesh is only
added to a parent, pushed to `interactive` or passed to `setShadows`. A mesh
that is stored anywhere else (in `userData`, in a returned object, in a list)
can be reached by code that tints or fades it, so its material stays unique.
`resources.sharedMaterialDrift(THREE)` lists shared materials whose current
values no longer match their cache key; the browser harness checks it after a
full tour and after calling every public API function, and it is empty.

**Material type.** With no type, `material`/`uniqueMaterial` choose
`MeshLambertMaterial` for rough, non-metal surfaces and `MeshStandardMaterial`
for specular ones (metalness at least 0.3, roughness at most 0.35, an
environment, normal, roughness or metalness map, or `hero: true`). Every
existing call passes `"MeshStandardMaterial"` explicitly, so the World looks
exactly as before. Switching the existing surfaces to Lambert was measured and
left out: it changes most frames by more than the golden-frame tolerance (the
Lambert surfaces lose the sky environment's diffuse light, so floors and grass
read darker and more saturated), and the performance gate fails any real
visual change. New non-hero surfaces should use the default.

**Culling.** Instanced batches keep frustum culling on and recompute their
bounding sphere when their instances move: the avatar part batches after each
sync that moved or recounted instances, the avatar detail mirror (pins,
activity rings, badges and operator belts) through `createInstancedMirrorSet`,
and the aquarium's fish after each swim step. The sky's planets and sun/moon
glows opted out until the scene-audit pass below gave them padded bounds.

**Instancing.** Operator belts and their non-blinking node lights now ride
the avatar detail mirror, which drew the Members Circle in 141 fewer calls.
Mirror-node cabinets kept unique materials because they are rebuilt from
network data after the one-time static instancing pass; since the scene-audit
pass below they share materials and draw their repeated parts through a
cabinet mirror of their own.

**The contract.** `world/tests/test_world_resource_authority.py` counts every
`new THREE.*Geometry(` and `new THREE.*Material(` outside `resources.js` and
fails when a file has more than its allow-list entry. The allow-list only
shrinks: a second test fails when a file has fewer than listed, so remove the
entry when you convert a site. Allowed: custom `BufferGeometry`, extrusions,
tubes and lathes, shader, sprite, points and line materials, geometries with
computed arguments, and the files outside `scene/` (`world-sky.js`, the
package `elements/`, floor notes).

**Budget.** `world/perf-budget.json` was tightened from two gate runs for the
counts this work lowers and that do not depend on how far the background GPU
warm-up got: duplicate materials, culling opt-outs, and Members Circle draws.
Programs, geometries, textures and triangles were left at their ceilings,
because on a busy machine the warm-up lags and the same build measures far
lower (main itself read 61 programs at the beach in one session and 142 in
another).

**Source-text tests.** `world_scene_source()` presents
`resources.primitive(THREE, "X", ...)` as `new THREE.X(...)` and
`resources.uniqueMaterial(THREE, {...}, "Y")` as `new THREE.Y({...})`, so a
contract about what is built does not depend on how it is built.

**Gate check after the split (2026-09-29).** One gate run reported
office-engineering textures 852 over the 733 ceiling. That run was on
llvmpipe (the GPU device nodes were missing before a reboot), where the same
commit read 506, 706 and 852 textures at that pose in three runs. On the
RX 580 main read 432-541 and the branch 480-549 at spawn and office
engineering, with the same texture memory in the audit (829.6 MB), so the
branch adds no textures and the ceiling stays. The counts that did move:

- town-overview triangles 119,478 to 124,442 (125,150 with main's ring labels
  and roof changes). The avatar detail mirror draws operator belts and their
  node lights as instanced batches; on main each belt was its own mesh and
  every one was frustum-culled at that pose, while a batch spanning the crowd
  intersects the view and submits all its instances. Removing the belts from
  the mirror restores 119,478 and costs the 141 Members Circle draws the mirror
  saves (1,816 instead of 1,676), so the ceiling was raised instead.
- spawn triangles, arrival-stats triangles and members-circle draws rose with
  main's own ring labels and roof edits (main reads the same values).
- GPU geometries read 6-11 higher than main at spawn, beach, repository
  portals and town overview. The scene holds the same number of geometries;
  the difference is in what the renderer has uploaded, which varies with the
  warm-up (main itself read 211 and 358 at arrival-stats in back-to-back
  runs). Those ceilings were left alone.

Only the exact counts (draws, triangles) were raised, each to the merged
value of two RX 580 runs through the budget policy; times were not rebased
because the box was under heavy load. town-overview `duplicateGeometries`
reads 5 or 119 on the branch and 205 on main depending on whether the
Members Circle deduper has finished, the same timing that keeps that metric
out of the members-circle budget.

**Main's World edits after the split.** Commits on main that still edited the
old single-file `world-scene.js` were ported into the modules: the Custom
avatar band (`MEMBER_CIRCLE_AVATAR_BAND`, `memberCircleBandSpec`,
`memberHasCustomAvatar`) in `member-engagement.js`, its texture and the ring
label texture in `members-circle.js`, the band layout and labels in
`districts/members-circle.js`, photo-first roster order in
`runtime/remote-players.js`, photo retries in `avatars.js` and
`runtime/avatar-selection.js`, the roof-height facade columns in
`office-building.js`, and the shaft cap, air conditioner and ladder in
`districts/office-displays.js`. Golden frames at all nine poses match main's
frames after the port.

## Load speed round 2: prefetch, sliced build, minified modules (2026-09-29)

An admin's production log after round 1 (RX 580) was interactive at 2.99 s
with one 1168 ms long frame (`TimerHandler:setTimeout → done`, the whole
scene build in one task), the account calls starting only after the scene,
a 205 ms `Response.json.then` for the 1.3 MB member directory, and a
`FrameRequestCallback` at `world-scene.js:1543927` with forced layout. A
second log added: the member-list handler ran four times (128–161 ms each),
the zip line looked like the heaviest element (93 ms), and the first
interactive frame landed 300 ms after the loader closed.

**Account data at boot** (`world-boot.js`, `world-loader.js`, `world.js`):

- `startWorldBootPrefetch(window)` starts `GET /api/accounts/users?view=world`
  (`WORLD_MEMBER_DIRECTORY_PATH`) and, with a stored session, the viewer's
  `/api/accounts/<me>?viewer=<me>` with the same bearer header `fetchJSON`
  would send. The loader calls it as soon as `world-boot.js` resolves
  (~+120 ms); `world.js` calls it too (idempotent) so the dev server, which
  serves the runtime directly, gets the same behaviour.
- The in-flight `fetch` promises sit on `window.__forkmeshWorldPrefetch` keyed
  exactly like `fetchJSON`'s `requestKey` (`GET:public:<path>`,
  `GET:account:<name>:<path>`). `fetchJSON` takes a matching entry once
  (falling back to a real fetch if it rejected), so nothing is fetched twice.
  An entry older than 30 s is ignored. Storage access is in try/catch.
- The World now reads the compact roster (`docs/app/member-roster.md`):
  no `avatarPng`, a 128² WebP `avatarThumb` data URL per member with a face,
  and `avatarUrl` as a fallback. `memberDirectoryAvatar()` prefers the thumb,
  then a same-origin `/api/accounts/<name>/avatar?…` URL, then the legacy PNG.
  Every face arrives in the one response; nothing is fetched per figure. On
  production the old field was capped at 90,000 characters, so 4 of the 8
  faces never showed; all 8 do now.

**Interactive means the player can move and look.** `markWorldInteractive()`
runs in the reveal frame, right after `revealWorld()` removes `inert` and the
host's `pointer-events: none`. The overlay removal and the
`forkmesh:world-interactive` event still follow 320 ms later (logged as
"Loader removed · world-interactive event"). Work that is not needed to move
and look waits on `afterWorldInteractive()`, which resolves two animation
frames plus a task after interactive, so it never lands in the first
interactive frame: member directory processing (`loadNearbyMemberData`,
`refreshMemberDirectory`) and the viewer's profile card. The `session` and
`data` ledger rows still show done at 100 %, but their load-log phase ends
when the work really ends (`bootBackgroundOpen`), so the log is truthful.

**Member directory processing** (the four `Response.json.then` handlers):

- Callers: `loadNearbyMemberData` (boot), `refreshMemberDirectory` (presence
  arrivals, forced), `syncWorldClientProfile` (forced a full refetch after
  saving the client profile), `applyWalletBadges` → `renderPeers` →
  `syncMemberLounge` for every wallet balance, and `loadWorldData` (explicit
  refresh). `loadNearbyMemberData` alone ran the lounge update three times.
- `syncMemberLounge()` now queues one `flushMemberLounge()` per microtask
  checkpoint, and `updateMemberLounge` returns immediately when its inputs
  (roster, counts, leaderboard, guests, local bench, removed accounts, live
  avatar counts) serialize to the same signature as last time.
- The profile write no longer refetches the roster: `patchOwnDirectoryRow()`
  updates the viewer's own row (country, browser, OS) and resyncs; only an
  account missing from the roster still forces a fetch.
- `noteDirectoryMembers()` does nothing until interactive and while the boot
  load is in flight; before, the populate step's first lounge sync saw an
  empty roster and forced a second, unsliced processing of the same response.
- Inside `updateMemberLounge`, the aquarium school, the badge/face refresh of
  standing avatars, the leaderboard card and the seat-label atlas run as
  separate `queueLoungeTask` tasks (one per `setTimeout`, a newer update
  replaces a pending one of the same name). Figures stay time-sliced
  (round 1).

**Sliced scene build** (`world-scene.js`, `world-boot.js`):

- `createWorldScene` is `async` and `await sceneSlice()` follows every
  top-level element registration, plus checkpoints inside the Office stretch
  and the construction tail. A slice yields once 12 ms (`SCENE_BUILD_SLICE_MS`,
  option `sceneSliceMs`, 0 disables) have passed: `scheduler.yield()` where
  available, else a `MessageChannel` message, else `setTimeout(0)`. Input and
  the loader's counter run between slices; the build's own work is unchanged.
- The trace subtracts yield time (`sceneYield(ms)`), so element times stay
  work only, and the log adds "Scene built in N slices · work · yielded ·
  longest slice".
- **The round 1 element list was mislabelled.** Time is charged to the next
  registration, so "Roof-to-beach zip line", "Instance launcher booth" and
  "Weather (rain & snow)" carried the Office interior built before them
  (aquarium, upper floors, floor displays, marketing floor, attendance
  boards, logo fountain, elevator). Trace checkpoints now name those blocks.
  Measured on their own, the zip line takes about 4 ms, the booth about
  11 ms and the weather under 4 ms, each inside one slice, so the zip line
  and the booth are not deferred: the zip line and the campfire are in view
  from spawn, and deferring them would risk pop-in and collision gaps for a
  few milliseconds.
- Weather is built on demand: `let weather = null`, `ensureWeather()` from
  `syncWeatherField()` when a rain or snow preset shows. The element is
  registered empty at boot so the elements list and a stored "disabled"
  choice still apply. The particle budget is remembered until then.
- The renderer and sky lights are unchanged (round 1: the environment must be
  synchronous), but `rendererContextInfo()` (a synchronous `getParameter`
  round trip) moved from the middle of the build, where it waited 90–260 ms
  behind the GPU queue, to right after the context is created.
- One drawing-buffer allocation: `resize` does nothing until construction
  ends (`canvasSized`), then sizes once before the animation loop starts, from
  the latest `ResizeObserver` `contentRect` (no forced layout) or the
  construction-time bounds. The resize path no longer renders explicitly
  before the first frame. The perf bar is synced when the ticket resolves
  (`syncPerfBar()`), so an admin's canvas has its final height before it is
  sized instead of being reallocated (~160 ms here) a second later.
- `FrameRequestCallback → anonymous @ world-scene.js:1543927` was that
  resize callback: `getBoundingClientRect()` after the HUD changed, then a
  synchronous full render in the same frame as the animation loop's. Both
  are gone from the load path.

**First view before the loader closes.** `prepareFirstView()` (called at the
end of the populate step, capped at 900 ms) waits until the compile guard has
no pending programs and the program count is stable over two rendered frames,
and calls `primeReadyPrograms(8)` in between: the first `getUniforms()` of
each ready, unused program, 8 ms per slice. The log line "First view ready ·
N programs · primed · still compiling · ms" reports it. Program counts at
settle differ with timing: the idle GPU warm-up starts 1.5 s after
construction, so a slower load has compiled more warm-up programs by the
time it settles (41 on main, 26 on the branch in the same run, the extra 15
being `MeshBasicMaterial`/`MeshStandardMaterial` warm-up variants). That is
not material duplication.

**Periodic work.** The diagnostics interval (`world-runtime.js:1169748`) no
longer renders while the tab is hidden (the crash-guard beat still runs). Its
first scene-complexity walk used to run synchronously inside the first tick
during load; it now uses the same idle-sliced walk as later refreshes
(`collectSceneComplexity` removed).

**Minified, versioned modules** (`app/tools/build_site_assets.py`
`stage_world`):

- esbuild 0.25.10 is a pinned build dependency in `world/tools/package.json`
  (lockfile committed, `node_modules` ignored). `esbuild_binary()` runs
  `npm ci --prefer-offline` there when it is missing; `FORKMESH_ESBUILD`
  points at another binary. `FORKMESH_WORLD_MINIFY=0` stages unminified (still
  versioned).
- Every World-owned module is minified in one esbuild call: `world/**/*.js`
  except `world/vendor/`, `qr.js`, and the bundled packages'
  `marketplace/packages/*/content/world/`. App-owned modules the World
  imports (`../account-events.js`, `../chat-*.js`, `/dashboard-chat.js`) are
  not touched: versioning them would load a second instance next to the
  App's own unversioned import.
- Each gets an external source map (`<file>.map`, `sources` naming the
  repository path, `sourcesContent` included) and a
  `//# sourceMappingURL=` line. **Load-log offsets such as
  `world-runtime.js:231599` are now into minified code**: resolve them with
  the `.map` (DevTools does this automatically), not against `world.js`.
- Versioning: every quoted module URL in those modules and in
  `world/index.html` (static and dynamic imports, template literals, URL
  constants such as the loader's preload list and `loadExtra(...)`) that
  resolves to a staged module gets `?v=<sha256[:12]>` of the target's final
  bytes. Hashes are computed dependencies first; an import cycle stops the
  build. A query already on the URL is kept after the version
  (`./world-discord-panel.js?v=…&retry=${n}`). The runtime modulepreload and
  the entry `src` use the same map.
- Caching: `deploy/core/caddy_routes.py` repeats the `@world_versioned`
  immutable header after the `_headers` directives for World files, so a
  `?v=` URL stays `public, max-age=31536000, immutable` even under the
  `/world/*` `no-store` rule; unversioned requests keep `_headers`. A deploy
  serves new bytes under an old hash only to a tab still running the old
  runtime, which never reloads that URL.
- Brotli: `tools/next_deploy.py precompress()` writes `.br` sidecars through
  node's zlib (quality 11) when the Python `brotli` module is missing, which
  it is on the build box; Caddy already serves `precompressed br gzip`.
- The dev server still serves `world/public` unminified, and every source
  test reads `world/public`.
- The admin profiler's callback descriptions read `Function.toString()`, so
  on a deployed build they show minified code. Use the dev server for that.

Sizes (bytes on the wire, brotli where the build writes it): `world-scene.js`
408 KB gzip → 186 KB br (1.62 MB → 693 KB raw); `world-runtime.js` 330 KB
gzip → 175 KB br (1.37 MB → 813 KB raw). The World dist without maps is
4.3 MB; the maps add 5.7 MB and are only fetched by DevTools.

**Fonts.** `ABCFavorit-Bold` ships as WOFF2 (111 KB OTF → 48 KB, lossless),
the only source in `world.css` and `membership-prompt.js` (the OTF was
removed on 2026-10-13). All three
faces are `font-display: swap` and block nothing; they load at ~0.6 s from
`world.css`, so they are not preloaded (that would compete with the module
downloads).

**Measured** (headless Chromium, 1600×900, the staged `world/dist` of main
at 46eb4ea873 and of this branch, each served with the sidecars its own
`precompress()` writes, production API responses recorded and replayed with
150 ms server time, Chromium throttled to 40 Mbit/s and 20 ms RTT, an admin
session. One discarded warm-up load per side, then five alternating pairs,
medians; load average 11–16. The first runs used the RX 580; from 14:57 the
render node's ACL belonged to another logged-in seat user, so ANGLE fell back
to llvmpipe for both sides and for the gate. The pairs below are llvmpipe):

| | main | branch |
| --- | --- | --- |
| first frame | 1733 ms | 1481 ms |
| loader closed (100 %) | 1756 ms | 1614 ms |
| interactive | 2672 ms | 1627 ms |
| loader removed / `forkmesh:world-interactive` | 2672 ms | 1948 ms |
| first interactive frame | 2787 ms (+115) | 1701 ms (+74) |
| longest long frame during load | 640 ms | 266 ms |
| longest blocking task during load | 546 ms | 151 ms |
| JS transferred | 1084 KB | 632 KB |
| API transferred | 2085 KB | 107 KB |
| member roster request | +1225 → +1704 ms, 1004 KB, twice | +165 → +350 ms, 30 KB, once |

On main "interactive" is the overlay removal; on the branch it is the reveal,
and the overlay removal is listed separately. The early RX 580 pairs showed
the same shape (interactive 2.9 s → 1.6–1.8 s, longest frame 776 → 230 ms).
The remaining longest task is the runtime's first evaluation with the HUD
template's style and layout (~150 ms blocking), then the first render.
Faces: on the branch all seven members with an avatar who sit at the fire
wear it (the eighth is the viewer, who is walking); on main three do.

Tests: `world/tests/test_world_load_round2.py` (prefetch plan, keys and
expiry with blocked storage; trace yields; slice points; lazy weather;
canvas sizing; program priming; interactive order and gating; lounge
coalescing and signature; thumbnail avatars; font; module versioning order,
rewrite forms and cycles; esbuild output that still runs and maps to its
source). Source contracts in the older World tests follow the new names.
`app/tests/test_split_workers.py` checks the staged maps, that every
versioned import carries its target's hash, and the minified asset versions.
`deploy/tests/test_caddy_routes.py` checks the header order,
`deploy/tests/test_next_deploy.py` the node brotli sidecar.

## Districts polish (2026-09-29)

Branch `feature/world-districts-polish`, built on `refactor/world-modules-2`.

### No dome over the repository circle

The repository ring used to seal an opaque shell (wall, transom, dome cap,
rim, door frame and an EXIT sign) over the portals whenever a visitor walked
inside, cull every World root outside it, and hold the walker inside a wall
collider with one doorway. Visitors read it as a dome slamming down over the
circle, so it is gone with its geometry, materials, raise and dissolve
animation, the wall collider, the "repositories" enclosure-scene mode and the
`repository-enclosure` World element. The portals' concentric size rings
still unfold for a visitor standing among them:
`updateRepositoryRingExpansion()` in `scene/districts/compact-districts.js`
expands them inside `REPOSITORY_RING_EXPAND_RADIUS` and folds them back past
`REPOSITORY_RING_COLLAPSE_RADIUS` (hysteresis, `scene/layout.js`).

### West side audit: leaderboards were not lost

Reported: the leaderboards and other features on the far side of the World
had gone missing after the hex re-layout (`8c2f5e70bc`) or the module split
(`a6c85fb859`). An audit found nothing to restore:

- Every `.name = "..."` object name and every data `id:` in the scene before
  the hex re-layout, before the split and on `main` is still built here. The
  only names that disappeared are the repository enclosure's (removed on
  purpose above) and the void-walk rails (retired earlier on `main`).
- `LEADERBOARD_ISLAND_CENTER_X` has been -130 since the reorganisation; the
  hex commit never touched the leaderboard district. In headless Chromium the
  district stands at (-130, 0, 0) with its hub, social and leaderboards circles
  and all 17 cards, on `main` and on this branch, well inside the town hex
  (about 170 units from the nearest hex road).
- The X and Reddit banner boards left the default World on purpose in
  `7cc28ee45b` (marketplace packages with `install.default: false`).

What can still make the west side look empty to one viewer: a hidden
`leaderboard-district` element or deleted pieces stored in that browser
(the new "Restore everything and start fresh" clears them). A compact
(phone) renderer used to draw a satellite district only within 100 units of
it (`COMPACT_DISTRICT_RESIDENCY` in `scene/quality.js`), so from spawn the
leaderboards were not drawn until the visitor walked west; since 2026-10-06
it also draws a district whose bounds are in the camera view (see Distance
detail).

### One start-your-world sign per desert hex

Each of the six desert hexes holds two `DESERT_PLOTS`, and each plot used to
get its own "Start your world here" sign, so every tile showed two identical
signs. `createDesertPlots()` now builds one sign per hex, at that hex's first
plot; the plots, cacti and sand are unchanged.

### Instance hexagons (2026-10-13)

An instance started from `/start` with **Give my instance its own world** on
gets a hexagon of its own, joined to the ForkMesh World
([INSTANCES.md](../INSTANCES.md), "In the World and on the homepage").

- **Where.** `INSTANCE_HEX_SLOTS` (`scene/layout.js`) is the second ring of
  hexagons, 12 tiles the same size and orientation as the city, one pitch
  (740) from their desert neighbours, so the tiling continues. The six tiles
  between two desert hexes (√3 pitches out, at 30°, 90°, …) fill first, then
  the six straight out from a desert hex (2 pitches). The server hands out
  the slot number; the client maps it to a tile.
- **What.** `createInstanceHex()` (`scene/instance-hexes.js`): a meadow
  hexagon, its own road ring with an asphalt patch on each corner where three
  roads meet, the logo as a 150-unit decal on the ground (shown once it
  loads), and a small landmark on the side facing the city: a stone plinth,
  an obelisk and a board whose face shows the logo, or the name's first
  letter without one. A label sprite above it shows the name and host. The
  logo is drawn into a 256 px canvas (`loadInstanceLogo`, contain fit, so an
  SVG without a size works) from `/api/start/instances/<id>/logo`, on the
  World's own origin.
- **Click.** The board, its face, the obelisk and the label are
  `instance-hexagon` interactives; `onInstanceHexagonSelect` opens the
  instance's address in a new tab.
- **Data.** `refreshInstanceDirectory()` (the deferred global work after
  boot, then every 5 minutes) also reads `/api/start/instances` (bucketed per
  5 minutes, no credentials); `normalizeInstanceHexes` keeps entries with
  `world: true`, an https address, a 32-hex id and an unused slot.
  Entries an admin hid or that missed two health checks are not in the list,
  so their hexagon goes away on the next refresh ([INSTANCES.md](../INSTANCES.md),
  "Offline instances, hiding and deleting").
  `updateInstanceHexes` (`scene/runtime/network.js`) rebuilds the
  `forkmesh-instance-hexes` layer only when the list changed, and the list is
  pushed onto a rebuilt scene with the other directories.
- **Walking.** `setInstanceHexTiles` registers the tiles with
  `desertWalkSurfaceContains`, which accepts each tile grown by the road
  width and a junction disc at each corner, like the desert ring.
  `WORLD_RADIUS` is 1900 so the farthest tile's road (about 1855 out) is
  inside the bound.
- **Cost.** Nothing while no instance has a hexagon, so the perf goldens are
  unchanged. Each hexagon adds about nine draws (ground, road, decal, plinth,
  obelisk, board, face, label) and one canvas texture.

### Restore everything and start fresh

Settings > Debug has a second action next to "Restore all deleted things":
"Restore everything and start fresh" (`startWorldFresh()` in `world.js`).
After a `window.confirm`, because it cannot be undone, it:

- cancels any pending position write and preference sync and blanks the
  position key, so the `pagehide` flush cannot write the old spot back;
- for a signed-in visitor, POSTs `/api/world/preferences` with empty
  `settings` and `settingsUpdatedAt: Date.now()`. The route already keeps the
  newer settings (`incoming >= stored`), so no new endpoint was needed. The
  server holds no per-user hide or delete lists: deleted pieces
  (`forkmesh.world.deletedObjects.v1`), hidden elements
  (`forkmesh.world.disabledElements.v1`), removed and installed packages,
  the Start Here board, the network-ad dismissal and the aquarium toggles
  are all browser storage;
- removes every localStorage and sessionStorage key starting with
  `forkmesh.world.`, `forkmesh.worldComposer.` or `forkmesh-world-`, except
  saved views (`forkmesh.world.savedViews.v1.*`, which the account also keeps
  and has no delete for) and the loader's `forkmesh-world-runtime-reload`
  loop guard. The login (`forkmesh.session`), theme, consent and API origin
  are not World-prefixed and stay;
- reloads.

### Mirror cabinets as working machines

The yard's cabinets are black again: the body was a shiny dark green
(`#17221e`, metalness 0.72, green emissive) that reflected the sky and read
as grey-green. It is now a matte black (`#060707` online, `#0e0f10` offline)
with graphite rails, and both are shared through the resource authority
(`makeSharedMaterial`) instead of one material per cabinet.

**Gears.** `mirrorNodeServices(node)` in `scene/mirror-nodes.js` turns the
record the yard already receives into at most six services: `sync` (online,
integrity not rejected or degraded, not behind), then one gear per operation
family the node advertises in `operations` (`git`, `browse`, `merge`,
`actions`), plus `actions` when the node reports runs and `agents` when it has
agent tasks. The advertised families are healthy while the node is online, not
abuse-blocked, and (if it has an HTTPS endpoint) `endpointHealthy`. The
deploy-status and `/status` feeds carry no per-node service list, so the
signed mirror record is the source. `createMirrorNodeMachines()` draws every
gear in the yard as one `InstancedMesh` (up to 384 instances) of one shared
toothed-disc geometry (`resources.geometry("mirror-node-gear:v1")`, built from
the authority's cylinder primitive), standing in a meshing row along the front
edge of each roof, with neighbours turning in opposite directions. A healthy
gear is brass and spins; an unhealthy one is red and stopped. `layout()` runs
at the end of every `updateNetworkNodes()`, and the rotation is written by one
shared `updateNodeMachines(time)` call from the animation loop, skipped when
the visitor is more than 190 units from the yard, when the element is hidden,
or under reduced motion.

**Sparks.** A spark is a five-segment jagged arc from the gear row down to a
front corner rail, re-jittered every frame for 450 ms. Twelve sparks are
pooled in one additive `InstancedMesh` of the authority's unit box (60
instances, typed-array state, no allocation per spark). Each cabinet keeps a
pending count (capped at 24) that is released at a spacing that spreads the
batch over its window, so a burst reads as a stream.

**Live signal.** No per-request event reached the client before: the only
serve signal was the self-reported `clonesServed` / `websiteServed` counters
in the mirror record, polled every five minutes. The router now adds a cheap
aggregated frame. `_https_mirror_route_advance()` in `app/src/entry.py`, which
already runs once per served upload-pack or website read, calls
`world_note_mirror_traffic(env, node)` (`world/src/world_routes.py`). That
only increments an in-memory per-node counter; at most once every 10 seconds
per process, the request that crosses the boundary posts the whole batch
(`{"at", "nodes": [[node, count], ...]}`, at most 64 nodes, counts capped at
10,000) to the World room's `/api/world/mirror-traffic`, HMAC-signed with the
same control secret as `mirror-push` and rejected when older than 60 s. The
room broadcasts one `{"type": "mirror-traffic", "nodes": [...]}` frame, and
`noteMirrorTraffic()` queues that many sparks on the matching cabinet (by
record name, machine name or node id) over the next 10 seconds. A batch left
over when traffic stops is sent with the next served request after the window.
Sparks are cosmetic, so they are the one effect that trusts the relayed frame
without a signed-record confirmation. Direct-to-mirror clones the core never
sees still arrive through the counters: a served-counter increase on a poll
queues sparks over 60 seconds unless that cabinet had a live frame in the
last minute, so the same traffic is not drawn twice.

Deploy: the gears, colours and poll-driven sparks are World static files. The
live frame needs the App/World Python (router hook, route helpers and the
World room handler) on the core nodes.

**Budget.** The gears added triangles to every pose that sees the yard, so
the gear is a six-tooth disc (24 segments, 96 triangles) and the roof beacon
lost segments it did not need (base and lens 20 to 12 sides, collar torus
8x20 to 4x16), which leaves the arrival pose below the refactor's budget. The
rails and feet kept a per-cabinet trim material: sharing it across cabinets
made them "instancing candidates" in the audit (six or more meshes on one
geometry and material) and pushed that metric over its ceiling. The cabinet
mirror from the scene-audit pass now draws them as instances, so the trim is
shared. The rails, feet
and vent now scale the authority's unit box instead of owning three box
geometries, which pays for the gear geometry in the uploaded-geometry count.

### Merging main's members circle into the modules

`main` changed the monolithic `world-scene.js` after the split (the Custom
avatar ring, band labels on the ground and photo-face retries, `ca89b7942e`).
The merge ports that change into `scene/member-engagement.js`
(`MEMBER_CIRCLE_AVATAR_BAND`, `memberCircleBandSpec`, `memberHasCustomAvatar`,
re-exported from the `world-scene.js` facade), `scene/members-circle.js` (the
portrait band texture and label texture), `scene/districts/members-circle.js`
(layout and labels), `scene/avatars.js` and
`scene/runtime/avatar-selection.js` (retries) and
`scene/runtime/remote-players.js` (custom avatars first). The band labels
share the unit `PlaneGeometry(1, 1)` from the resource authority, scaled per
band, instead of one new plane per band.

### Members circle labels are lettered into the bands

The ground labels on each members-circle band ("CUSTOM AVATAR 8",
"DORMANT 48" and so on) began as bright separate plates. A later change
(branch `feature/world-districts-polish`) moved those plates around the ring
toward the camera every frame. The user wanted neither: the labels should stay
put, run along the middle of their band, and look lettered into the ground
rather than stuck on it. `fix/world-ring-labels-embossed` replaces the plates
with lettering baked into the band surface:

- `memberBandLetteringTexture()` (`scene/members-circle.js`) paints every
  band's text once, into one grayscale canvas covering a strip through the circle from
  its north edge to its south edge. Each band gets two fixed copies on its
  radial centre line, following the curve: one on the town (north) side,
  letters' tops toward the fire so it reads from outside the ring on the
  arrival path, and the same lettering turned 180° about the centre on the
  south side, which reads upright for a viewer south of the circle looking
  north. The background is black, the letters white with a grey edge. The
  canvas is painted at 56 px per unit, scaled down only if a side would pass
  2048 px.
- Each band's existing ring material takes a clone of that texture
  (`memberBandLetteringFor()`, sharing one source and one upload) as both
  `emissiveMap`, with the band's accent at 0.24, which lightens the letters a
  shade in the band's own colour, and `bumpMap` (`bumpScale` 3), which gives
  them a slight raised bevel. The clone's `repeat`/`offset` map the ring's
  planar UVs onto the shared strip, and clamp-to-edge keeps the black border
  outside it, so the band texture is untouched everywhere else.
- No meshes, draws or programs per label: the nine label planes, their
  materials and textures and the per-frame `orientMemberCircleLabels()` are
  gone. The bands gained one shader variant (bump plus emissive map).

Fixed lettering can only read upright from the directions it faces. The
arrival path (north, looking south) and anyone south of the circle looking
north read their copy upright. A camera inside the ring looking outward, as
some seated spawn benches place it, still sees the nearest copy upside down.

The "MEMBERS CENTER" sign is two front-sided planes back to back sharing one
material and texture, so it reads correctly from both sides. The bench
nameplates are oriented radially and read correctly from outside the ring on
every side.

### Copied load log carries the Optimize report and a note to the agent

The load log's "Copy log" (`world-boot.js`) used to copy only the load
lines and the Suggestions. It now copies, in order: the log lines, the
Suggestions, an `=== Optimization audit ===` section, and a closing note
addressed to the AI agent the log is pasted into (`WORLD_BOOT_AGENT_NOTE`,
user-facing text), asking it to improve the diagnostics themselves as well
as fix the findings. `worldBootCopyText()` assembles it.

The audit section comes from the World through
`WORLD_BOOT.setReportProvider(() => this.perfCopyReport())`. `perfCopyReport()`
runs the scene audit if none has run yet (the same ~20-45 ms walk the
Optimize drawer runs) and returns `perfReportMarkdown({ logTail: false })`:
the device, GPU and output line, the frame stats, the new "Scene audit
counts" line (duplicate geometries and materials, instancing candidates,
culling-off objects, visible drawables, shadow casters, GPU geometries and
textures with estimated MB, programs), every finding with its evidence, why
and fix, and, when the profiler has frames, the frame budget, heaviest
callbacks and draws by element. Lists (offenders, callbacks, draws) stop at
20 entries with a count of the rest, and the log-line tail the Optimize copy
button adds is left out because the load log already precedes it. Before the
World exists the section says "Optimization audit: pending".

## Load speed round 3: first view compiled before the reveal (2026-09-29)

The admin production log on main 496bffde30 (RX 580) was interactive at
2.87 s, with 17 long frames and 2.83 s blocked. The worst was one
1,215 ms frame at +1602 ms: `FrameRequestCallback → s @ world-runtime.js:231599`
taking 1,072 ms. The same log showed only 2 of 37 first-view programs primed,
32 still compiling, 1.21 s blocking on first use and 560 draws deferred.

**What the 1,072 ms task was.** `world-runtime.js:231599` maps (through the
deployed `world-runtime.js.map`) to `done` inside `nextBootPaint()`
(`world.js`), the `requestAnimationFrame` callback the populate step awaits.
The callback resolves a promise, so the frame's microtask checkpoint runs the
rest of the populate step synchronously: the pushes into the scene and then
`prepareFirstView()`. A CPU profile of the same load (RX 580, headless)
charged 2.04 s of that frame to one three.js function under
`getUniforms → guardedGetUniforms → primeReadyPrograms → prepareFirstView`:
priming a program whose own `isReady()` was true still reads its uniforms
through a synchronous GPU round trip, and that round trip waits behind every
program still compiling in the GPU process. It was not the roster or the
member lounge (`/api/accounts/users` only landed at the same time).

A second defect doubled the work being waited on. `setPaused(false)` renders
synchronously even when the scene is already running, and `syncRenderPause()`
runs during populate, so the scene was drawn once before the point-light
budget had padded the count to four. Every lit program compiled for three
point lights and again for four: 21 programs of the first view were built
twice.

**First view** (`scene/shader-warmup.js`, `scene/runtime/diagnostics.js`,
`scene/runtime/animate.js`, `scene/core.js`):

- `primeReadyPrograms()` reads no uniforms while any program is compiling.
- `shaderCompileGuard.warm(render, visit)` runs a render in which every first
  use throws (nothing is drawn, nothing is counted as deferred); it only
  creates programs, so the GPU compiles them in parallel with the main
  thread. `warmFirstView()` runs one with the final light budget
  (`updateSceneLevelOfDetail()` first) at the end of construction, and
  `prepareFirstView()` runs one after populate (with the shadow pass, for the
  depth programs of what populate added), then primes once nothing compiles.
  It returns as soon as every program of that pass is primed and every
  first-view plate is painted, hopping between slices with
  `scheduler.yield()` (a `setTimeout(0)` hop stretched the wait by hundreds
  of milliseconds when the main thread was contended); it waits 16 ms only
  while the GPU is still compiling.
- The animation loop does not draw the main view until `prepareFirstView()`
  finishes (`firstViewHoldUntil`, capped at 4 s for embedders that never call
  it), and neither does the construction-time canvas sizing. With a warm GPU
  program cache the reveal draws every first-view program with 0 deferred
  draws.
- `setPaused()` returns early when the state does not change.
- The GPU warm-up of the rest of the World starts only after the first view.
- The canvas is sized synchronously at the end of construction
  (`applyCanvasSize()`) instead of in a later animation frame; resizing the
  drawing buffer behind a queue of compiling programs was a 750 ms wait.

**Construction** (`scene/canvas.js`, `scene/batching.js`,
`scene/districts/office-floors.js`, `scene/quality.js`, `world.js`):

- Every panel canvas created during construction is queued instead of painted
  inline (`deferredCanvasPaints` is on for the whole build). The warm pass
  records which textures the first view draws; `prepareFirstView()` paints and
  uploads (`initTexture`) those in 8 ms slices before the reveal, and the rest
  paint in idle slices afterwards. Art canvases (`canvasArtTexture`: avatar
  faces) still paint inline because their painters return values. A new
  construction-time painter must not depend on its draw callback having run
  when `canvasTexture` returns.
- `mergeObjectBatches` is a generator underneath (`mergeObjectBatchesSteps`,
  same pre-order walk). Construction builds the Office far proxy between
  slices, and the rebuild the first far distance sync schedules runs in idle
  slices and swaps the batches in when complete. That second merge is kept:
  the construction-time proxy carries 708 more triangles of state that
  settles after the build.
- `planWorldRenderer()` derives the boot tier and the context attributes. The
  runtime runs it as soon as three.js and `quality.js` are loaded and builds
  the canvas, the WebGL2 context and the `WebGLRenderer` while the rest of the
  scene modules stream; `createWorldScene({ preparedRenderer })` adopts it
  when its own plan has the same attributes. The tier probe's GPU name is kept
  in `localStorage` (`forkmesh.world.gpu.v1`).
- The sky environment bake (`bakeSkyEnvironment` in `environment.js`, the
  PMREM of the gradient dome) runs on the prepared renderer during the same
  wait; `buildSkyEnvironment` adopts the result once, and a restored context
  bakes again. The 40–130 ms PMREM slice left the scene build.
- The staged `index.html` modulepreloads every module `world-runtime.js`
  imports statically, not only the runtime, so they are requested with the
  HTML instead of after the runtime is parsed and behind the 85 scene-module
  preloads the loader adds.
- The first-view warm-up has its own slice, and `updateIdentity` paints the
  local avatar's badges once instead of twice.

**After the reveal:**

- Members Circle figure jobs all count against the 8 ms lounge slice (jobs
  for figures that already existed used to be exempt, so a roster update
  re-dressed every figure in one task).
- The aquarium places its user fish (arc-length tables for 234 curves) when
  the fish are first shown, not when the roster arrives.
- The seat-label atlas repaints only the cells whose name changed while its
  layout is unchanged (`repaintCanvasRegion`).
- The reward-pool rim title is sized from one measurement instead of a 2 px
  shrink loop over a 4096 px canvas.
- Remaining long tasks after load are single large board paints in the idle
  painter (the Discord and infrastructure boards, 30–100 ms each on this box):
  one `draw` cannot be split.

**Bundled scene.** The module split made a first visit fetch 122 World
resources (85 scene modules in waves; production cca07a77b1 went from 1.75 s to
8.34 s interactive and was rolled back). `stage_world` now bundles the scene
graph with `world/tools/bundle_world_scene.mjs` (esbuild bundle + splitting +
minify, external maps naming the repository paths) into `world-scene.js`, the
small `scene/quality.js` and `scene/environment.js` entries the runtime imports
early, and shared `scene/chunks/*`. Modules outside `scene/` stay separate and
shared with the runtime (their imports become absolute `/world/` URLs), the
loader's scene modulepreloads become the bundle files, and
`FORKMESH_WORLD_BUNDLE=0` stages the unbundled tree. The dev server still
serves the modules as authored. A visit now fetches 42 World resources;
`app/tests/test_split_workers.py` fails if the staged scene grows past a
handful of files.

**Measured** (staged dists served over HTTP/2 with brotli, headless Chromium on
the RX 580, 1600×900, 40 Mbit/s and 20 ms RTT, recorded production API
responses replayed with production-like latency: roster 1.2 s, own profile
1.5 s. Six alternating runs per side, medians, load average under 12.
"Warm" reuses a profile, so the GPU program cache is warm; HTTP caching is off
for both sides because of the self-signed certificate.)

| | 447af730e136 warm | branch warm | 447af730e136 cold | branch cold |
| --- | --- | --- | --- | --- |
| interactive | 1630 ms | 1600 ms | 3273 ms | 2460 ms |
| first frame | 1468 ms | 1205 ms | 1710 ms | 1462 ms |
| worst long frame | 277 ms | 261 ms | 1676 ms | 324 ms |
| long frames, total | 1483 ms | 1667 ms | 3577 ms | 2917 ms |
| scene build work | 544 ms | 348 ms | 599 ms | 473 ms |
| longest build slice | 74 ms | 58 ms | 126 ms | 110 ms |
| first view: programs, primed, compiling | 43, 20, 0 | 36, 36, 0 | 43, 2, 38 | 36, 0, 4 |
| draws deferred during load | 474 | 74 | 668 | 642 |
| blocking on first use | 48 ms | 224 ms | 1555 ms | 4 ms |

The engine phase is longer on the branch (382 vs 252 ms warm) because the
renderer, its context and the sky bake moved into it from the scene build; the
populate step (356 vs 232 ms) now waits until every first-view program is
compiled and primed. On a cold GPU cache the first view still times out at
900 ms with a few programs compiling.

**Pointer hover picking** (`scene/runtime/pointer.js`). `handlePointerMove`
never raycasts or reads layout. An idle mouse move stores the latest pointer
position and asks for one animation frame; that frame does the treasury-sign
hover pick. The pick first checks a cached world bounding box of the sign
(rebuilt only when the sign's world matrix changes) and only then intersects
meshes, so a fast mouse sweep costs one cheap pick per frame at most. The pick
is cancelled on pointerdown and skipped while a button is held or after
dispose. Click, tap, drag and camera handling are unchanged. The production
86 ms `onpointermove` could not be reproduced here (the handler never went
above 5.5 ms); the likeliest cause was the forced layout it did when the
canvas rect was stale during boot.

**HUD avatar face** (`world.js`, `world-avatar-face.js`). The first
`worldTemplate` no longer paints the launcher face. In Chrome the first
`canvas.toDataURL` of a session costs 110–330 ms, and it ran inside the
runtime module's evaluation task. The face is read from localStorage
(`forkmesh.world.hudFace.v1`, keyed by identity and runtime module URL, so
each deploy repaints once); on a miss the badge stays hidden and the face is
painted in an idle callback. Its canvas asks for `willReadFrequently`, which
keeps it in software (`toDataURL` about 1.4 ms). `publicIdentity` reuses the
one cached `Intl.DateTimeFormat` (the first costs about 35 ms of locale data).
The first HUD style and layout (110–150 ms) is one-time startup cost: hiding
the closed panels with `content-visibility` halved the layout objects and saved
nothing measurable, so the template is unchanged.

**Resources.** Office floor and ceiling slabs, facade segments and side walls,
aquarium rubble, campfire coals, organization flowers and swing legs take
their geometry from the resource authority (their arguments come from a small
fixed set), and the desert plot signs join the static instancing pass. The
remaining duplicate materials are per-instance by design (door frames tinted
by access, mirror-node cabinets rebuilt from network data, repository graph
nodes, jetpack and bike parts that are recoloured).

**Not done.** A baked CubeUV texture of the sky PMREM is 184 KB brotli, more
than moving the bake off the critical path saves. Lambert for non-hero
surfaces was left out again for the golden-frame reason in "Resource
authority". Giving every InstancedMesh an instance colour would merge the
instanced and instanced-colour program variants, but only two programs of
the first view. Bundling the 85 scene modules (esbuild `--bundle --splitting`)
would remove most first-visit module requests; the split was measured over
HTTP/2 here and is not on the critical path once the modules are cached.

**Server** (`docs/app/world-boot-latency.md`). The slow `/api/accounts/*` and
World reads were mostly waiting behind the admin ticket's table inventory
(about 256 `COUNT(*)` statements, rerun by several requests at once after
every deploy on 1-vCPU cores). The inventory, the World roster, the viewer's
own profile and wallet balances are now cached and refreshed in the
background; they need a core roll.

## Load speed round 4 (2026-09-29)

- **Shared modules preloaded with the bundle.** The loader's preload list
  now also names the World modules outside `scene/` that the scene bundle
  imports statically (`world-sky.js`, `world-resource-cache.js`, ...);
  before, they were requested only after the bundle was parsed. The engine
  phase dropped from about 360 ms to about 240 ms.
- **Lighter populate.** Mirror nodes, federated instances, bots, the
  Fediverse directory, media spaces, the Mastodon kiosk, the social banners
  and the repository scene are pushed after the first interactive frame
  (`afterWorldInteractive`). First-view plates are painted before the reveal
  only within 70 units of the camera (`FIRST_VIEW_PAINT_DISTANCE`); farther
  plates paint in the idle painter. `prepareFirstView` yields after its warm
  pass. Populate went from about 330 ms to about 140 ms.
- **Renderer preparation in two tasks.** The runtime creates the renderer in
  one task and bakes the sky in the next; the scene trace starts after both,
  so the engine phase owns that wait.
- **QA board.** `updateQaBoard` repaints the existing card canvas
  (`worldQaCardPainter`) instead of allocating and uploading a new
  1200x1050 texture for every snapshot.
- **Fresh arrivals sit down again.** The Members Circle benches are built
  when the roster is processed, which since round 2 happens after the reveal,
  so `seatFreshArrivalAtCampfire` never found a bench and left the visitor
  at the entrance (0 of 11 loads seated on cca07a77b1). The scene exposes
  `campfireSeatsReady()` and `whenCampfireSeatsReady(callback)`; the lounge
  update notifies once seat ownership is assigned, and the runtime seats the
  arrival then unless the visitor has already moved (`freshArrivalMoved`).
- **Deterministic node-yard hiding in the Office.** The enclosure verdict
  used `triangleWeight`, memoized forever, so a node yard measured before its
  cabinets arrived stayed "light" and was never hidden. The verdict (already
  cached per enclosure entry) now uses `countTriangles`, the uncached count.

**Measured** (staged dists over plain HTTP so the browser caches the
versioned modules, recorded API responses served by the test server with
production-like latency, RX 580, 1600x900, 40 Mbit/s and 20 ms RTT, warm
GPU program cache and warm HTTP cache, four runs per side after a warm-up,
medians, load average under 10):

| | 447af730e136 | main f89daa84b7 (round 3) | round 4 |
| --- | --- | --- | --- |
| interactive | 1470 ms | 1440 ms | 1237 ms |
| first frame | 1278 ms | 1062 ms | 1061 ms |
| engine phase | 237 ms | 246 ms | 248 ms |
| scene work | 467 ms | 335 ms | 436 ms |
| populate | 288 ms | 331 ms | 140 ms |
| long frames | 14 (1524 ms) | 14 (1614 ms) | 10 (1217 ms) |
| worst long frame | 302 ms | 271 ms | 273 ms |
| first-view programs primed | 19 of 44 | 36 of 36 | 36 of 36 |
| draws deferred | 514 | 74 | 90 |

An earlier run of the same setup read 1162 ms for round 4 and 1403 ms for
round 3. After merging main 6c5de1b9e9, four more runs per side read 1204 ms
interactive for round 4 (1197-1228; first frame 1033 ms, populate 139 ms,
longest build slice 50 ms, 46 draws deferred) against 1489 ms for
447af730e136. The worst long frame is the runtime module's evaluation: about 65 ms
of script and 170-190 ms of the first style and layout of the HUD (1677
elements, 2512 CSS rules). Inserting the same HUD again after load costs
26-40 ms, so most of it is one-time style-engine work on first use, not the
template's size. Not done: compiling the first view progressively during the
build for a cold GPU cache (a cold cache still reveals at the 900 ms cap),
splitting the few 30-100 ms board paints in the idle painter, and splitting
the HUD stylesheet.

## Load speed round 5 (2026-09-30)

The admin's production log (RX 580, warm cache, 2026-09-30 03:55, rev
e416743eef) read interactive 2.63 s, first frame 1.77 s, loader removed
3.19 s, 19 long frames with 2.77 s blocked, populate 763 ms (139 ms in the
round 4 lab) and ten API calls between 313 ms and 1.14 s.

**What the log's positions were.** The deployed `world-runtime.js` and
`world-scene.js` and their maps map the attributed positions to:

| log entry | source |
| --- | --- |
| `TimerHandler:setTimeout → anonymous @ unknown source`, 192 ms | `prepareWorldRenderer()` in `world.js`: `setTimeout(resolve, 0)` resumes into the sky PMREM bake and the first scene slice. A native `resolve` has no source, so the log could not name it. |
| `Response.json.then @ world-runtime.js:269083` | `fetchJSON()`'s `await response.json()`: the caller's continuation runs in that task. Profiling showed the roster flush into the Members Circle (`flushMemberLounge`, 30–45 ms) and the ticket's follow-up (`updateIdentityUI`, `applyAdminElementsAccess`, `updateSystemCapacityMetrics`). |
| `FrameRequestCallback Sh @ world-scene.js:769409`, 293 ms during populate | the canvas resize handler (`applyCanvasSize` in `scene/core.js`). The admin performance bar appears when the ticket says `isAdmin`, which shrinks the World by 54 px; the drawing buffer is reallocated and redrawn behind the programs still compiling. |
| `TimerHandler:setTimeout @ world-runtime.js:228072`, 132 ms | `markWorldInteractive()`'s timer releasing every `afterWorldInteractive()` waiter in one task: the deferred populate pushes, the roster, the profile card. |
| `import.then @ world-runtime.js:214031`, 48 ms | the WebGL renderer and context creation in `prepareWorldRenderer()`; unchanged. |

**What regressed since round 4 and why.** In the lab, main measured the same
as round 4 (interactive about 1.2 s, populate about 130 ms). The production
regression came from timing that the lab did not reproduce plus features added
since:

- The ticket (600–700 ms on the core) lands while the scene is still being
  placed. `scheduler.yield()` continuations run ahead of network tasks, so the
  ticket and roster handlers queue behind the scene build and run in the gaps
  of populate. The ticket turns on the admin performance bar, the World
  shrinks, and the resize costs 250–330 ms (reproduced in the lab on cold
  runs: `setDrawingBufferSize` 239 ms plus a 72 ms synchronous render).
- The first HUD layout grew with the HUD island and the settings, launcher and
  Office panels: 1,657 elements, of which 675 were laid out although the closed
  panels are `visibility: hidden`. Measured in isolation (the HUD markup
  inserted into a page with `world.css`), the first style and layout took
  219–230 ms while the web fonts were still loading and 79–143 ms once they had
  loaded; the fallback-font layout that is redone when the fonts arrive was
  most of it.
- The Members Circle figure batches (added after round 4) copy four canvas
  layers per member into texture arrays, up to 32 copies per frame at about
  0.9 ms each, and the first copy reads five `UNPACK_*` parameters from the GPU
  (15 ms). On a cold GPU cache the first copy frame took 257 ms.
- More scene elements (drone course, bikes, zip line) made the build longer;
  slices of up to 60 ms were grouped two to a frame.

**Boot and HUD** (`world.js`, `world.css`, `index.html`, `world-loader.js`):

- Closed panels carry `data-world-dormant`, which `world.css` maps to
  `content-visibility: hidden`: the brand menu, detail, account, settings,
  tour, both launchers and the Office lobby, room, chat, task panel and
  check-in. A `MutationObserver` on `data-open` wakes a panel (and any dormant
  panel inside it, for the brand menu) in the microtask after it opens, and
  after the reveal one panel per idle slice is woken so no panel is first laid
  out while a visitor measures or focuses it. The first HUD layout dropped from
  150 ms to about 25 ms in the load trace.
- The two HUD fonts (`Geist-Variable.woff2`, `GeistMono-Variable.woff2`,
  52 KB) are preloaded with the HTML; `ABCFavorit` is only used by closed
  panels and is not.
- The HUD template is inserted after one painted frame, and the event
  bindings and `bootstrap()` run after the next one, so the runtime module's
  evaluation, the template's style and layout and the bindings are three
  frames instead of one.
- The loader warms `Intl.DateTimeFormat` while the runtime downloads (the
  first one cost 12 ms of locale data inside the template task).
- The performance bar and its admin tools keep the layout they had at first
  paint until the World is interactive and the ticket has resolved: shown for
  an account whose last ticket was an admin one (`forkmesh.world.bootlog.v1`)
  or with the debug panel on, otherwise hidden. Before, a returning admin's
  bar was hidden at boot, shown when the ticket arrived (a resize of the World
  in the middle of populate), and a late ticket hid and showed it again.

**Renderer and scene** (`scene/environment.js`, `scene/core.js`,
`scene/scene-build.js`, `scene/shader-warmup.js`, `scene/runtime/diagnostics.js`):

- `prepareSkyEnvironment()` creates the PMREM targets and compiles the dome,
  background, blur and GGX programs (`renderer.compile` into a half-float
  target, so the variants match the bake) one task before
  `bakeSkyEnvironment()`, and the runtime polls `isReady()` for up to 120 ms in
  between. The bake went from 45 to 33 ms of which almost none is link waits.
  It relies on `PMREMGenerator._setSize`, `_allocateTargets`, `_lodMeshes`,
  `_blurMaterial`, `_ggxMaterial` and `_backgroundBox` of three.js r184 and
  falls back to the plain bake when they are missing.
- The scene build yields to a painted frame (`yieldSceneFrame()`:
  `requestAnimationFrame` then a task) once 32 ms of work have run since the
  last frame, instead of `scheduler.yield()`, so a frame holds one slice or two
  short ones. `prepareFirstView()` does the same between its slices.
- The first-view warm pass is budgeted: `shaderCompileGuard.warm(render,
  visit, budgetMs)` skips the remaining draws once 16 ms have passed and
  reports `warmComplete === false`, and construction repeats the pass in later
  slices (up to 12) without recomputing level of detail or matrices. The
  83 ms single pass became three or four passes of 16–37 ms.
- Canvas resizes wait (re-checked every frame, at most 1.5 s) while shader
  programs are compiling, and the resize no longer draws synchronously then.
- Figure batches do not copy layers while programs compile, the per-frame copy
  count follows the measured cost (8 ms per frame, 4 to 32 copies), and three's
  unpack-state cache is seeded so the first copy does no `getParameter` round
  trips.

**Populate and after the reveal** (`world.js`):

- A painted frame separates the populate prelude (identity, arrival stats,
  lounge) from `prepareFirstView()`, and `nextBootPaint()` resumes after the
  frame (`requestAnimationFrame` then a task, 120 ms fallback for hidden tabs).
- `afterWorldInteractive()` waiters resume one per task, and the deferred
  populate pushes (mirror nodes, federated instances, bots, Fediverse
  directory, media spaces, Mastodon, banners, repository scene) are separate
  waiters.
- After the reveal, member lounge flushes and the ticket's HUD follow-up run
  after a painted frame instead of inside the JSON handler.
- The native chat (`dashboard-chat.js`: repositories, agent bots, room key,
  Discord sources, chat retention) loads in an idle slice 600 ms after the
  interactive event instead of on it; opening the chat still loads it at once.

**APIs** (`world.js`; the server side is in
[`docs/app/world-boot-latency.md`](../app/world-boot-latency.md)):

- The visitor's own `/api/accounts/<name>/contributions` (1.07 s on the core,
  six queries) waits until 8 s after the World settles; it only fills the
  chest's contribution list, and the back plate reads the roster's totals
  meanwhile.
- `/api/world/preferences` is read once after the reveal and written back only
  when this browser holds newer settings or saved views
  (`worldPreferencesNeedUpload`); before, every load did a GET and a POST.
- Production showed every request of one load on one core (HTTP/2 keeps one
  connection), queueing behind each other on its single vCPU; fewer calls in
  the first seconds shorten all of them.

**Diagnostics** (`world-boot.js`, `world-debug-log.js`):

- Timers with a native callback (`setTimeout(resolve)`) registered during the
  load are wrapped, and the long-frame line names the caller that queued them
  (`TimerHandler:setTimeout → native callback queued at prepareWorldRenderer @
  world-runtime.js:1561:6450`). Other callbacks keep their own attribution; the
  wrapper is removed when the load settles.
- API lines show the server time from `Server-Timing: app;dur=` when the host
  sends it (`· server 604 ms`), and the summary lists the calls requested
  before interactive with server and network time, flagging those over 300 ms.
- Each settled load stores a compact record (`forkmesh.world.bootlog.runs.v1`,
  last five) and compares it with the previous one: any phase, API or total
  that grew by 50 ms and 30 % is listed ("populate phase 139 ms → 763 ms"),
  with the network bytes of both loads when the cache state differs.
- The copied log starts with a summary (totals, long frames with the worst one,
  phases with scene slices, shaders, critical APIs, the comparison), and runs
  of files served from the browser cache collapse into one line; a warm copy
  went from about 230 to about 170 lines.

**Measured** (staged dists of main c1867a9ee6 and the branch, served by a
Node HTTP/2 server with brotli (quality 11) and the production cache headers
(`?v=` immutable, `/world/*` no-store), Chromium headless on the RX 580
(`--use-gl=angle --use-angle=gl-egl`), 1600×900, throttled to 40 Mbit/s and
20 ms RTT, a signed-in admin, production API responses recorded from
app.forkmesh.com and replayed with the server times of the production log
(roster 834 ms, ticket 685 ms, profile 621 ms, contributions 1.14 s, ...).
Warm reuses a profile after one warm-up load; cold uses a fresh profile, so
both the HTTP and the GPU program caches are empty. Alternating runs, medians,
load average 5–11.)

| | main warm | branch warm | main cold | branch cold |
| --- | --- | --- | --- | --- |
| interactive | 1215 ms | 1106 ms | 2529 ms | 2080 ms |
| first frame | 1030 ms | 922 ms | 1360 ms | 1330 ms |
| loader removed | 1620 ms | 1498 ms | 3016 ms | 2434 ms |
| long frames | 16 | 14 | 16 | 21 |
| blocked in long frames | 1816 ms | 1055 ms | 3280 ms | 2277 ms |
| worst long frame | 260 ms | 109 ms | 814 ms | 333 ms |
| long frames over 100 ms | 8 | 2 | 11 | 8 |
| engine phase | 226 ms | 0–90 ms | 235 ms | 37 ms |
| scene phase | 528 ms | 566 ms | 798 ms | 784 ms |
| longest scene slice | 58 ms | 39 ms | 168 ms | 181 ms |
| populate | 134 ms | 140 ms | 1051 ms | 738 ms |

With the ticket slowed to 1.0 s so that it lands after the reveal (three
alternating pairs): main 1201 ms interactive, 1768 ms blocked, worst 284 ms,
7 frames over 100 ms; the branch 1122 ms, 1255 ms, 120 ms and 2. Before the
last fix the branch still resized the World twice in that case (hide, then
show the bar: 60–125 ms each).

The engine phase reads near zero on the branch because the modules finish
downloading while the HUD template waits for its frame. The scene phase is
longer because slices now wait for a painted frame. On the branch the own
contributions, the agent bots and the chat's other reads start after the load
settles; the roster, the own profile (both prefetched with the HTML), the
ticket and the visitor counts remain before interactive and only get faster
with the server changes.

**Remaining long frames over 100 ms (warm):** the browser's first frame of the
page (100–135 ms, no script, 0 ms blocking: document and GPU process start),
and one or two frames right after the reveal where the first renders of the
populated scene (50–90 ms: idle-painter uploads, first figure copies, new
programs from the deferred pushes) meet the roster's lounge rebuild (27–37 ms);
they read 103–121 ms. On a cold GPU program cache the first-view wait, the
deferred resize and the first renders stay long (up to about 330 ms).

**Not done.** Copying figure layers GPU-to-GPU (a framebuffer blit from the
already uploaded source textures instead of a canvas readback per copy) would
remove about 0.5 s of main-thread work spread over the frames after a roster
arrives. The lounge rebuild (`updateMemberLounge`: ranking, the campfire sign,
the arrival plaque, the Office display) could move its repaints into lounge
tasks. The chat module still asks for Discord sources and four channels when
it loads.

## Post-load round 6: no stalls after the reveal (2026-09-30)

Round 5 left 3–6 gaps of more than 100 ms between presented frames in the
first seconds after the World opens (the worst 0.44–0.54 s on the RX 580). A
new production log (RX 580, live 02903ed2a) added load, Office, reflection,
layout, chat and audit findings. This round covers both.

### How the stalls were found

A scratch harness (`~/.cache/world-r6/`, not in the repository:
`run6.mjs`, `instrument6.js`, `gaps.py`, `gapgpu.py`, `table.py`, `ab2.sh`)
loads the staged dist from an HTTP/2 server with load round 5's production
fixtures and server latencies. It uses a persistent profile for warm runs
(after one warm-up load) and a fresh one for cold runs, at 1600×900, and
records:

- every `requestAnimationFrame` timestamp;
- every `linkProgram`, with the material and object behind the program and
  whether it ran inside a draw, a `renderer.compile` or neither;
- every texture upload of 256² or more, with the source size and the material
  that uses it;
- every Long Animation Frame;
- optionally, a Chrome trace of all processes.

`gaps.py` reports the gaps over 50 and 100 ms in the 10 s after the
interactive mark. `gapgpu.py` lines each gap up with the GPU process's main
thread: program links (`ShaderTranslateTask`, `DoLinkProgram`,
`CompileEvent::wait`), Skia raster of the page, WebGL command flushes
(uploads and draws) and compositor draws.

**GPU access during this round.** The machine's GPU device nodes
(`/dev/dri/renderD128`, `card0`) follow the active desktop user through an
ACL. For most of the round they belonged to the other desktop user, and
headless Chromium fell back to ANGLE on Mesa llvmpipe, which renders the town
at 2–20 fps. Every number in the tables below comes from runs whose
`UNMASKED_RENDERER_WEBGL` read "AMD Radeon RX 580". `runs.sh` checks this
before each run and stops when it is missing. The traces that located the
stalls were taken on llvmpipe, but the mechanisms they show belong to ANGLE and
Chromium, not the driver, and the RX 580 runs confirm them.

### What the traces showed

- **Program links run one at a time in the GPU process.**
  `KHR_parallel_shader_compile` keeps the page from waiting, but ANGLE
  translates both shaders and links on the GPU main thread, one program per
  task. A frame cannot be presented while that thread is busy.
  - The 11 Office far-proxy programs that `warmBeforeReveal` compiled in one
    batch 450 ms before the reveal were still linking 600 ms after it, as
    seven GPU tasks of 50–105 ms each (cold, llvmpipe).
  - The populate pushes (campfire bench labels, engagement bands, figure
    batches, mirror cabinets) and the idle warm-up's batches of four did the
    same.
  - Each batch showed up as one long gap.
- **A large canvas upload is a single GPU task.**
  - The 1536×4096 Mastodon kiosk (25 MB) is one `texSubImage2D`: 136 ms of
    GPU main thread from an accelerated canvas on llvmpipe.
  - The 1536×2560 blog banner, the 2044×2044 capacity board and the 2048×1024
    Office boards behave the same way.
  - The idle warm-up uploaded six textures per slice whatever their size. On
    a warm profile, 30 MB in one slice made a 133 ms gap.
  - A micro-benchmark on the RX 580 with a canvas of the same size: uploaded
    in one call, it made an 83 ms gap from an accelerated canvas, and 65 ms of
    main thread plus a 50 ms gap from a software one. Uploaded as 512-row
    stripes, one per frame, it made no gap. Each stripe cost 8–12 ms of main
    thread from a software canvas and under 1 ms from an accelerated one.
- **First-use reads wait for the whole GPU queue.** The first draw of a program
  reads its uniforms and attributes. That is a synchronous round trip, and it
  waits for every command queued ahead of it.
  - After the populate pushes, the lounge repaints (the campfire's
    bench-label atlas, engagement bands, the arrival box) queued 19 MB of
    uploads inside the main-view draws of one frame.
  - The first-use read behind them waited 33 ms, and that frame took 62 ms of
    main thread.
- **Skia raster of the HUD at the reveal.** On a cold profile only, the page's
  own raster spends 50–95 ms compiling shaders. This remains; see "Not done".

### Programs are paced (`scene/gpu-pacer.js`, `installProgramPacer`)

The pacer wraps these calls on the context instance: `compileShader`,
`attachShader`, `linkProgram`, and the program and shader reads.

- **Until the first view is prepared,** every call passes straight through,
  so the load is unchanged. `prepareFirstView` calls `programPacer.pace()`
  when it is done.
- **After that,** and at any time inside `programPacer.paced(...)`,
  `compileShader` and `linkProgram` are queued. The idle warm-up's compiles
  and `warmBeforeReveal` run inside `paced(...)`.
- **A queued program reports not ready.** `COMPLETION_STATUS_KHR` returns
  false for it. three.js and the compile guard already handle that state: the
  draw is deferred, and `warmMaterialsAsync` keeps polling.
- **One program links per frame.** `programPacer.tick()` runs after each
  frame's render and issues the next program once the previous one has
  completed. Bursts were tried and dropped: letting four go at once after
  links had finished within a frame still met programs missing from the
  program cache, which made a 283 ms gap on a warm profile.
- **Reads force a link.** Any synchronous read of a queued program links it at
  once, so nothing can read an unlinked program. That covers `LINK_STATUS`
  and draws that cannot be deferred: the lobby cube capture or an environment
  bake reading uniforms.
- **Other rules:**
  - A deleted program leaves the queue.
  - Without the extension, the pacer does nothing.
  - The deferred canvas resize waits only for links in flight, then holds new
    links for two frames.
  - Figure layer copies also wait only for links in flight.
  - While a link is in flight, only one upload stripe goes per frame.

### Uploads are budgeted and striped (`createUploadBudget`, `createStripedUploads`)

- **A shared budget.** A token bucket of 6 MB per 16.7 ms is shared by:
  - the idle warm-up's texture uploads;
  - the construction-canvas painter;
  - repaints of resident textures in main-view draws, once programs are
    paced.

  A texture larger than the bucket waits until the bucket is full, then
  empties it.
- **Striped uploads.** Canvas and image textures of 4 MB or more are never
  uploaded in one call. `stripedUploads.begin(texture)`:
  1. allocates the storage (three.js with `source.dataReady = false`);
  2. marks the texture current;
  3. uploads 2 MB row stripes, at most two per frame and within the bucket
     (`UNPACK_SKIP_ROWS`, with the same destination rows for either `flipY`);
  4. generates mipmaps if the texture has them.

  A texture repainted during this is left to three.js.
- **Kiosk and banner repaints.** The Mastodon kiosk and the social banners
  create a new texture on every repaint. `stripedUploads.swapMap(material,
  texture)` keeps the old texture on the board until the new one is fully
  uploaded, then swaps them and disposes of the old one.
- **Repaints in main-view draws.** A main-view draw whose texture was
  repainted (`version` ahead of the uploaded one) uploads it at once only
  while the frame's budget allows (`deferDrawUploads`). Otherwise:
  - the old content stays on screen for a frame or two, marked current;
  - the upload runs after the render within the budget;
  - large textures that the uploader allocated go in stripes.
- **No texture was downscaled.** The boards keep their resolution.

### Load (production log: interactive 1.67 s, 27 long frames, worst 178 ms, scene phase 837 ms, longest slice 43 ms, first-view warm-up 81 ms)

- **The two scene-build timers are both the build itself.**
  `t @ world-scene.js:168568` is where `yieldSceneFrame()` resumes
  (`requestAnimationFrame`, then `setTimeout`).
  `world-scene.js:168295` is the continuation of `yieldSceneBuild()`'s
  `scheduler.yield()`. Both are the scene build's own slices: the first slice
  after each painted frame, and the slices after it.
- **Build frames stay at 32 ms.** A frame holds up to 32 ms of slices of up
  to 12 ms each, and one builder step can overrun it. Capping frames at 16 ms
  (8 ms slices) was tried and reverted: the scene phase grew by 65 ms warm
  and 220 ms cold on the RX 580, because the build then spends that long
  waiting for painted frames (`sceneSlices.yieldedMs` 36–38 → 238–258 ms
  cold) while the GPU is busy with the first view's programs. Those frames
  are behind the loader, and interactive time must not regress. Single
  builder steps still run whole: the Office far proxy, logo fountain and
  aquarium take 20–40 ms each.
- **Resumed first-view passes skip earlier draws.** The first-view warm-up
  already ran as budgeted 16 ms passes. Every resumed pass used to walk and
  reissue all of the earlier passes' draws before reaching new ones. The
  compile guard now remembers the (object, material) pairs a warm session has
  drawn, and resumed passes skip them (`warm(render, visit, budgetMs,
  resume)`).
- **Each pass is its own boot-trace element** (`first-view-warm-2` and so
  on), so the log shows the slices instead of one 81 ms element.
- **Not done: building districts that aren't visible from spawn when the
  visitor approaches.** Most of the remaining build is the Office interior
  (aquarium, logo fountain, lobby and floors). It can't simply move later:
  - the Office far proxy is merged from the whole high-fidelity tower,
    including the interior seen through the glass;
  - a spawn can be inside the Office;
  - building it after the reveal would rebuild and recompile the far proxy
    in exactly the window this round clears.

### Forced layout in `requestAnimationFrame`

`pf @ world-scene.js:785527` is `applyCanvasSize` in `scene/core.js`.

- It consumed the `ResizeObserver` size, then read `getBoundingClientRect()`
  on every later call that had no fresh observation: window `resize` events,
  a deferred touch resize, a context restore.
- Each read forced the HUD's layout inside the frame: 192 ms over four calls
  in the log.
- The observed size is now kept. The layout read only happens before the first
  observation.

### Office interior draws and the lobby reflection

**What the lobby draws were.** A draw probe at the office-lobby pose found:

- 296 draws, 269 of them the Office high-fidelity level;
- 186 of those were the marine aquarium;
- the coral garden alone was about 110 draws. Every coral was a group of 4–17
  small meshes, and 83 of them were transparent `MeshPhysicalMaterial` (the
  soft corals, at opacity 0.86–0.9);
- the sand bed, dunes and rubble were another 11.

**Changes to the aquarium.**

- **Merged static corals and sand.** `mergeMeshesByMaterial(THREE, root)`
  (`scene/batching.js`) merges the meshes of a static subtree that share a
  material into one mesh per material, keeping that material, so the program
  and the look don't change.
  - It applies to the staghorn, plate, brain, sea-fan and soft corals, and to
    the sand bed, dunes and rubble.
  - The sea fans and soft corals still sway as a whole.
- **Anemones and seagrass sway on the GPU.** They animated every tentacle and
  blade on the CPU.
  - Each is now one mesh per material.
  - The vertex shader applies the same rotation the CPU did
    (`onBeforeCompile`, program key `forkmesh-aquarium-sway`).
  - One phase uniform per material drives it.
- **Opaque soft corals.** Their translucency did not show behind the tank
  glass and the water, and it put them in the transparent sort.
- **Result:** lobby draws 296 → 188, transparent draws 116 → 32, aquarium
  draws 186 → 78, geometries 345 → 217. The lobby frame matches main's
  within the golden tolerance.
- **Swaying meshes stay out of the far proxy.** The anemone and seagrass
  meshes have their own program key, so the Office far proxy gave them four
  batches of their own: three more draws at every outside pose (spawn 208 →
  211). They carry `userData.farProxySkip`, and `beginOfficeFarProxy` leaves
  them out; from outside the tower they are a few pixels behind the glass.

**The lobby reflection.** On the base, a capture frame took 87 ms: 65 ms in
the reflection pass, of which 62 ms waited on programs used for the first
time in that pass. The log's "other pass" first-sight compiles are those same
materials (aquarium fish, bubbles, caustics, water surface, particles,
starfish, reef dial, the cube support, the floor warp, the avatar stars).

- **Aquarium decor skips the reflection.** The decor carries
  `userData.logoReflectionSkip`, and both the reflection layer and the idle
  warm-up's reflection scan stop at it. The tank frame, glass, backdrop and
  water stay in the reflection.
- **A face no longer walks the scene.** A face render turns
  `scene.matrixWorldAutoUpdate` and `shadowMap.needsUpdate` off around
  `renderer.render`. It no longer walks about 9,800 objects or refreshes the
  shadow map.
- **Reflection programs are warmed ahead.** Entering the lobby starts
  `warmLogoReflection()`, which:
  1. compiles the reflection's drawables against the cube target through
     `programPacer.paced`;
  2. uploads their maps within the budget.

  The first capture waits for this (at most 3 s), and also waits until the
  cube is on screen.
- **Result (llvmpipe CPU profile, 6 s of looking around the lobby):**
  `updateOfficeLogoReflection` fell from 425 ms to 122 ms. One face is now 61
  draws and about 3 ms.
- **Cause of "scene update 41–46 ms".** It was the reflection pass's
  first-use waits and uploads plus the per-face matrix walk. The main matrix
  walk (`settleMatrixWorld`, about 4 ms a frame on llvmpipe) is unchanged.
- **Other floors.** Engineering and rooftop were already culled to their own
  floor (99 and 104 draws). The lobby's floor slab, ceiling slab and light
  grid, which show from above, are the only cross-floor draws left.

### Production audit and chat

- **Desktop agent bots.** The bots' rolling ball (#14272c) and body (#b8892f)
  built their own materials per robot, and a model change recoloured them in
  place. Both now come from the shared material cache
  (`desktopAgentBallMaterial`, `desktopAgentBodyMaterial`, keyed by the
  model's shell colour), and a model change swaps materials. The status light
  stays per robot, because its glow is animated per bot.
- **Activity rings are not merged.** The avatar crowd's ring materials
  (#4d5f75 ×4, #3f4f67 ×4) are the four phase slots per colour, which keep
  rings pulsing out of step.
  - A trial with one static material per colour for rings that never animate
    was reverted.
  - Live peers do animate their rings, so the crowd detail mirror then held a
    static batch and slot batches of the same colour: members-circle went
    from 87 to 89 draws.
  - Sharing them properly would mean animating every slot material centrally,
    which would start the Members Circle rings pulsing and change the look.
- **Discord refresh in slices.** `dashboard-chat.js:37179` is
  `discoverDiscordSources`, the start of the Discord refresh. The refresh's
  last step appended every message in one task (the log's 4× 245 ms, worst
  81 ms).
  - `refreshDiscordMessages` now yields to a painted frame after each 6 ms of
    appends (`DISCORD_APPEND_SLICE_MS`, `nextChatSlice`), with a 100 ms timer
    so a hidden tab still finishes.
  - Message order and the after-paint pin are unchanged, and refreshes still
    cannot overlap.

### Measured

These are RX 580 runs with the harness above (1600×900, warm and cold
profiles, alternating main and branch). "Gaps" are gaps between presented
frames in the 10 s after the interactive mark. rAF timestamps come in 16.7 ms
steps, so "over 100" means seven frames or more. Main is 5c4386c29. The branch
is the final build; the earlier build with the same changes except the last
pacing fix (single links, one stripe while linking) is shown for more samples.

| first 10 s | main warm (11 runs) | branch warm (3) | earlier build warm (6) | main cold (8) | branch cold (2) | earlier build cold (2) |
| --- | --- | --- | --- | --- | --- | --- |
| gaps over 100 ms, per run | 0–2 (median 2; 13 in all) | 0–1 (2 in all) | 0 | 4–10 (median 6) | 4, 7 | 6, 6 |
| gaps over 50 ms, median | 6 | 4 | 7 | 17 | 18 | 18 |
| worst gap | 100–150 ms | 83–117 ms | 67–100 ms | 500–800 ms | 217–333 ms | 217–250 ms |
| time in gaps over 50 ms, median | 517 ms | 300 ms | 475 ms | 2,292 ms | 1,792 ms | 1,892 ms |
| worst long frame during load, median | 143 ms | 113 ms | 101 ms | 512 ms | 348 ms | 272 ms |
| interactive, median | 1,019 ms | 1,030 ms | 1,020 ms | 2,176 ms | 2,207 ms | 2,196 ms |
| programs at 10 s | 73 | 71 | 72 | 70 | 71 | 69 |

The tour (Members Circle, town, the Office lobby and three floors, the beach,
the desert) after those 10 s:

| tour, warm | main | branch |
| --- | --- | --- |
| gaps over 50 ms | 9 | 9 |
| gaps over 100 ms | 3 | 0 |
| worst gap | 150 ms | 100 ms |
| programs at the end | 95 | 88 |

- **Warm profile.** The gaps over 100 ms after the reveal are gone in 7 of 9
  runs. The two that remain are 100–117 ms, while the kiosk's stripes were
  uploading behind a busy GPU.
- **Cold profile.** Every remaining gap over 100 ms is one program link:
  ANGLE translates and links it on the GPU main thread, 70–120 ms on the RX 580
  with an empty program cache, plus the populate and roster tasks right after
  the reveal. The first ever visit still sees 4–7 of them, but the worst is
  about half of main's.
- **Interactive time is unchanged** within run-to-run noise.
- **Golden frames.** `tools/world_bench.py --gate` on the RX 580 passes: 134
  metrics within budget, and every pose's golden frame within tolerance (at
  most 0.15 %, repository-portals).
- **Budget changes.** `world/perf-budget.json`: the office-lobby draws go from
  296 to 188, and its triangles from 45,816 to 45,892 (the merged aquarium).

### Not done

- **Skia raster of the HUD on a cold profile.** At the reveal, Chromium
  compiles its own shaders for the page's raster (50–95 ms on the GPU main
  thread). It happens once per browser profile and is not WebGL work.
- **First uploads at first sight.** A texture that has never been uploaded
  has no old content to keep showing, so it still uploads inside the draw
  that first needs it. On a cold profile, the leaderboard ground and the town
  paths make one 87 ms frame right after the reveal.
- **Lazy districts.** See "Load" above.

### Diagnostics

- **Copied log order.** The copy now opens with:
  1. the World load summary;
  2. "Optimization suggestions", the in-page panel's own heading (it was
     "Suggestions:", placed after about 140 event lines);
  3. the dropped-lines note and the event lines;
  4. "=== Optimization audit ===" and the agent note.

  The head is built by `createWorldBootLog().text({ summary, suggestions,
  compact })`.
- **"Scene update" is split.** The render-stall monitor keeps its own
  per-frame phase marks, fed by wrapping `worldProfiler.mark`, so it works
  while the profiler is off.
  - A stall record carries `updateParts`: matrix walk, animation, labels,
    camera and proximity, housekeeping.
  - The LONG line reads "scene update 46 ms (matrix walk 30 ms · animation
    12 ms · labels 4 ms)".
- **Instanced mirror batches have an owner.** `forkmesh-instances-N` batches
  are named after a common ancestor that sits outside every World element, so
  they read "Unregistered". Each batch now keeps one of its sources
  (`userData.mirrorOwnerSource`), and `worldElementOwner` resolves a batch
  through it.
- **Long frames without scripts are split.** A Long Animation Frame with no
  script entries is broken down from its own timestamps into:
  - tasks before rendering (GC, parsing, browser work);
  - frame callbacks;
  - style, layout and paint.

  When style and layout dominate and a stylesheet or font finished loading
  within 200 ms before or during the frame, the line names it ("after
  world.css loaded"). The rest, if any, is reported as unaccounted (GPU or
  GC).

## Members Circle figure batches (2026-09-29)

The live optimization report on the RX 580 (main 6c5de1b9e9) read 46 fps,
21.6 ms a frame, CPU 20.5 ms and GPU 1.3 ms. `renderer.render` took 19.9 ms
of it: 1,820 draws, of which the Members Circle figures were 1,631 (363k
triangles, 233 transparent). Each member was seven meshes of its own (torso,
two arms, head, face disc, chest card, name board), each with a per-member
texture (shirt, face, card, name), so none of them could share a draw. The
legs, shoes, antenna, activity ring, pin and patches were already in the
crowd batches (`createAvatarPartInstances`).

`scene/figure-batches.js` (`createFigureBatches`) draws those seven parts for
the whole crowd as seven `InstancedMesh` batches (torso, left arm, right arm,
head, face, card, name board) under the crowd batch group.

- **Per-member textures become layers.** Each texture kind is one
  `DataArrayTexture` with one layer per member: shirt 256², face 128², card
  256² (mipmapped, anisotropy 8, like the card's own texture; scaled by
  `canvasTextureScale` on the compact renderer) and name board 256x128. A
  layer is filled by drawing the member's own canvas or photo into a staging
  canvas and `renderer.copyTextureToTexture` into that layer (the staging
  texture is never uploaded, so three.js takes the direct `texSubImage3D`
  path). The array is allocated with `source.dataReady = false`, so no CPU
  copy of the whole array is kept. At most 32 layers are copied a frame; a
  member stays whole until its layers are in. Source textures are compared by
  identity and `version`, so a repainted card, a new outfit or a photo that
  finishes loading is copied again (each member is rechecked every ~40 frames,
  or at once when it changes state). An unreadable (cross-origin) image keeps
  that member whole.
- **One material per texture kind.** The shirt batch uses the same
  `MeshStandardMaterial` parameters as a member's shirt, the face and card the
  same `MeshBasicMaterial` ones. `onBeforeCompile` swaps `map_fragment` for a
  `sampler2DArray` lookup; the layer index is the instance's `instanceColor.r`
  (a per-mesh attribute, so the source geometries are shared as they are and
  the audit sees no copies), and `vColor` is reset to 1 so it does not tint.
  r184's `vColor` is a `vec4`. The name board is drawn opaque with
  `alphaTest: 0.5` instead of blended, which removes the member draws from the
  transparent sort.
- **Instances are compacted.** Each batch keeps a member list; adding or
  removing a member swaps the last instance into the gap and uploads only the
  changed range. A still, settled member costs one matrix compare a frame.
- **The figures stay in the scene.** Nothing is reparented or removed, so
  picking, the selection box, the Elements panel, seating and every
  `userData` path still use the member's own meshes. `scene.onBeforeRender`
  (after the matrix walk) runs `figureBatches.sync(camera)`, which sets the
  batched members' roots `visible = false`; `scene.onAfterRender` puts them
  back, so between frames every figure is visible and clickable as before.
  Renders from other cameras reuse the main camera's decision.
- **Levels of detail** from pixels per world unit (drawing-buffer height,
  field of view, zoom, figure scale): above 140 (`near`, hysteresis 0.85, at
  most 24 members) the member keeps its own chest card and name board, whose
  reading resolution is larger than a layer, while its body, head and face stay
  in the batches at their native resolution (their meshes are taken out of the
  render through `layers.mask = 0` for that render only); the selected member
  and any card upgraded to reading resolution are always `near`. Below 140 the
  whole member is batched. Members keep their full 32x24 head at every
  distance: people are never simplified by distance (the rule in
  `createAvatar`), so there is no far head level; an earlier 14x10 far head was
  removed before merge. Members that are hidden (compact residency,
  the group culler, the Elements panel) contribute no instances.
- **Eligibility.** A member is batched only if every visible drawable in it is
  one of the seven parts, a crowd-batch source (layer 30) or an invisible
  proxy; the check runs when the member's child counts change and six members
  a frame otherwise. Anything else attached keeps that member whole.
- **Who is batched.** Members Circle figures and live remote peers
  (`remote-players.js`). The roster reserves capacity before the figures are
  built; growing copies the old layers on the GPU.
- **Remote peers at rest** no longer lerp position and heading every frame
  once they are within 1e-4 of the target, so a seated peer settles in the
  matrix walk.
- `getInstancedMirrorStats().figures` and the diagnostics panel's
  `figureBatches` show members, `near`/batched/hidden counts, layer copies,
  draws and the sync's moving-average cost. `setFigureDetailThresholds({ near })`
  on the scene API moves the close-up threshold for inspection.

**Measured** (world bench on the RX 580, 180 directory members and 48 live
peers; the page's own profiler, `cpuMs` is the frame budget total and render
is the `renderer.render` phase; main 77fbd5939f against this branch):

| pose | main draws / tris | branch draws / tris | main CPU / render ms | branch CPU / render ms |
| --- | --- | --- | --- | --- |
| members-circle | 1,680 / 444k | 92 / 235k | 16.0 / 14.9 | 7.8 / 5.9 |
| town-overview | 507 / 126k | 448 / 100k | 14.4 / 12.7 | 10.1 / 9.0 |
| arrival-stats, spawn | 320 / 70k, 327 / 71k | unchanged | 9.1, 9.3 | 9.3, 9.4 |

Bench frame work (`renderMsP50/P90`, all JavaScript in the frame) at
members-circle: 15.1 / 20.8 ms on main, 7.2 / 9.0 ms on the branch.

`world/perf-budget.json`: members-circle draws 1,677 → 91 (triangles stay
at 443,186 with full heads at every distance; the 235k measured above was with
the removed far head); town-overview draws 535 → 338 after merging main's
scene-audit pass, triangles 126,220. The members-circle programs ceiling goes from 117 to 209 (baseline
139, like the other poses at 138-164): the old baseline of 78 came from a pose
too slow for the idle shader warm-up to finish before the capture, not from
fewer materials.

Looking around, a 6 s camera turn at 60 Hz:

| | main | branch |
| --- | --- | --- |
| inside the ring: CPU / render / peak render | 9.9 / 8.6 / 23.9 ms | 7.3 / 6.0 / 14.9 ms |
| inside the ring: draws, frame p95 | 491, 20.7 ms | 157, 19.9 ms |
| from 36 u outside: CPU / render / peak render | 7.8 / 6.4 / 40.9 ms | 5.9 / 4.6 / 15.3 ms |
| from 36 u outside: draws, frame p95 | 342, 26.5 ms | 124, 19.2 ms |

Close-ups of members (front at 2.9 u, 9.5 u, backs at 2.4 u and 10 u, a photo
face on two of them, the overview) differ from main by at most 0.61% of
pixels beyond 32/255 (the fire), and forced-batched close-ups match the
members' own meshes. Clicking a batched member opens the same profile as on
main and seating on a bench lands on the same spot. The golden frames match
(members-circle 0.02%). What is left in a frame at members-circle is general:
about 16 µs of three.js work per draw, the projection walk over ~1,000 visible
objects (~420 meshes), the crowd batch and mirror syncs (~0.7 ms), and the
group culler's per-frame checks. The biggest remaining draw owners are the
mirror node cabinets (26 in view, 131 from spawn) and the 17 ring-colour and
phase materials of the activity rings in the crowd detail mirror.

## Scene audit to zero: geometries, materials, instancing, culling (2026-09-29)

The Optimize panel's scene audit read 56 identical geometries, 65 identical
materials, 30 draws that instancing could remove and 2 objects with frustum
culling off on the live World. The headless bench read 1 duplicate geometry,
because the runtime geometry deduper (`scene/geometry-dedupe.js`) had already
swapped the copies by the time the frozen frame was audited; with the deduper
switched off in the harness main read 55 at every pose, which is what a visitor
sees for the first seconds and after every lazily built district.

**How it was measured.** A scratch harness (not in the repository) ran the
bench driver with two extra init scripts: one wrapped `isBufferGeometry` and
`isMaterial` so every construction kept its stack, and one served
`geometry-dedupe.js` with `sweep` returning at once. A probe after each pose's
capture grouped the scene's geometries and materials the way the audit does
and printed the creating call for each copy; the town-overview pose also toured
the town, the beach and all five office floors before auditing again.

| bench, RX 580, deduper off in the harness | main (`adba972fcf`) | branch |
| --- | --- | --- |
| duplicate geometries, every pose and after the tour | 55 | 0 |
| duplicate materials, spawn / after the tour | 107 / 178 | 12 / 12 |
| instancing groups (draws saved), spawn / office lobby | 4 (26) / 5 (39) | 0 / 0 |
| frustum culling off (visible) | 2 | 0 |
| scene geometries after the tour | 736 | 609 |
| renderer geometries after the tour | 660 | 561 |
| geometry buffers after the tour | 4,369 KB | 3,932 KB |
| scene materials after the tour | 1,693 | 1,529 |

With the deduper running (the gate's own numbers) main reads 1 duplicate
geometry (55 at members-circle, where the sweep had not finished), 107 to 178
duplicate materials, 4 or 5 instancing groups and 2 culling opt-outs; the
branch reads 0, 10, 0 and 0 at every pose. Draws fall from 326 to 223 at spawn,
316 to 213 at arrival-stats, 521 to 416 at town-overview and 115 to 102 at the
rooftop. Golden frames match main's at all nine poses (at most 0.02 % of
pixels, at arrival-stats).

The materials left are avatar parts (operator belts, jetpacks, activity
rings), which belong to the Members Circle batching work, and one plain white
unlit material built separately by the drone race course and the avatar
antenna batch.

**Geometries.** The resource authority (`resources.js`) and the older
`sharedGeometry`/`sharedMaterial` helpers in `world-resource-cache.js` kept
separate maps, so a leaderboard sign built through one and the active
leaderboard built through the other uploaded the same box twice. Both now read
one map (`sharedResourceCache`), and `cloneSharedPlane` asks the authority for
its plane. The copies came from call sites that built a fresh primitive for
every mesh: the reflective logo strokes, aquarium starfish, coral, clams, frame
and fish eyes, bike bars, legacy file cards, promenades and town paths, rooftop
barriers, the elevator panes and the planets' sphere. Those and the other
constant-argument primitives in the same files now go through
`resources.primitive`; the allow-list in `test_world_resource_authority.py`
went from 184 to 100 constructions on this branch (96 after merging main). The storage fill in the network yard is a
unit box scaled in y instead of one box per fill height.

**Audit honesty.** The audit grouped parametric geometries by constructor
arguments only, so a geometry moved after construction, or two Shape/Extrude
geometries (whose arguments are objects), counted as copies of each other. It
now keys each group by the arguments plus a hash of the index and attribute
buffers, cached per geometry and attribute version. Material groups now also
compare polygon offset factor and units, colour write, single-pass, environment
intensity, clearcoat and point size, which tell batch materials apart.

**Materials.** Materials that code recoloured per object now swap between a few
shared materials instead: the office warp-door frames (initial, allowed,
locked), the QA board frame (normal, failed) and the legacy repository file
cards (five colours by three glow levels). Materials nothing changes after
construction come from the shared cache: concrete paths, mirror-node cabinet
bodies, trims, beacons and vents, bikes, landscape flowers, office floor bands,
elevator doors, floor portals, light fixtures, bench seats and legs, campfire
logs. `mergeObjectBatches` shares a batch material between batches that differ
only by render order and between separate batching passes. Expired repository
sparkles were kept hidden with their own material and buffer; they are now
removed and disposed. `sharedMaterialDrift` stays empty after the tour.

**Instancing.** Aquarium bubbles are one `InstancedMesh` with a padded bounding
sphere around their rise column. Swing ropes and seats go through an instanced
mirror set that syncs every frame, so the swinging stays smooth and each rope
and seat stays pickable on the instanced source layer. Ground plaque bases and
slabs are registered by `makeGroundPlaque` and join the static mirror pass on
the world group. Mirror-node cabinets are rebuilt from network data after the
static pass, so their repeated parts (body, rails, feet, beacon base, lens and
collar, vent) join a cabinet mirror on the node interior when a cabinet is built
and leave it when the cabinet is removed.

**Culling.** Stars keep culling on with the bounds of their static positions.
Planets, sun and moon glows and satellites carry a sphere of 1.25 times the sky
radius, so their moving instances are never culled wrongly.

The reef panel's school dial LEDs swap between two shared materials instead of
changing their own. Main's mirror-node machines kept a per-cabinet trim
material to stay out of the instancing count; with the cabinet mirror the trim
is shared.

**Budget.** `world_bench.py --tighten` ran on a quiet box into a copy of
`world/perf-budget.json`, and only the audit metrics (duplicate geometries and
materials, instancing candidates, culling opt-outs) were copied back: every
pose's audit ceilings are now 3 (13 for duplicate materials). Draws kept their
ceilings: the mirrors' draw count moved by a few between runs (spawn 220 to 224)
and main's own edits add draws, so an exact ceiling at the new value would fail
unrelated merges. Triangles at the office
lobby and engineering floor rise by about 130 and 110 where a mirror batch
spans instances that were culled one by one before, the trade described under
"Resource authority"; they stay under their ceilings. The triangle, program and
members-circle draw failures the gate prints are main's own (main reads the
same values).

## Runtime round 5: no hitches in play (2026-09-30)

The admin's live report on the RX 580 (main about 03:55) read 60 fps, CPU
9.4 ms, GPU 1.5 ms, 222 draws, 328 programs (296 in the audit, from 1.5k
materials), 1,883 shadow casters with no shadow draws, and a long-frame list
led by `r @ three.module.min.js via FrameRequestCallback`: 107 frames,
11.6 s in total, the worst 1.8 s. That is the render loop. The island chat
added 729 ms in three `Response.json.then` tasks (418 ms of it forced layout)
and 90 ms of forced layout in a `ResizeObserver` callback.

**How it was measured.** A scratch tour (not in the repository) loads the
real page through the World dev server with the bench's fixtures and page
library, then walks and flies at 22–45 units/s: a turn at spawn, the Members
Circle, the town (repository and leaderboard islands), the Office lobby and
the marketing, engineering and rooftop floors, the beach, the desert plots and
the drone course, with a 360° look at each stop. An init script wraps the
WebGL calls (compile/link, the synchronous reads, texture uploads, buffer
uploads, draws) and `renderer.renderBufferDirect`, records every
`requestAnimationFrame` frame's work, the gaps between frames, the Long
Animation Frames with their scripts, each program created inside a draw
("first sight") with its object, and at the end every program's cache key
decoded into its parameters and the materials that use it.

**What made the long render frames.**

- *The lobby reflection.* The first capture of the Office logo cube rendered
  the whole scene into a cube target with six cameras. Render-target programs
  have no tone mapping and linear output, so every material those cameras saw
  (the 180-unit far plane reached the town, the sky, the desert plots and the
  avatar crowd) needed a second program, and the shader guard only deferred
  the main camera's draws: 27 programs were linked synchronously in one frame,
  1.6–1.8 s, almost all of it `getProgramParameter`.
- *Figure layer copies.* Each member's shirt, face, card and name are copied
  into texture arrays through a staging canvas. The staging canvas was
  GPU-backed, and `texSubImage3D` from a GPU canvas reads it back to the CPU,
  a round trip that waits behind every shader the GPU process is compiling:
  30–640 ms per copy, 13 of the 21 long frames of a baseline tour.
- *Shadow and light variants.* `sun.castShadow` and `shadowMap.enabled` were
  switched off for the zoomed-out camera, the Shadow maps element and the
  cheaper tiers; both are part of every program key, so zooming out rebuilt
  every visible program, and the idle warm-up compiled the flipped variant of
  everything on purpose. Entering the beach hid the lit point lights' parents
  up to half a second (`SCENE_LOD_SAMPLE_MS`) before the point-light budget
  re-padded the count, so the first beach frames compiled their materials for
  zero point lights.
- *Warm-up coverage and cost.* The idle warm-up listed the scene once, 1.5 s
  after the first frame, so everything added later (the populate pushes, the
  roster's figures, the Office far proxy that swaps in after load) compiled at
  first sight. It also compiled variants that are never drawn (raycast
  proxies, instanced-mirror sources on layer 30, invisible materials), and
  every `renderer.compile` walked the whole scene with `traverseVisible` to
  collect lights: 25–30 ms per slice, 368 ms in three seconds of a CPU profile.
  That is the report's `IdleRequestCallback` entry (40 tasks, 422 ms).
- *Program links stall the GPU process.* With `KHR_parallel_shader_compile`
  the main thread no longer waits, but the frame after a batch of links is
  still presented late: in the tour every rAF gap over 100 ms lined up with
  links or large texture uploads in the same interval. Fewer programs, linked
  before they are needed, is the only fix.

**What produced 296 programs.** Decoding the 165 programs of a headless tour
of main: 55 were the shadow-on/shadow-off pairs, 40 were lobby-reflection
render-target variants, 8 were the zero-point-light beach variants; without
the shadow and render-target axes 76 remain. The rest are real permutations:
material type (basic, standard, physical, lambert, points, sprite, depth,
PMREM and the campfire's shader materials), transparent against opaque,
double sided, instancing and instance colour, vertex colours with alpha in the
merged batches, clearcoat, `toneMapped: false` on signs, and the figure
layers' `onBeforeCompile` keys. A live session multiplies the same axes: it
zooms out (every visible program again without shadows), enters the lobby
(render-target variants) and sees more districts before the warm-up reaches
them, which is how the report reached 296–328.

**Changes.**

- *Fixed shadow programs* (`applyShadowQuality`, `render-setup.js`). The
  shadow map and the sun's `castShadow` stay at the boot tier's value; live
  switches (far camera, element toggle, a cheaper tier) set
  `sun.shadow.intensity` to 0 or 1, which is a uniform, and the frame loop
  skips shadow refreshes while it is 0. The image is identical (intensity 0
  multiplies the shadow term away). The flipped-shadow warm-up is removed.
- *Point lights* (`ensurePointLightCount`, every frame before the matrix
  walk). If the visible point lights plus the padding slots differ from the
  tier's budget the budget is re-run at once, so an enclosure switch never
  draws a frame with a different light count.
- *Lobby reflection* (`office-displays.js`). The cube is rendered one face per
  frame and published (PMREM filter, `logoCaptureCount`) on the seventh; the
  six face cameras are deferrable in the compile guard, and a capture that
  deferred any draw is repeated 400 ms later instead of being published with
  holes. The cube camera sees layer 2 only, set at capture time on the Office
  landmark (walls, interior, lobby), the player and every light, and its far
  plane is the tower's width (85 units) instead of 180: 27 render-target
  programs instead of 40, none of them for town, sky or desert. Through the
  glass the reflection shows the scene's background colour instead of a
  256-pixel town.
- *Warm-up* (`scheduleGpuWarmup`, `diagnostics.js`). It rescans the scene every
  4 s in idle slices (1,000 objects per slice), queues unseen material variants
  nearest the player first (the prefetch radius), skips what the main camera
  cannot draw, compiles four per slice against a lights-only target scene
  (`warmTarget()`: the lights are listed once a second, fog and environment
  are read from the scene) and primes ready programs when nothing is
  compiling. Within 150 units of the lobby cube it also compiles the
  reflection's variants against the cube target, restoring each material's
  current program afterwards. Texture uploads stop when the idle slice runs
  out, and the members' own textures are not uploaded (batched members draw
  from the texture arrays). The Office far proxy rebuilt after load is
  compiled (`warmBeforeReveal`, up to 3 s) before it is swapped in.
- *Figure layers* (`figure-batches.js`, `canvas.js`). The staging canvases and
  the members' shirt, face, card and name canvases are CPU canvases
  (`willReadFrequently`, `softwareCanvas()`), so a copy is a CPU draw and a
  plain upload with no read-back. A far member whose layers are not copied
  yet waits hidden (`waiting`) instead of drawing its own meshes: drawn whole,
  its own shirt, face, card and name (about 2 MB) upload at first sight, and
  with CPU canvases a test run uploaded 210 of them, 123 MB, in one frame.
  A member that comes near uploads its own card and name
  within a 4 ms budget per frame and stays batched until they are in. Load
  round 5's copy budget (8 ms, 4 to 32 copies, none while programs compile) is
  kept.
- *Construction canvases* painted by the idle painter are uploaded in the same
  idle slice (`paintDeferredCanvases(jobs, disposed, upload)`), not in the
  first frame that draws them.
- *Shadow casters out of reach* (`parkShadowCaster`). The sun's shadow box is
  fixed around the town centre. The warm-up scan turns `castShadow` off for
  meshes whose world sphere is outside that box (24-unit margin, tested in the
  light's view space) and back on when they return: 201 of the 1,942 casters
  in the tour (the far districts and the beach) no longer take part in the
  shadow pass.
- *Audit to zero* (`world-profiler.js` findings, lobby and all floors): the
  duplicate materials were the jetpack shell, trim and flames (now shared),
  the blinking operator-belt lights (one material per colour and belt slot,
  so members still blink in phase) and the white antenna-bulb batch material
  (now the shared white basic material the drone course uses). The 28
  aquarium food pellets are one `InstancedMesh`; the pellets stay as named,
  invisible poses so the feeding code and its tests are unchanged. The water
  surface waves in the vertex shader (two wrapped phases as a uniform) instead
  of re-uploading 387 vertices a frame, and the SOL fountain sparks are written
  in `onBeforeRender` with `DynamicDrawUsage`, so they cost nothing off screen.

**Diagnostics.**

- `scene/render-stalls.js` (`createRenderStallMonitor`) is always on. It wraps
  the context's upload, compile and synchronous-read calls on the context
  instance (not the prototype) and the draws, and splits every frame over
  50 ms into program compiles (count, the time waiting on links, and the
  material type with its World element), texture uploads (count, MB, the
  three largest by element and object), shadow and reflection draws, and the
  scene update outside the draw. A program created inside a main, shadow or
  reflection draw is a first-sight compile and is recorded with its element.
  `getDiagnostics().renderStalls` carries the last 24 stalls and 64 first
  sights; the admin log writes them as LONG lines ("Render frame 155 ms ·
  draw 137 ms · 93 ms waiting on programs · shadow map 127 draws 2 ms · 11
  texture uploads 23 ms, … (ForkMesh Office · forkmesh-office-far-batch-33
  12 ms, …)") and SHADER lines ("First-sight compile in the other pass:
  MeshStandardMaterial · Office interior (forkmesh-office-aquarium-user-fish-bodies)"),
  next to the browser's LoAF line for the same frame.
- The Optimize report gains "Render stalls", "First-sight compiles" and
  "Programs by World element" (distinct program keys per element, with the
  material types), and the `programs` finding names the top producers.
- The audit lists every duplicate-material group from one copy up (as info
  below 20) with the material's name and colour, and names the objects behind
  buffer re-uploads with their KB/s.

**Chat island** (`dashboard-chat.js`, `world.js`). Every appended message
pinned the log synchronously (`scrollTop = scrollHeight`, then the scroll
thumb read the layout again), and the side list rebuilt itself and called
`scrollIntoView` per message; a Discord refresh appends a batch, so each batch
laid out the whole HUD once per message. The pin, the scroll-thumb sync and
the `ResizeObserver` work now run once, in a task after the next paint
(`afterNextPaint`: a `requestAnimationFrame` then a `setTimeout`), when layout
is already clean; the side list renders once per frame and scrolls its own
box. The World's island feed pin and its `MutationObserver` use the same
after-paint pin. The live log keeps at most 150 rows (retired in batches of
30 into the history window, which already materializes only the visible
rows). `world.spec.js` polls for the pinned feed instead of reading it in the
same task.

**Measured** (headless Chromium on the RX 580, 1600×900, the tour above;
the scratch tour, not the bench):

Two runs per build; each cell is the range of the two.

| | main before (c1867a9ee) | main with load round 5 (c5430ccfe) | this branch |
| --- | --- | --- | --- |
| programs at the end of the tour | 165 | 176–177 | 91–92 |
| first-sight compiles | 57 | 64–65 | 11–12 |
| frames with over 50 ms of main-thread work, in play | 14–15 | 13–19 | 0–1 |
| worst in-play frame (main-thread work) | 1,691–1,788 ms | 1,255–1,736 ms | 0–155 ms |
| gaps between presented frames over 100 ms | 12–14 | 16–17 | 3–6 |
| time spent in gaps over 50 ms | 6.5 s | 6.6–7.7 s | 2.1–2.4 s |
| worst gap between presented frames | 1.69–1.79 s | 1.26–1.74 s | 0.44–0.54 s |
| Long Animation Frames in play | 51–53 | 52–59 | 21–23 |
| frame work, average / p95 | 7.1–7.8 / 11.0–11.3 ms | 7.0–8.1 / 12.2 ms | 6.4–7.4 / 10.8–11.3 ms |
| shadow casters | 1,942 | 1,942 | 1,741 |
| duplicate materials (audit) | 10 | 10 | 0 |
| GPU textures at the end | 464–469 | 1,024–1,125 | 262 |
| chat forced layout in Long Animation Frames | 31–35 ms | 9–10 ms | 0 ms |

The remaining gaps are right after the reveal: the populate pushes add the
mirror nodes, the SOL fountain count, the drone course and the social boards,
whose programs link and whose 1–25 MB canvases upload in the first seconds
(the 155 ms frame is a draw held 1.5 s by the compile guard and then forced
behind those links, plus a shadow refresh).

**Bench** (`tools/world_bench.py`, nine poses):

Main is 3fc90bc9ee (one run, before load round 5; load round 5's main read
the same draws and 132–141 programs at the three poses it was measured on);
the branch is the merge
with c5430ccfe8 (two runs, `--gate`). Golden frames: every pose within 0.09 %
of main's own frames.

| pose | programs | GPU textures | draws | triangles | render p50, main / branch |
| --- | --- | --- | --- | --- | --- |
| spawn | 134 → 71–72 | 386 → 192 | 208 | 68,628 | 6.5 / 6.8 ms |
| office-lobby | 167 → 92 | 402 → 194–195 | 296 | 46,296 → 45,816 | 9.5 / 8.9–9.1 ms |
| office-rooftop | 140 → 83 | 424 → 193–194 | 104 | 25,168 | 4.6 / 4.8 ms |
| office-engineering | 143 → 83 | 416 → 193–197 | 99 | 24,686 | 4.9 / 4.8 ms |
| beach | 140 → 76–77 | 399 → 191–192 | 36 | 5,532 | 2.7 / 2.4–2.7 ms |
| members-circle | 141 → 70–73 | 423 → 191 | 91 → 87 | 443,666 | 5.1 / 5.3–5.5 ms |
| arrival-stats | 138 → 72 | 415 → 191–192 | 200 | 66,930 | 6.0 / 6.6 ms |
| repository-portals | 140 → 73 | 423 → 263–264 | 22 | 70,224 | 3.4 / 3.5 ms |
| town-overview | 138 → 69–72 | 435 → 191–192 | 328 | 124,084 | 8.1 / 9.1 ms |

Render time moved by less than the run-to-run spread: an alternating A/B of
main and the branch at four poses read 5.7–9.8 ms against 6.1–8.3 ms.
`world/perf-budget.json` now holds these programs (policy 1.2/+12 instead of
1.5/+16: the counts no longer depend on how far the idle warm-up got), draws
and triangles exactly, textures at 1.15/+32 of about 192, and 0 duplicate
materials. The members-circle and repository-portals triangle ceilings were
main's own failures (443,666 and 70,224 on main too) and take main's values.
Two budgets were raised: the Office floors' geometries (the count moved
409–448 between runs, and main read 393–437) and spawn's `loadLongTasks`
(22–25 on the branch against 15–16 on main: the warm-up now compiles the
populate pushes and the roster's figures right after the reveal, so the load
window holds more frames of 50–80 ms, while their total fell from 2.6–2.8 s
to 2.4–2.5 s).

The spawn, beach, arrival-stats and town-overview goldens predated the
hexagonal Office tower and failed on main as well (8.6 %, 0.9 %, 6.0 %,
3.2 %). They were replaced with main 3fc90bc9ee's frames after comparing them;
the old images are under `golden/<gpu>/archive/2026-09-30T09-52-33-778292Z/`.

**Not done.** Body level of detail for the avatar crowd: 404k triangles at
distance cost GPU time the RX 580 does not miss (GPU 1.5 ms), and the heads
must stay full. Lambert for matte decor: it adds programs and changes the
image-lit look (see "Resource authority"). The 1536×4096 Mastodon kiosk and
the 1536×2560 blog banner upload 25 and 16 MB after the populate pushes add
them (30–200 ms once, a GPU-to-GPU copy when the canvas stays accelerated);
striping that upload needs three.js internals. Program links still delay the
presented frame in the GPU process even when the main thread does not wait, so
the remaining first-sight compiles right after load (the populate pushes and
the first shadow refresh) show as 100–500 ms frame gaps in the first seconds.
The report's socket `onmessage` entries (7 tasks, 136 ms) were not reproduced
here (the tour's socket never connects).

Tests: `world/tests/test_world_runtime_stalls.py` (the stall monitor in Node
with a fake context: compiles, uploads, shadow and reflection passes, warm
passes excluded, the log lines; the audit's programs by element; source
contracts for the lights-only warm target, shadow parking, figure waiting,
the point-light check and the chat's after-paint reads);
`test_world_render_performance.py` (warm-up rescan, reflection warm-up,
fixed shadow programs, deferrable reflection cameras, the staged capture).
In `world.spec.js` the FM sculpture test now polls for the first published
capture and accepts repeated six-face passes (a deferred pass is retried). Run
over a private-port `http.server` of `app/dist`, that test, the chat replay
test and the two aquarium tests fail on main at the same lines as on this
branch (the chat terminal and aquarium lookups, and the sculpture's avatar
mesh count), so they could not confirm the chat change in a browser; the
tour's Long Animation Frames carry no forced layout from `dashboard-chat.js`
on the branch.

## Quadcopter flight

`updateQuadcopterRide()` in `world-scene.js` flies the craft where the camera
looks, so the mouse (or touch drag) is the stick.

- **Aim.** W/S (or the forward axis of the touch stick) moves along the view
  direction from `cameraYaw` and a flight pitch from
  `quadcopterFlightPitch()`. In first person that is `firstPersonPitch`. In
  the orbit camera it is `cameraPitch` minus the resting orbit pitch
  (`QUADCOPTER_ORBIT_LEVEL_PITCH`, the tilt of `CAMERA_OFFSET`): the default
  chase view looks down about 31°, and flying straight along it would dive
  into the ground on take-off, so the resting view flies level, tilting the
  view down descends and tilting it up climbs. A/D strafe level at
  `QUADCOPTER_HORIZONTAL_SPEED`. Space/C still climb and descend on top.
- **Cruise.** Holding forward accelerates from `QUADCOPTER_HORIZONTAL_SPEED`
  (42) by `QUADCOPTER_CRUISE_ACCELERATION` (90/s since 2026-10-07, was 36/s)
  up to `QUADCOPTER_CRUISE_MAX_SPEED` (150), reached after 1.2 s
  (`quadcopterCruiseSpeed()`). The faster ramp was asked for so holding the
  throttle picks up speed quickly; the top speed is unchanged, so race times
  and the pass test (a segment test, see "Drone racing") are unaffected. Letting go of forward resets the ramp, and
  the craft stops as before. `getQuadcopterState().speed` reports the
  current speed.
- **Heading.** The airframe yaws to the level part of its motion and keeps
  its heading when flying straight up or down.

## Drone race course over the town (2026-09-29, woven 2026-10-07)

The ten-ring course (`forkmesh-drone-race-course`,
`scene/districts/quadcopters.js`) first circled the World centre at radius
500, then a radius-290 loop over the town hexagon. Since 2026-10-07 it weaves
around the ForkMesh coaster (see "ForkMesh coaster"): every ring sits directly
above or below the track, and between two rings the course swings to the side
of the track at track height, so a lap snakes over and under the coaster all
the way round the hexagon.

- **Where.** `coasterDronePoint(angle)` in `scene/coaster.js` maps the course
  angle onto the coaster's plan loop: plan distance
  `DRONE_COURSE_START` (50) + angle × `DRONE_COURSE_UNITS_PER_RADIAN` (the plan
  length over 2π, about 345). The lateral offset is
  `-DRONE_COURSE_WEAVE_WIDTH × sin(5 × angle)` (18), zero at each ring. The
  height is the track's envelope (its height with the inversions flattened out,
  softly floored at `DRONE_COURSE_FLOOR`, 14) plus
  `DRONE_COURSE_WEAVE_HEIGHT × side × cos²(5 × angle)` (14), where `side` is
  the ring's entry in `DRONE_COURSE_RING_SIDES` (+1 above, -1 below).
  `droneCoursePoint()` returns that point and `droneCourseTangent()` its
  numeric derivative; the LOD root still sits on the town tile.
- **Why the sides are a table.** Strict above/below alternation needs five
  rings under the track, and ring 5 lands on the ramp jump, where the
  roof-to-beach zip-line cable crosses the track at about 16 high: a ring
  under the jump would hang on the cable. Rings 4 and 5 are therefore both
  above; rings 1, 3, 6 and 8 are below, where the track was raised to at least
  30 (lift crest, north turn, south-east turn, south turn).
- **Clearance.** Checked against the sampled track in
  `world/tests/test_world_coaster.py`: every ring centre is within 1.5 of the
  track line in plan, at least 9 from any track sample, above or below the
  track by more than 10 on its side, and its bottom is above 2. The
  START · FINISH gate is beside the track (12+ from it). The whole course
  stays at least 8 from the track and 26 from the zip cable; the loops shift
  outward and the corkscrew rolls inward so the weave passes them.
- **LOD.** Unchanged: the coarse batch is chosen by distance to the nearest
  ring centre.

## ForkMesh coaster (2026-10-07)

A roller coaster runs right round the town hexagon. Click the train in the
station (west edge of town) or press E within `COASTER_MOUNT_DISTANCE` (12)
of it; one lap takes about 75 s and ends back in the station, where the rider
steps onto the platform. E (or clicking the train) bails out mid-ride: a
rider high above the ground gets the roof parachute, as when leaving a
quadcopter in the air.

**Files.** `scene/coaster.js` is pure maths (no three.js): the layout, the
sampled track, speeds and the drone weave. `scene/districts/coaster.js`
builds the meshes, `scene/runtime/coaster.js` runs the ride and its HUD.
`core.js` wires them like the quadcopter (`ctx.coaster`, `ctx.coasterRide`,
`rideCoaster()`, `dismountCoaster()`, `getCoasterState()`).

**Layout.** The plan is a rounded hexagon (`COASTER_APOTHEM` 320, corner
radius 80, 2,165 units round), between the old drone ring radius and the town
edge at 365, where the scene audit found nothing standing but the beach
portal and road. Positions are written as plan distance `f` from the start of
the west edge plus a lateral offset (towards the town centre is positive) and
a height. `coasterLayout()` lists the pieces in order:

| f | Element |
| --- | --- |
| 0–70 | Station (height 1.2) |
| 70–250 | Lift hill to `COASTER_LIFT_HEIGHT` (60) on a chain |
| 250–350 | First drop through the north-west corner |
| 400, 440 | Double loop: two radius-15 loops, the first shifting 7 outward, the second back |
| 620–740 | Banked turn round the north vertex |
| 740–955 | Three airtime hills (34, 30, 26) |
| 1080–1115 | Kicker ramp, 30° at the lip (22 high) |
| 1115–1202 | Freefall: no rails, a ballistic arc over the zip cable and the beach road |
| 1202–1227 | Landing ramp, starting at the arc's own slope |
| 1590–1660 | Corkscrew |
| 1830–1855 | Descending helix (radius 24, inward) |
| 2080–end | Brake run through a tunnel into the station |

The jump is a projectile: the lip speed comes from the same energy formula as
the rest of the ride, so following the arc at that speed is free fall, and
the landing ramp begins at the slope the arc arrives with (no kink). The
cable crosses the east edge at about f 1133 and 16 high; the train passes 12.8
above it. The road under the arc (z −94 to −66) gets no supports, and nor does
the portal.

**Track samples.** `coasterTrack()` samples each piece densely, then
resamples every `COASTER_SAMPLE_STEP` (0.5) of arc length into typed arrays:
position, tangent, an up vector (pieces give a reference up: banked for
turns, towards the centre inside loops, the corkscrew and the helix; it is
then made perpendicular to the tangent) and speed. Speed is the chain speed
on the lift, 7 in the station, `sqrt(13² + 2g(60 − y))` with
`COASTER_GRAVITY` 30 elsewhere, and on the brake run v² falls linearly to the
station speed. Nothing after the lift rises above it, so the train never
stalls (minimum 7, top about 60 u/s).

**Meshes.** The track is 12 `THREE.LOD` chunks of about 220 units, each one
mesh with one shared vertex-coloured Lambert material (so the geometry has a
`color` attribute; see Traps). Detailed level: two rails and a spine as
4-sided tubes with a cross-section every 3 units or every 6° of turn, a tie
every 3 units, supports (a column and a footing) every 15 units, the tunnel
shell. Coarse level past `COASTER_DETAIL_DISTANCE` (420): a thicker spine
every 7 units and every other support. About 36,000 detailed triangles in
total. Supports skip inversions, the station, the road and portal, any spot
with other track below, and anywhere the drone course passes under the track.
The station is one mesh (platform on the town side, posts, roof) plus two
label sprites. The train is one `InstancedMesh` of three cars
(`COASTER_CAR_COUNT`, `COASTER_CAR_SPACING` 4.6 apart), `frustumCulled` off
because it moves, one draw call. No shadows from the track.

**Ride.** `updateCoasterRide()` runs first in `walkPlayer()` (before the
jetpack), advances the lead car by the sampled speed in three substeps per
frame (frame time capped at 0.1 s), places the cars and seats the avatar in
the front row, oriented by the track frame, so the rider goes upside down in
the loops and corkscrew. Arms go up in the air on the jump and on steep fast
drops. The camera yaw turns with the track's heading, ignoring frames where
the track is upside down or near vertical so the loops do not spin it. The
HUD shows speed (km/h, 3.6 × units/s) and the name of each element for 2.2 s
as the train enters it. Presence frames go out every 250 ms with the activity
`COASTER_RIDING_ACTIVITY`; other players see the rider's avatar, not the
train, as with the quadcopter. Swings, bikes, the car and quadcopters refuse
to mount while on the coaster, and E checks the coaster before them.

**Cost.** The default-town render workload (the `@critical` spec's numbers)
went from 69 draws / 43,706 triangles to 73 / 47,862. In `world_bench.py` the
coaster is in view from eight of the nine poses; only the draw and triangle
baselines in `world/perf-budget.json` were raised, by exactly what main and
the branch measured apart in the same run (spawn +6 / +3,276, Office poses +3
/ +6,552, members-circle +4 / +9,202, arrival-stats +5 / +2,844,
repository-portals +4 / +8,052, town-overview +8 / +6,956; beach unchanged).
Both runs used SwiftShader: the agent's shell cannot reach 127.0.0.1, so the
bench ran under `unshare -rn` (a private loopback), where ANGLE had no GPU,
and `--update-baseline` would have replaced the RX 580 timings and GPU name,
so it was not used. The RX 580 golden frames will differ for every pose that
sees the coaster; check the diffs and `--bless` on that machine.

## Drone racing (2026-09-29)

Flying a quadcopter turns the ring course into a race. Everything lives in
`scene/drone-race.js` (pure rules: pass detection, lap state, ghost track,
formatting), `scene/runtime/drone-race.js` (per-frame update, HUD, storage)
and the course builders in `scene/districts/quadcopters.js`.

### Controls

- Mount a quadcopter (click it or press E beside it). Race mode starts at
  once: the rings light up, chevrons appear along the course and the race HUD
  shows. Dismounting (E again, or clicking the craft) ends race mode and drops
  any lap in progress; the best time and ghost are kept.
- Steer as usual (see "Quadcopter flight"): W/S along the view, A/D strafe,
  Space climbs, C or Control descends, the mouse or a touch drag aims.
- Fly through the checkered START · FINISH gate to start the lap timer, then
  through rings 1 to 10 in order, then through the gate again to finish. The
  finish starts the next lap straight away.
- Flying back through the gate before all ten rings restarts the lap. A ring
  flown out of order does not count ("Out of order"); flying through a ring or
  the gate backwards shows "Wrong way" and does not count either.

### Course and passes

- `droneCourseGates()` returns the ten rings and the start gate in World
  coordinates, each with a centre, a unit normal along the race direction
  (`droneCourseTangent()`, increasing course angle, the coaster's direction of
  travel) and a pass radius: the
  ring radius `DRONE_COURSE_RING_RADIUS` (8) for rings, `DRONE_RACE_GATE_RADIUS`
  (9.5) for the gate. The gate sits halfway between ring 10 and ring 1, at
  course angle -π/10.
- A pass is a segment test on the craft's movement each frame (the previous
  and current airframe centre, 0.6 above the skids): the segment has to cross
  the ring plane from behind to in front, and the crossing point has to be
  inside the pass radius. The test is on the segment, not on sampled
  positions, so a 150 u/s cruise at a low frame rate cannot jump a ring.
  `gateCrossing()` returns 1 forward, -1 backward, 0 otherwise.
- `advanceRace()` is the lap state machine. Lap times come from the frame
  clock (`requestAnimationFrame` time), so a lap is measured between the two
  frames that cross the gate.

### Look

- **Active rings.** While flying, both ring LOD batches swap their material
  for one shared unlit `MeshBasicMaterial` from the resource authority, so the
  existing instance colours show at full brightness. Passed rings turn dark
  slate, upcoming rings keep the course palette, and the next ring pulses
  between yellow and white through its instance colour. On dismount the
  original per-batch material and colours come back. Only instance colours
  change; there are no per-ring materials.
- **Arrows.** One `InstancedMesh` of unit boxes with one shared unlit material
  draws every chevron: three chevrons before each ring and before the gate
  (spaced `DRONE_RACE_ARROW_SPACING`, 12 units), each made of two box arms and
  tilted up by `DRONE_RACE_ARROW_TILT` so the chase camera sees a V rather
  than an edge. Chevrons for the next target are 1.6 times larger and gold,
  passed ones are scaled to zero, and a brightness wave runs along each group
  of three by rewriting instance colours. A last, larger guide chevron floats
  `DRONE_RACE_GUIDE_DISTANCE` (11) ahead of the craft and points at the next
  ring (or the gate). Instance matrices are rewritten only when the target
  changes, apart from the guide chevron. One draw call in total, hidden when
  not flying.
- **Gate.** One `InstancedMesh` of the same unit box with a shared Lambert
  material: two gold posts and a checkered banner (two rows above the opening,
  one below), coloured per instance. It is always visible, one draw call. A
  "START · FINISH" label sprite above it shows only while flying and farther
  than 50 units from the gate.
- **Ghost.** A deep clone of the quadcopter prototype with every mesh on one
  shared translucent unlit material, no label. It shares geometry with the
  real craft.

### Ghost and best lap

- The current lap is recorded at 10 Hz (`DRONE_RACE_GHOST_SAMPLE_MS`, 100 ms)
  into a preallocated `Float32Array` of x, y, z and yaw (up to
  `DRONE_RACE_GHOST_MAX_SAMPLES`, 6000, ten minutes). A lap that beats the best
  time is copied into the ghost track, and on the next lap the ghost replays
  it, interpolated between samples and turning the short way round.
- The best time and its ghost are stored in `localStorage` under
  `forkmesh.world.droneRace.v1` as `{ bestMs, ghost: { stepMs, samples } }`,
  samples rounded to 0.1. Reads and writes are wrapped in try/catch; a
  missing, blocked or malformed record means no best and no ghost.

### Racing others

- The quadcopter's movement frame (every 250 ms while moving, then the
  existing 1 s presence throttle in `world.js`) carries a small `race` object:
  `ring` (rings passed this lap), `lap` (ms since the lap started, 0 when not
  timing) and `best` (ms, 0 for none). `world.js` bounds it on send and on
  receive (`droneRaceFrame()`), and the scene stores it on the remote avatar
  with the time it arrived.
- The relay (`world/src/world.py`, `sanitize_message()` and
  `movement_delta()`) keeps only those three integers, clamped to 64 rings
  and one hour, and forwards them in the move frame only when present. Any
  move frame without `race` clears it, so a pilot who dismounts drops out of
  the panel on their next step. It is not part of `WORLD_PUBLIC_FIELDS`, so it
  never enters presence snapshots. `world/tests/test_world_drone_race_relay.py`
  covers it.
- The race panel lists up to four other pilots whose race state is fresher
  than `DRONE_RACE_REMOTE_STALE_MS` (15 s): name, rings and their lap time,
  advanced locally from the last frame, plus their best. It refreshes at 4 Hz.

### HUD

The HUD is DOM inside the label layer (`.world-drone-hud`, styles in
`world.css`), built on first flight and hidden when not flying.

- Top centre: a compass strip that slides with `transform` under a fixed
  caret, and the heading in degrees (north is -z, east is +x). The heading is
  the aim direction, the same `quadcopterFlightVector()` the flight uses.
- A crosshair marks where the craft is aiming (80 units along the aim,
  projected), and a green velocity marker where it is actually going (0.9 s of
  smoothed velocity ahead, projected). Both move by `transform` only and hide
  off screen.
- Top left: "Rings n/10", the running lap time and the best time. Below it,
  the race panel for other pilots. A message line under the compass shows
  lap start, ring passes, wrong way, out of order and the finished lap time
  with "New best!".
- Per frame the update writes at most three transforms and only when their
  string changed; text is refreshed at 10 Hz and only when it changed. It
  never reads layout. The viewport size comes from `ctx.viewportRect`.

### Cost

No allocations in the per-frame path (scratch vectors, colours and matrices
are made once). New draw calls: the gate (1, always), and while flying the
chevrons (1), the gate label (1) and the ghost (its prototype meshes, only
during a lap with a ghost). The ring swap does not add draws.
`tools/world_bench.py --check` against main on the same machine: only
town-overview sees the gate (517 → 518 draws, 124066 → 124522 triangles,
inside its ceilings); spawn, members-circle and repository-portals draw the
same counts as main, and the three entries over budget are over on main too.

## Parachute descent (2026-09-29)

The mini parachute that opens on a roof jump or a high dismount now sinks
about twice as fast: `ROOF_PARACHUTE_TERMINAL_VELOCITY` -4.2 → -8.4 and
`ROOF_PARACHUTE_GRAVITY` 4.8 → 9.6. The old descent took about 19.5 s from
80 units and felt like floating; it now takes about 10 s. The speed still
eases toward the cap instead of snapping (at most 0.16 u/s change per 60 Hz
frame), landing still snaps to the floor and collapses the canopy, and
horizontal steering is unchanged. `parachuteSinkVelocity()` in
`scene/movement.js` holds the step so `world/tests/test_world_drone_race.py`
can pin it.

## ForkMesh logo on the town grass (2026-09-29)

The ForkMesh hexagon mark is drawn into the town lawn, very large and very
faint, so it reads from the overview and from drone height and disappears at
walking height.

- **How.** `groundLogoTexture()` in `scene/town.js`, next to the grass it
  tints (a module of its own would be one more fetch outside the preload
  list), paints the mark once from `logo.svg` (the
  dark hexagon, the white top face with the fork graph, the F and M faces, in
  flat greys) into a 256×256 canvas on a neutral 50% grey background, as a
  non-colour (`NoColorSpace`) mipmapped `CanvasTexture`. `applyGroundLogo()`
  patches the existing city grass material (`cityGrassMaterial()`, already a
  unique material) through `onBeforeCompile`: the vertex shader passes the
  World xz, and after `map_fragment` the fragment shader samples the mark at
  `(xz - town centre) / size` and scales the grass colour by
  `1 + fade × (2 × tone − 1)`: the dark hexagon darkens the grass, the light
  faces brighten it, and the grey border is a no-op, so outside the mark the
  lawn is unchanged (clamp-to-edge keeps it grey).
- **Size.** `GROUND_LOGO_APOTHEM` is 0.8 × the town apothem (292), so the mark
  fills most of the town hexagon and lines up with it (both are pointy along
  north/south).
- **Fade.** `fade` = `GROUND_LOGO_STRENGTH` (0.14) × `smoothstep(24, 190,
  cameraPosition.y)`, using the built-in camera uniform: 0 at the default
  chase camera (about 16 up) and on foot, around a third at drone height,
  full from the overview. No uniform is updated per frame.
- **Cost.** No new mesh, draw call, geometry or material: one extra texture
  sample and a few ALU ops in the one lawn shader, and 256 KB of texture
  (about 350 KB with mips). Because it is a colour change inside the existing
  surface there is nothing to z-fight. `customProgramCacheKey` keeps the
  patched program distinct. Without a DOM (node tests) the material is left as
  it was.

## Floor notes

A visitor can leave a note on the floor of the town and anyone can read it
and reply to it.

- **Placing.** A single tap on empty walkable ground (no interactive object
  under the pointer, town mode, not the beach) raises `onFloorTap` from
  `world-scene.js` with the ground point. `world-floor-notes.js` waits 320 ms
  so the second click of a double-click (which dashes the avatar) cancels it,
  then shows an "Add a note here" chip at the cursor. The chip opens the
  composer; posting needs a signed-in account.
- **Showing.** Notes are `THREE.Sprite` cards standing on the floor at their
  saved point, so they always face the camera at no per-frame cost. Only the
  24 nearest notes within 48 units are drawn (one draw call each), and the
  group is hidden inside the Office and on the beach. A tap on a card opens
  its thread. The module is imported after the scene is built, so it stays
  out of the initial module graph.
- **Conversation.** Each note is the root of a thread. Replies are listed
  under it; the author of a note or reply, or an admin, can delete it
  (deleting the note deletes its thread).
- **Tagging.** `@name` in a note or reply pings that account with a
  `mention` ping whose link is `WORLD_ORIGIN/world/?note=<id>`; the World
  opens that thread when it loads with `?note=`. Only existing accounts are
  pinged, at most 10 per post. A reply also pings the note's author and the
  earlier repliers (at most 25 people in all). The composer suggests names
  from the online visitors and the member directory as you type `@`.
- **Live updates.** Every create, reply and delete sends a signed control
  request (`/api/world/floor-notes-changed`, HMAC over the note id and time,
  valid for one minute) to the `ForkMeshWorld` object, which broadcasts
  `{"type": "floor-notes", "id": ...}`. Clients then re-read the list and
  the open thread; the frame itself carries no content.
- **API.** `GET /api/world/floor-notes` lists the 80 most recently active
  notes (public); `GET ?id=<id>` returns one thread; `POST` with `action`
  `create` (`text`, `x`, `y`, `z`), `reply` (`id`, `text`) or `delete`
  (`id`, optional `replyId`). Text is at most 500 characters, positions are
  bounded to the town (|x|, |z| ≤ 1500, −20 ≤ y ≤ 200), an account can keep
  20 notes and post once every 5 seconds, a thread holds 100 replies, and a
  manually blocked connection cannot post.
- **Storage.** `world_floor_notes` and `world_floor_note_replies`
  (migration `0215`). Author and text are in the encrypted `data` column;
  rows are keyed by the author's `account_bi`, and account deletion removes
  the account's notes, the replies under them and the account's replies.

## Bike junction steering, handlebars and gym poses (2026-09-29)

### Controls

While riding a bike: **W / Up arrow** pedals forward, **S / Down arrow**
backs up, and **A / D** or the **Left / Right arrows** steer. Near a road
junction, tapping left or right turns onto the road on that side (holding
for about half a second leaves the road instead, see "Off-road bikes"). On touch
screens the movement stick does the same: push up to pedal, lean it left or
right to pick the branch (the legacy `setControl` D/A keys count too). With
no steering input the bike stays on the road ring it is on. Press **E** or
click the bike again to get off.

### How it works

- **Road graph** (`districts/roads.js`, `createRoadGraph()` /
  `worldRoadGraph()`). Built once, lazily, from `WORLD_HEX_TILES`,
  `WORLD_BIKE_LANE_APOTHEM` and `WORLD_BIKE_LANE_CORNER_RADIUS`: the same
  data `createHexRoadNetwork()` draws, so it cannot drift from the asphalt.
  Nodes are the hex corners on the lane centreline (24: the six inner
  `WORLD_ROAD_JUNCTIONS` and six outer desert junctions have three roads,
  the twelve outer corners two). Edges are hex sides (30 roads, 60 directed
  edges); each directed edge records which hexes it borders.
- **Turns.** For each directed edge the graph precomputes the turns at its
  far node. A turn is allowed only inside a hex's corner, because that is
  where the road is drawn: in the empty sector outside two desert hexes
  there is no asphalt, so that pair of roads has no turn. Every turn is the
  existing corner smoothing: a 60 degree arc of radius 75 tangent to both
  straights, the same arc `roundedHexagonLoopPoint()` rides. Each turn
  carries its side (+1 right, -1 left, from the cross product of the two
  directions) and its hex.
- **Route.** A bike keeps `state.route` = {edge, next turn, hex, s}, where
  `s` runs over the straight (340.6) then the arc (78.5) of the chosen turn.
  `roadRouteAdvance()` moves `s` by speed x time; past the arc it enters the
  next edge, backwards past 0 it re-enters the previous edge through the
  turn that stays on the current hex. `roadRoutePoint()` writes position and
  heading into a reused object. Neither allocates or loops over the graph, so
  a frame costs a few multiplies.
- **Choosing.** The choice window is the last 60 units of a straight
  (`ROAD_TURN_CHOICE_DISTANCE`, 2 to 4 s at bike speed). A steer past 0.3 in
  that window picks the turn on that side and latches until the arc starts,
  so a tap is enough. With no input the bike takes the turn that stays on its
  current hex (the only turn, at a two-way corner). All junctions here are
  Y-shaped with 120 degree arms, so there is no geometric straight branch:
  "straight" means keep following the hex ring you are on, and one of left or
  right always equals it. Riding the town ring with no input reproduces the
  old loop exactly (a test compares it to `roundedHexagonLoopPoint()`
  point for point), and the parked bikes still start from their loop
  fractions via `placeBikeOnLane()`. Speed is untouched, so turns keep speed.
  The bikes ride the road centreline, as the loop did; there is one lane.
- **Input.** `movementInput()` in `runtime/camera-controls.js` adds
  `bikeSteer` = A/D + Left/Right + legacy touch keys + the stick's x, clamped
  to [-1, 1], to the reused `movementInputState` (the field is declared in
  `core.js` so the object keeps one shape). `walkPlayer()` passes it to
  `rideBikeAlongRoad()`; a stopped bike still takes a choice. Parked bikes
  have no rider and do not move, so nothing else depends on the old loop.
- **Cue.** While a turn is pending (inside the window) or under way the bike
  and rider lean 0.09 rad into it and the handlebar turns 0.34 rad that way;
  holding a steer key anywhere turns the bar a little more. Both ease at a
  fixed rate per second. This touches existing transforms only.

### Handlebars and riding pose

- The old handlebar was a flat box level with the top tube, well below where
  an avatar's hands can reach (the avatar is much taller than the bike), and
  the ride pose pitched the arms to -0.72, which on this rig swings the hands
  backwards: avatar fronts face -Z, so a positive pitch reaches forward (the
  handshake and the swing already use positive pitches).
- Each bike now has a `forkmesh-world-bike-handlebar` group: fork, stem, bar
  and two rubber grips, built from `resources.primitive()` cylinders and two
  `resources.material()` materials, so all bikes share the same five
  geometries and two materials. The bikes are separate groups, not
  instanced, so the handlebars are too. The group pivots on the stem and is
  what turns.
- The grips are placed where the arms end: `BIKE_GRIP_Y` / `BIKE_GRIP_Z` are
  computed from the seat height, `seatedAvatarY()`, the shoulder height and
  the arm length at `BIKE_ARM_PITCH` (0.75), so both hands land on the grips.
  `poseAvatarArms()` (`avatars.js`) pitches an arm about its shoulder end
  instead of its centre (the handshake arc), so the shoulder stays in the
  torso. When the bar turns, `bikeGripArmPitch()` lengthens the reach of the
  arm whose grip moves forward and shortens the other.
- Getting off calls `poseAvatarArms(avatar, 0, 0)`, which restores the arms'
  rest positions, and clears the lean and bar angle.

### Gym poses

- **Bench press.** `rotation.x = -PI/2` with the default order tipped the
  avatar forward onto its face. The bench now uses order YXZ and
  (PI/2, PI, 0): head toward the rack, face to the sky, back on the pad
  (`GYM_BENCH_LIE_POSITION` raises the body so the back rests on the pad top
  and puts the shoulders under the bar). The arms point straight up and hold
  the bar; a rep lowers bar and arms together by `GYM_BENCH_PRESS_DEPTH` and
  pushes back up over `GYM_BENCH_REP_MS`. While benching, the bar sits in the
  hands (`GYM_BENCH_PRESS_TOP_Y`); otherwise it stays racked.
- **Other stations** had the same backwards-arm sign: the exercise bike
  reached behind, the rower pulled from behind and the punches went
  backwards. They now use positive pitches through `poseAvatarArms()`;
  `stopGymExercise()` restores the rest pose. The exercise bike and rower
  still stand the avatar rather than seat it; that is unchanged.

### Verification

`world/tests/test_world_bike_junctions.py` checks the graph (node and edge
counts, degrees, every inner junction three-way, edges connected and
reversible), arc tangency, the default route against the old loop, left,
right and no-input at a three-way junction, the choice window, that route
stepping has no allocations, the grip geometry and the bench orientation.
Headless Chromium (ANGLE GL): mounting bike 1 and holding W with Left took
the radial road into the desert (heading 1.05 to 2.09), with Right it stayed
on the town ring (1.05 to 0); screenshots of the ride, the pose from the side
and behind, and the bench press were checked by eye.

## Off-road bikes (2026-09-29)

### Controls

While riding, **hold** left or right (A / D, the Left / Right arrows, the
legacy touch keys, or the touch stick pushed well sideways) for about half a
second and the bike leaves the road. Off the road you can ride anywhere the
avatar can walk: the town grass, the desert hexes, the plazas. Left / right
steer continuously and W / S (Up / Down, the stick's forward push) pedal
forward and back. A short **tap** still does what it did before: near a
junction it picks the branch on that side and never leaves the road.

To get back on, ride onto a road roughly along it: within 3 units of the lane
centreline and heading within 50 degrees of the road's direction. The bike
eases onto the centreline and the lane direction over about half a second and
follows the road again, junction choices included. Crossing a road at a
steeper angle does not snap, so you can ride straight over roads.

### How it works

- **Hold versus tap** (`bikeSteerHold()`, `districts/bikes.js`). A steer of
  at least `BIKE_LEAVE_ROAD_STEER` (0.6) on one side, held for
  `BIKE_LEAVE_ROAD_HOLD_SECONDS` (0.45 s) of frame time, leaves the road. The
  0.6 threshold is above the junction threshold (0.3) so a stick pushed
  forward with a little drift does not throw the bike off the road. Changing
  side restarts the timer. After a snap back the hold is disarmed until the
  steer is released, because the rider is usually still holding the key they
  used to line up.
- **Free riding** (`bikeOffRoadStep()`). Heading turns at
  `BIKE_OFFROAD_TURN_RATE` (1.6 rad/s) times the steer, independent of speed,
  so the bike can also turn while stopped; the step then moves the bike along
  its heading by the same distance the road ride would use (walk speed times
  `BIKE_RIDE_SPEED_MULTIPLIER`). `rideBikeAlongRoad()` keeps its name and now
  takes the frame delta; it routes to the road or off-road step.
- **Collision.** The off-road step writes the bike and `ctx.player`, then
  `walkPlayer()` runs the same checks walking does
  (`activeWalkSurfaceContains()` for the world edge and any ground that is
  not walkable, `constrainTownOfficeWalls()`, each district's `constrainPlayer()`),
  and `applyBikeRidePose()` copies the corrected player position back onto
  the bike. The bike sits on `ctx.currentFloorY`, the floor walking already
  probes each frame, so off-road riding adds no ground ray.
  `walkPlayer()` now copies the bike's pose to the player every frame, not
  only while pedalling, so turning in place off-road shows.
- **Snap grid** (`createRoadSnapGrid()` / `worldRoadSnapGrid()`,
  `districts/roads.js`). Built once, lazily, from the road graph: every
  directed straight (60) and every directed turn arc (84) is put in each
  32-unit cell that could hold a point within 3 units of it (a flat `Int32Array` start/items
  pair, about 70 x 67 cells). `roadSnapFind()` looks at the rider's one cell
  and, for each straight, projects onto the straight part; for each arc,
  checks the angle lies inside the sweep and measures the radial distance.
  It reports `near` (any road within 3 units) and the closest candidate whose
  direction is within `ROAD_SNAP_ANGLE` of the heading. Directed items mean a
  bike facing the other way joins the reverse edge. No allocation per frame;
  the result goes into a reused hit object.
- **Rejoining** (`roadRouteSnap()`). A straight hit sets the route to that
  edge, its first hex and that hex's default turn; an arc hit sets the route
  to the arc's incoming edge with the arc as the chosen turn. From there the
  existing `roadRouteAdvance()` handles the choice window and junctions.
- **No pop.** At the snap the offset between the free pose and the route
  point (x, z and the wrapped heading difference) is stored in `state.blend`
  and added to the route pose, so the first on-road frame draws exactly the
  last off-road pose; the offset then decays by `exp(-4 t)`
  (`BIKE_SNAP_BLEND_RATE`) and is cleared below 1e-3. The sideways slide this
  adds per frame (at most 3 x (1 - e^(-4/60)) = 0.19 units at 60 fps) is
  smaller than the bike's own step.
- **Leaving arms the snap late.** On leaving, the bike is still on the
  centreline and aligned, so snapping is disarmed until the bike has once
  been more than 3 units from every road.
- **Cue.** Off-road the lean and handlebar follow the steer itself
  (`bikeCueSide()`): full steer is the same 0.34 rad bar turn and 0.09 rad
  lean as a junction turn, and `bikeGripArmPitch()` keeps both hands on the
  grips as before.
- **Other clients.** Remote avatars take `x`, `z` and `heading` from the
  movement messages (`setRemotePlayerMovement()`), with no road model, and
  `walkPlayer()` sends the player's position, which the bike now sets both on
  and off the road, so off-road riding needs no protocol change. The activity
  string stays `BIKE_RIDING_ACTIVITY`.
- A bike left off the road stays where it was dismounted and is still off the
  road when mounted again.

### Verification

`world/tests/test_world_bike_offroad.py`: hold versus tap timing (tap 0.25 s
stays, 0.6 s leaves at 0.45 s, a soft 0.4 push never leaves, disarmed after a
snap until released, a side change restarts the timer); a tap on the road
keeps the route and a long press leaves it; the steering integration (a full
circle returns to the start, left and right signs, reverse); the snap test
(aligned and 45 degrees snap, 60 and 90 degrees do not but count as near,
8 units away is neither, facing backwards joins the reverse edge, arcs snap);
the grid against a single-cell brute force on 4000 random probes; a full ride
that leaves, comes back at 30 degrees, snaps with no position or heading jump
beyond one step plus the blend, and settles on the centreline; a steep
crossing and a far parallel ride that do not snap; left, right and no input
at the next junction after rejoining; the off-road cue; and that the frame
path has no allocations and runs before the walk collision.
Headless Chromium (ANGLE GL, `world_dev_server.py`): mounted bike 1, a
0.25 s D tap kept the road, a 0.65 s hold left it; rode on the town grass and
in a desert hex; placed 14 units off a town road heading 30 degrees
in, held W and the bike blended onto the centreline at heading 2.094 (the
edge's direction). Screenshots were checked by eye.

## Office aquarium reef controller (2026-09-29)

The reef controls used to be an HTML disclosure ("REEF CONTROL") projected
next to the tank with `updateScreenLabel()`. On screen it hung in the air
beside the glass and drifted as the camera moved. It is replaced by a
physical controller built in the scene (`scene/aquarium-panel.js`).

### Where it is

A 2.5 x 1.8 housing stands on the tank's front base ledge (y 0.5) at the
lower-right corner as seen from the lobby (tank-local z about -11.8), with its
back against the front glass. It faces +x, towards the lobby. Two chrome feet
are bolted to the front of the base. The panel group is a child of the
aquarium group, so it moves with the tank. The old `controlAnchor` Object3D
is gone, and so is the HTML panel with its CSS.

### Controls

| Control | Kind | What it does |
| --- | --- | --- |
| FEED | push button (amber, lit while feeding) | `aquarium.feed()`: food floats at the surface and sinks slowly for 60 s, and the fish swarm to it |
| TAP | push button (cyan flash) | `aquarium.tapGlass()`: the old tap, where every fish reverses its route and a few come to the glass |
| FOLLOW | push button (green while on) | Follow mode: fish near the cursor over the glass (mouse hover, or the last tap on touch) swim towards it, or away when set to SHY |
| SCHOOL | 4-detent rotary dial | AUTO (the old timed schools by country, browser and agent), SPECIES, COLOUR (six hue bins of the fish body colour) and SCATTER |
| BACKDROP | toggle switch | Opaque or clear backdrop (the old backdrop toggle) |
| GLASS | toggle switch | CURIOUS (fish come to a tap on the glass) or SHY (they flee it) |
| LIGHT | lever with an animated throw (380 ms, slight overshoot) | Tank light on or off (the old light toggle); a green LED shows the state |
| Screen | canvas display | Light state, feeding countdown, school mode (the old "SCHOOL MODE · X ACTIVE" / standby readout), glass, follow and backdrop |

The front glass is clickable too. A click or tap on it makes fish within 6.5
units swim towards the spot (CURIOUS) or away from it (SHY) for 4.5 s.

Every control is picked through the normal pointer path. Each one has an
invisible `raycastProxy` hit pad of at least 0.44 x 0.44 units (the lever's
is 0.6 x 1.3), tagged `interactive: "aquarium-control"`, and the pads are in
`ctx.interactive`. `finishPointer` sends a pad hit to `panel.press(id)` and a
glass hit to `panel.touchGlass(point)`, but only in the lobby. The mouse
hover pick (one rAF-throttled raycast against the panel's targets, only in
the lobby) raises the control's emissive glow and sets
`canvas.dataset.hoverTarget`: the CSS shows the pointer cursor on a control
and a crosshair on the glass. On touch a tap is a click, so the large pads
serve fingers.

The controls stay local, as before. Nothing is sent over the World socket.
Light, backdrop and glass response are saved in `localStorage`
(`forkmesh-world-aquarium-light`, `-backdrop`, `-glass-curious`).

### Fish steering

`scene/aquarium-schooling.js` is a boids step over typed arrays: interleaved
positions, velocities and home points in `Float32Array`s, groups in an
`Int16Array`, and a counting-sort uniform grid (1-unit cells) for neighbour
queries. Arrays are allocated only when the population grows. In SCHOOL
mode each fish steers towards its group centre (blended with a slowly
drifting per-group anchor, so groups spread along the tank), aligns with its
group's mean and nearby velocity, and separates within 0.62 units. SCATTER
uses a wider separation (1.35) and a weak pull to each fish's own route
point. TRACK (AUTO) is a spring towards the route point, so a glass tap or
follow target can bend nearby fish off their curves and let them return.
Soft walls plus a hard clamp keep every fish inside the tank. Six groups of
thirty settle to a radius under 2 units.

`updateAquariumFish` stays one loop. The curve point (and the old formation
target) becomes the fish's home, and its position is blended towards the
steered one by an eased weight (1.4 s in and out). While the weight is 0 the
steering state is re-seeded from the displayed pose, so switching modes never
jumps. Feeding is still layered on top and wins while the fish eat. The step
only runs while the aquarium animation runs: Office lobby, not reduced motion,
fish drawn, and now also while the tank is in the camera frustum or the
visitor is within 4 units (`setInView`, checked at the 12.5 Hz proximity
cadence).

The route tangent is now read from a second `getPointAt` a little ahead,
because three.js `Curve.getTangentAt` allocates two vectors per call. After
warm-up that cut the fish loop's minor GCs from 56 to 36 per 3,000 steps with
150 fish (the rest are V8 number boxes inside three's curve maths).

### Verification

`world/tests/test_world_reef_control_panel.py` builds the aquarium and panel
in Node with a canvas stub. It checks that the panel is a child of the tank,
that its housing sits on the base (y 0.5) against the glass, and that every
visible panel mesh is connected to the tank frame through touching bounding
boxes (nothing floats). The whole panel stays inside the aquarium's walk
footprint plus the avatar radius, and there is no control anchor or HTML
panel. It presses every control and checks feed, tap, backdrop, light (the
lever reaches its off angle), the school readout, the four dial modes,
follow, glass response, hover results, glass touches and hit-pad sizes. It
runs the steering step for groups that stay under 2.2 units, no escapes in
school, scatter or attract runs, an attract target that pulls fish in, and
under 64 KB heap growth over 4,000 steps. A source check keeps `new`,
closures and spreads out of the step and the fish-loop steering code. In
headless Chromium (ANGLE GL, local dev server) real mouse clicks on the
projected pads threw the lever, lit FEED, turned the dial through SPECIES,
COLOUR and SCATTER, and tapped the glass, and the hover target read
`control` over pads and `glass` over the tank. Screenshots of the mounted
panel, the close-up, the lever before and after, and the fish schooled,
scattered and feeding were checked by eye.

## Photo faces, Custom avatar ring and ring labels (2026-09-29)

**Photo faces without a click.** A live peer's `faceImage` was `False` in the
world server's default presence (`world/src/world.py`) until a presence frame
set it, and `remotePlayer` in `world.js` read anything but `true` as an
opt-out. A member whose flag had not arrived (join frame, older client) wore
the generated face, and `updateAvatarBadge` wiped a photo already painted.
Only the click path (`onFediverseProfile`, which fetches `/api/accounts/{name}`
and paints `avatarPng`) put it back. The default is now `None` (unset) and the
client treats only an explicit `false` (the "generated" preference) as an
opt-out. `applyAvatarFaceImage` retries a failed photo load up to three times
(12 s, 24 s, 36 s) instead of leaving the figure on the generated face until
the roster changes, and `dressMemberFace` leaves a URL that is waiting on a
retry alone rather than reloading it on every presence frame.

**Custom avatar ring.** Members with a directory avatar (`avatarThumb`,
`avatarUrl` or `avatarPng`) sit in the innermost ring,
`MEMBER_CIRCLE_AVATAR_BAND`, before the lifecycle bands, so their faces are
the ones closest to the fire. `updateMemberLounge` orders the roster with
photo members first so seat index, slot and figure still line up. Their
benches keep their lifecycle colour and plank label; only the ground band is
the Custom avatar one. The lifecycle counts on the approach sign still count
every member.

**Ring labels.** Every band carries a ground label
(`campfire-member-engagement-label-<id>`) with its name and member count. It
lies in the Town-side entrance gap where no bench stands, readable from the
approach.

**Background glitch.** The sun and moon glow rings (`world-sky.js`) were drawn
with `depthTest: false`, so the glow of the body below the horizon (the moon by
day, the sun by night) showed through the ground and buildings. They now test
depth and are scaled to zero below the horizon. The World element no longer
fades in on its own: it is opaque under the loader and only the loader fades
out, so the reveal no longer washes the scene with the page grey.

## Office roof line: rails, elevator head and air conditioner (2026-09-29)

The five facade columns on the Office front and the four elevator shaft beams
ran the full `OFFICE_TOWER_HEIGHT`, a whole story above the rooftop floor, so
from the roof they stood as bare poles in the sky. The facade columns now stop
at the top of the rooftop glass barrier (`OFFICE_ROOF_BARRIER_TOP`, 3.35 above
the roof). The shaft beams stop `OFFICE_ELEVATOR_SHAFT_ROOF_HEIGHT` (8.4) above
the roof, just clear of the car's ceiling when it is parked on the roof, and
carry a cap slab (`forkmesh-office-elevator-shaft-cap`). A rooftop air
conditioner with two fan grilles sits on the cap
(`forkmesh-office-elevator-air-conditioner`) and a ladder runs up the shaft's
roof-side face (`forkmesh-office-elevator-shaft-ladder`). All three are
opaque, so the shaft still holds no transparent layer; the ladder is scenery
and does not change collision.

## Task signs removed from the hub circle (2026-09-29)

The user asked for two hub-circle signs to go:

- **FORKMESH · WHAT WE'RE BUILDING** (the build board). It was the "Office task
  bulletin" world element: a canvas board built from the frozen
  `WORLD_TASK_BULLETIN_ITEMS` list, its "DONE + DEPLOYED" strip underneath, the
  "▶ WATCH VIDEO" link and the refresh spinner. The strip, link and spinner
  hung off the same group, so they were one sign and went with it. The
  client-side flows that only fed this board went too: the proximity poll of
  `/api/world/build-board`, the repository-issue enrichment behind it,
  drag-to-reorder, drag-to-assign an issue, "DONE → QA", and the
  "Issue assigned to What we're building" and "Build priorities saved" toasts.
  The Dashboard never read this board, so no wider feature lost its UI.
- **HUMAN TODO · FROM CLAUDE + CODEX**, which stood next to it on the same
  group. It was painted from the engineering agent sessions and from the X
  package's setup hint. `updateMirrorAgentTasks` still runs for the agent
  robots; only the repaint went. The "HUMAN TODO" line inside the X banner's
  unavailable state is part of that banner and stays.

The QA board stays. Its "SEND TO TODO" button sent a card back to the build
board, so it went and "SEND TO ISSUES" now sits centred on its own. When a
Codex robot finishes a task it now walks to the QA board, since the finished
task lands in QA. The QA cards that asked testers to check the build board or
the Human TODO board were removed or reworded in `world/src/world_qa.py`.

The server still has `/api/world/build-board` (`world/src/world_build_board.py`)
and the `route_todo` action of `/api/world/qa`. Nothing in the World calls them
now, and they were left in place.

## Infrastructure floor removed; System Capacity moved to Engineering (2026-09-29)

The user asked for the Office's fourth storey, Infrastructure, to go with the
boards on it. `OFFICE_FLOORS` now lists four floors (Lobby, Marketing,
Engineering, Rooftop Patio), `OFFICE_FLOOR_COUNT` is 4 and the Rooftop Patio is
`level: 3`. Everything that stands on or above the roof reads its height from
`officeFloorY("rooftop")` or `OFFICE_TOWER_HEIGHT`, so the patio, roof glass,
zip-line mast, elevator shaft head, air conditioner, ladder, facade columns,
exterior glass and far proxy all moved down one story (the roof slab went from
y 64 to 48, the tower from 80 to 64) without hand-moved numbers. The elevator
panel, the lobby floor-warp doorways and the floor placards are built from
`OFFICE_FLOORS`, so they lost the Infrastructure entry by themselves: the panel
has four buttons in two rows. The mirror racks, the `office-infrastructure`
spawn space and the Infrastructure collision rectangles went with the floor.

What stood on that floor:

- **System Capacity** (durable objects, the D1 STORAGE bar and one bar per D1
  table) is kept. The same platform, with its data flow, click-to-open table
  list and refresh, now stands on the Engineering floor at
  `SYSTEM_CAPACITY_ENGINEERING_POSITION` (-24, 0, 16), clear of the
  workstation row at z 0, the lounge table at (0, 17) and the lounge seats at
  x ±46. The Engineering placard now mentions it.
- **LOCAL CONSOLE** and its switch are removed, together with the console
  patching in `world.js` (`createInfrastructureConsoleCapture`, the redaction
  helper `sanitizeInfrastructureConsoleText` and its re-export from
  `world-loader.js`, the toggle and its toasts). Nothing else read the captured
  entries. The load and debug log (`world-boot.js`, `world-debug-log.js`, copy
  log) and crash reporting are separate and unchanged.
- **WORKER STARTUP FOOTPRINT** and **WORKER COMPONENT + FREE PLAN MAP** are
  removed, and so is their data file `world/public/world/worker-footprint.js`.
  `world/tools/build_worker_footprint.py` stays: it is the deploy-time budget
  gate for the first World module graph and static assets (its Python Worker
  startup-source budget was retired on 2026-09-30)
  (`app/deploy.sh`, `app/wrangler.toml`, `tools/cloudflare_bootstrap.py`,
  `app/tools/deploy_targets.py` and about twenty `app/tests` read its
  `LAZY_MODULES`), and its `--report` output `worker-footprint.json` feeds the
  desktop's Logs → Build tab. Only its chart code (`rendered`, `committed`,
  `needs_refresh` and the drift constants) went.

The server still lists Infrastructure: `OFFICE_FLOOR_TEAM_ALIASES` in
`entry.py` and `office-infrastructure` in `world/src/world.py` spaces. The
client ignores floor ids it does not have, so a server grant for Infrastructure
unlocks nothing, and a saved `office-infrastructure` position is an unknown
space that falls back to the normal spawn.

## HUD island: actions, chat and agent prompts (2026-09-29)

The user asked for four things:

- smaller action buttons;
- one place for chat;
- a way to send a prompt to an agent from the World;
- a quieter top bar.

The bottom-centre avatar-action bar, the bottom-right chat bubble and its
panel are now one `<section data-world-hud-island>` in `world.js`
(`worldTemplate`). It is styled by the `.fm-world .world-hud-island` block at
the end of `world.css`.

### Layout

- **Action row.**
  - The ten avatar actions (wave, jump, super jump, spin, backflip, dance,
    float, wobble, sparkle, jetpack) are a `role="toolbar"` row of 30px
    buttons with 16px icons.
  - Each button has a `::after` hit area that reaches 7px above and below.
    The touch target is 44px tall while the visible chip stays small.
  - The row replaces the old 44px buttons and the phone-only "Actions"
    toggle. `setAvatarActionsExpanded` is gone.
  - Two buttons sit at the right end: open-chat, which opens channels and
    direct messages in `/dashboard/chat` in a new tab, and the expand
    chevron, which carries the unread badge.
- **Feed.** `#fullChatMessages` lives in the island.
  - Collapsed, it is 46px tall and shows the last two messages, one line
    each, pinned to the newest.
  - Tapping the feed or the chevron expands it to `min(44% of the visible
    viewport, 340px)`. Expanded, lines wrap and reactions, attachments and
    the message actions appear.
  - Clicking outside, pressing Escape, or the chevron collapses it.
  - While it is expanded the phone thumbstick is hidden, because the feed
    covers it (`data-world-island-expanded` on the host).
- **Composer.**
  - A Chat or Agent switch, the textarea and one send button.
  - The Task button and its hidden routing selects are gone, so
    `dashboard-chat.js` no longer reads `/api/tasks` at mount to fill them.
  - In Agent mode a chip row appears under the input: repository, model, and
    a Desktop or Runner switch. A run strip shows the last prompt's state.
- **Size and position.**
  - 540px wide on desktop.
  - At 600px and below it is `100vw - 16px` minus the side safe-area insets,
    and the Chat and Agent labels collapse to icons.
  - It sits `max(12px, safe-area-inset-bottom)` from the bottom, plus
    `--world-chat-covered-bottom`: the part of the layout the software
    keyboard covers, measured by `applyViewportMetrics`. So the input stays
    above the keyboard.
  - The textarea is 16px on touch devices, so iOS does not zoom.
  - A `ResizeObserver` writes the collapsed island height to
    `--world-hud-island-height`. The thumbstick and the jetpack controls
    stay above the island through `--world-hud-island-clearance`.
- **Chat cards.** Chat lines no longer raise floating cards, because the
  island feed already shows them. Since 2026-10-09 nothing else does either:
  the activity stream is gone (see
  [Notices are inline, no toasts](#notices-are-inline-no-toasts-2026-10-09)).

### Chat or Agent

The composer has an explicit Chat or Agent switch. The alternative, a target
chip that silently changes what Enter does, was rejected.

Agent mode is visible without reading the chips:

- the island border, the model chip and the send button turn violet;
- the placeholder reads "Prompt your desktop…" or "Prompt a runner…";
- the repository, model and target chips appear.

The mode is not remembered across loads. A reload always starts in Chat, so
a chat message can never be queued as agent work by accident. Guests who
pick Agent get the sign-in panel.

In Agent mode, `bindHudIsland` catches the send click and Enter in the
capture phase and calls `stopImmediatePropagation`, so `dashboard-chat.js`
never sees them. In Chat mode everything goes to `dashboard-chat.js` as
before.

### Sending a prompt

Both targets use the owner agent queue that the dashboard composer already
uses: `/api/repo/<owner>/<repo>/agent-sessions`, handled by
`org_agent_bots_handler` with `owner_scope=True` in
`app/src/route_handlers.py`. No server change was needed.

1. Entering Agent mode (not page load) sends `GET .../agent-sessions` once
   per repository. The answer, which lists `targets` and `models`, is cached
   for ten minutes (`agentTargetCache`, `WORLD_AGENT_TARGETS_TTL_MS`).
2. The two targets are picked from that list, online targets first:
   - **Desktop** is the best target whose `runtimeMode` is `desktop`.
   - **Runner** is the best `runner-pool` or `headless` target: a runner-tier
     node such as `fmnext-r1` that lists the account in `allowed_owners`
     (`_org_agent_pool_target` in `entry.py`), or one of the account's
     personal runners (`_org_agent_personal_targets`). See "Runner target"
     below for what the line says when none can take the prompt.
3. The composer sends `POST .../agent-sessions` with
   `{prompt, provider, model, targetNode}`. The target is always named, so a
   Desktop choice can never fall through to a runner.
   - A desktop claims the queued job through its signed `org-agent-jobs`
     drain. The relay's `cloud-agents` ring on the account event socket wakes
     it (`docs/agents/README.md`).
   - A runner claims it the same way (`server/agents.go`).
4. The 202 answer's `session.status` and `queueState` drive the run strip:
   - "Queued on <node>";
   - "Queued · starts when <node> reconnects", when the desktop is offline;
   - then "Running", "Finished" or "Failed".

   While the run is still queued, the strip re-reads
   `GET .../agent-sessions/<id>` every 5 s, at most 12 times. "Follow" opens
   `/<owner>/<repo>/agents`. A one-line note also lands in the chat feed
   through `notifyChatArea`.

Errors show in the status line under the composer: no target, a 403 on a
repository the account cannot run agents on, an invalid model, or a network
failure.

The repository chip lists, in this order:

1. the last repository used here;
2. the dashboard composer's recent repositories for the account
   (`forkmesh.agent.recentRepositories.v1:<account>`);
3. the account's own repositories from the chat repository list;
4. `forkmesh/forkmesh`.

### Where the model list comes from

The models are the server's `ORG_AGENT_MODEL_ALIASES` (`app/src/entry.py`).

- The GET above returns them as `models: {provider: [alias]}`. The composer
  keeps only the providers the chosen target lists in `providers`.
- They are grouped by provider in a custom listbox
  (`data-world-agent-model-menu`), which works with the arrow keys, Home, End
  and Escape.
- `WORLD_AGENT_MODEL_LABELS` in `world.js` only turns aliases into names
  (`opus` becomes "Opus 5.5"). An alias it does not know is shown as-is.
- Before the first answer arrives, the menu shows the same aliases from
  `WORLD_AGENT_FALLBACK_MODELS`.
- The default is `claude-code:opus`, the server's first Claude alias.
- The last model, target and repository are remembered in `localStorage`
  under `forkmesh.world.agentComposer.v1`. If the chosen target does not offer
  the remembered model, the composer falls back to the default.

### Top bar cleanup

The status pill keeps only the online count. Removed:

- **Prompt** (`copyMcpTaskPrompt`, `mcpPromptOrganization`,
  `worldRemoteMcpPrompt`). It minted a one-day task token with
  `POST /api/orgs/<org>/bot-tokens` and copied an MCP setup prompt. Agent
  mode in the island replaces it.
- **Pings bell** and its count. The Pings board is still the `events`
  landmark, reached from the map entry and the world bulletin board. It
  still reads `/api/poll` and `/api/notifications`, and only while it is
  open.
- **Errors triangle** and everything behind it:
  - its 60 s `GET /api/world/admin/errors?after=` poll
    (`startAdminErrorPolling`, `refreshAdminErrors`);
  - the seen cursor in `forkmesh.world.adminErrorsSeen.v1`;
  - the "New error logged" toasts.

  Admins open the same error board from Settings → Debug → "Logged errors".
- **Tasks checkbox** and its count (`taskCounts` in
  `world-office-tasks.js`). The Work tab in Settings is unchanged.

API calls in the first 60 s after load went from 31 to 29. This was measured
in headless Chromium against the dev server, with a stubbed signed-in admin
session. Two calls stopped:

- `GET /api/world/admin/errors`: one at load, then one a minute, so 60 an
  hour per open admin World;
- `GET /api/tasks`: once per load for any signed-in account, for the Task
  button's routing.

Guests are unchanged. The Prompt button's `POST /api/orgs/<org>/bot-tokens`
only ran on click.

### CSS cleanup

About 3,000 lines of `world.css` were deleted, including nine keyframes. They
were every rule that styled only the retired chat terminal, the channel tabs,
the quick composer, the old avatar-action menu or the removed status buttons.
The message content styles (attachments, reactions, issue form, mentions)
were rewritten compactly for the island feed.

### Runner target: why it said "no runner", Runners panel, sign-in chip (2026-09-29)

**The bug.** With Agent → Runner on `forkmesh/forkmesh`, the status line
said "No ForkMesh runner can run agents on forkmesh/forkmesh yet." while the
pool runner `fmnext-r1` was registered, active, checking in and opted in for
`jett`. The route was right (`forkmesh/forkmesh` is rewritten to its backing
core `fmnext-n1`, which is in `RUNNER_POOL_TARGET_NODES`, and `jett` is on
r1's `allowed_owners`). The cause was r1's expired Claude login:

- `_org_agent_pool_target` counts a provider only when its `loginState` is
  `available`. With Claude expired and Codex not installed, it returned the
  pool target with `providers: []`, `needsLogin: {"claude-code":
  ["fmnext-r1"]}`, `node: "fmnext-n1"` and `online: false` (the last-seen
  time was taken only from signed-in runners).
- `worldAgentTargetFor` drops every target without providers, so the World
  saw no runner at all and printed the generic line.

Production state, read-only on 2026-09-29: `runner_nodes` row
`fmnext-r1` active, pool, `allowed_owners ["jett"]`;
`runner_availability` Claude `loginState: expired` (`reason: job_failed`),
Codex `binaryFound: false`.

**The fix.**

- The pool target now carries `runner` (the runner's name even when none is
  signed in) and `runners`: one entry per opted-in runner with `online`,
  `lastSeenAt`, `ready`, `needsLogin` (expired) and `signIn` (installed, never
  signed in). A runner that is installed but not signed in no longer makes the
  target vanish, and `online`/`lastSeenAt` fall back to any opted-in runner.
- Personal runners the account registered are now targets of their own
  (`_org_agent_personal_targets`) when the repository sits on a pool core.
  Their jobs are pinned (`job.runner`), which `runner_leases.py` already
  honours, so a pool runner never takes them.
- `worldAgentRunnerNotice` turns the answer into one honest line with one
  action:
  - "fmnext-r1 needs Claude reconnected." + **Connect Claude** (opens
    `/dashboard/runners?runner=fmnext-r1&provider=claude-code&connect=1`,
    which starts the sign-in at once);
  - "<runner> needs Codex signed in." + **Connect Codex**;
  - "<runner> has no agent CLI installed yet." + **Manage**;
  - "No runner yet for <repo> —" + **Set one up**
    (`/dashboard/runners?setup=1`);
  - "<runner> is offline, last seen 20 min ago. The prompt waits until it
    reconnects." + **Runners** (opens the panel);
  - an offline runner that also needs a login gets both sentences.
  A send refused with `runner_login_expired` shows the same Connect button.

**Runners panel.** The gear next to Desktop/Runner (`data-world-runners-toggle`)
opens a small dialog above the island: each runner's name, scope, online dot,
last seen, running jobs (`activeLeases`), and per provider "Signed in",
"Signed in · expires in N h", "Needs sign-in" with **Connect**, or "Not
installed". Its data is `GET /api/runners` (runners the account may manage),
read when the panel opens and cached for a minute, plus read-only rows for
shared runners seen in the repository's targets ("An administrator signs this
shared runner in."). The footer has **Set up a runner** and **Manage all**.
Below 430 px the Desktop/Runner switch shows its icons only (each keeps
its title), so the repository chip keeps room next to the gear.
Sign-in runs on the dashboard in a new tab: it needs the terminal relay and
code paste of the Runners page, which the World does not carry.

**Sign-in chip.** For a signed-in account, 4 s after the World is
interactive and then every `pollMs` (5 minutes; also when the tab becomes
visible after that long), the World reads `GET /api/runners/attention`
(`docs/RUNNERS.md`, "Runner attention"). When it lists anything, a red chip
("fmnext-r1 needs Claude sign-in", amber for expiring) sits under the online
pill and links to the item's `connectUrl`; the gear gets a red dot. When the
list changes after a reconnect, the target cache is dropped so the composer
re-reads its targets. Signing out stops the poll and hides the chip. The
chip is one extra read per signed-in load, delayed so it never competes with
the first frame, and private-cached for 60 s by the server.

## HUD island: channel switcher (2026-09-29)

In Chat mode the compose row has a channel chip between the Chat/Agent switch
and the input (`data-world-chat-channel`). It shows the current channel
(`#general` at first) and opens a listbox (`data-world-chat-channel-menu`)
that works like the model menu: arrow keys, Home and End move, Enter or a
click picks, and Escape or Tab closes it. Agent mode hides the chip, and the
row goes back to three columns. On phones (600 px and less) the chip is at
most 84 px wide and truncates with an ellipsis.

### What it lists

- **World.** `#general` and the World room channels (`#world-neighborhood`,
  `#world-broadcast` and the rest). The list is `ForkMeshDashboardChat.spaces()`
  from `dashboard-chat.js`, the same `SPACE_CHANNELS` that the dashboard
  chat's channel select offers. `world.js` keeps only `#general` as a fallback
  before `dashboard-chat.js` loads, because
  `test_world_has_consent_aware_activity_events_and_media` forbids the
  retired landmark names (`sky-campus` and so on) in `world.js`. Guests see
  only `#general` and a "Sign in for more channels and DMs" line, since every
  other room needs an account (`canJoinChat`).
- **Channels** and **Direct messages.** For a signed-in account the menu
  reads `GET /api/chat/channels` and
  `GET /api/chat/direct-messages?limit=12` when it opens, at most once a
  minute. Up to 12 of each are listed; read-only channels (Notices) are
  skipped. DMs show their unread count.

### What picking does

- A World room switches in place. `ForkMeshDashboardChat.switchSpace(space)`
  closes the old socket, drops the room key, seal and every row, thread and
  reaction of the old room, clears the feed, and connects to the new room,
  whose relay replays its history into both the collapsed and expanded
  feed. Send then posts there. The island's placeholder, send label and the
  Chat button's title name the channel. Before this change,
  `dashboard-chat.js` fixed its room at mount from `?space=` and the
  dashboard switched channels by reloading the page. The World cannot reload
  (it would drop the 3D scene and presence), so the room constants became
  variables set by `configureChatSpace`. A `roomGeneration` counter makes a
  connect that was still deriving the old room's key give up, and the
  socket's handlers ignore a socket that is no longer current. An
  organization chat (`?org=`) refuses to switch.
- A private channel or DM opens `/chat` in a new tab with that conversation
  selected. It writes `forkmesh.chat.channel` or `forkmesh.chat.direct`, the
  keys `chat.js` restores its selection from. Those rooms use `chat.js`'s own
  room access, keys and transport, which the island does not load. The
  entries are marked "opens Chat" and carry an arrow. The existing "Open
  channels and direct messages" button is unchanged.

### Remembered channel

The last World room is saved in `localStorage` under
`forkmesh.world.chatChannel.v1` as `{space}`. Reads and writes are wrapped in
`try`/`catch`, so a blocked store just means `#general`. On load,
`loadNativeWorldChat` passes the saved room to `dashboard-chat.js` as
`window.FORKMESH_CHAT_SPACE`, only when the browser has a stored session.
`dashboard-chat.js` accepts it only if it is still one of its
`SPACE_CHANNELS`, and otherwise opens `#general`. A `?space=` in the URL
still wins.

Tests: `world/tests/test_world_chat_channel_switcher.py`.

## GPU warm-up and disposed materials (2026-09-28)

The idle GPU warm-up and the flipped-shadow warm-up call
`warmMaterialsAsync` (`world-scene.js`), not three.js's
`renderer.compileAsync`. three 0.184's `compileAsync` polls its material set on
a timer and reads `properties.get(material).currentProgram.isReady()`. When the
World pages a batch out and disposes its materials mid-compile, `currentProgram`
is gone, and the timer throws `Cannot read properties of undefined (reading
'isReady')`. That surfaced as a World client error group at 21:46 on
2026-09-28, and the `.catch()` on the returned promise cannot catch it because
it throws in a timer callback. `warmMaterialsAsync` runs `renderer.compile`,
then polls itself and treats a released program (no `currentProgram` or no
`.program`) as done (`test_warmup_survives_a_material_disposed_mid_compile`).

## Lazy extras survive a failed module fetch (2026-09-28)

`error_log` had `client unhandledrejection [world] Importing a module script
failed.` on 2026-09-19. That is WebKit's text for a dynamic `import()` that
could not be fetched. `world-loader.js` already reloads once when
`world-runtime.js` fails that way, but `index.html` loaded
`world-discord.js` and `world-notes.js` with `void import(...)`, and
`world-discord.js` loaded `world-discord-panel.js` with no rejection handler.
A dropped connection or a deploy between shell and module turned into an
unhandled rejection, and the Discord loader kept the failed promise, so the
panel could not open again until a reload.

- `index.html` `loadExtra` catches the failure and tries once more after 5 s
  with `?retry=1`, because a browser can cache a failed module-map entry for the
  same URL. A second failure is logged to the console: both extras are
  optional, and the World runs without them.
- `world-discord.js` clears `panelPromise` on failure, so the next click tries
  again, with `?retry=<n>`. `openPanel` catches the failure. If the preload for
  a `?discord` return fails, the normal click and event entry points are bound
  instead.

Tests: `world/tests/test_world_lazy_module_failures.py`.

## World crash reports after 2026-09-24

The `World crashed and reloaded; previous session ended without pagehide`
groups (2026-09-03..23; Mali-G57, Apple M1, Iris Xe, iPhone) all predate the
2026-09-24 cutover to the core nodes and the render-cost and heap work
(`9fe13756b`, `06e58f88e`, `d9d7d31a5e`, `296b1a4cb3`). No such report has
arrived since 2026-09-23 18:08, though the World has had visitors (the core
logs for 09-27 and 09-28 alone show 55 `world.js` and 38 `world-scene.js`
loads) and the reporter (`world.js`, `/api/client-errors`) is unchanged.
Between `7cc28ee45` (2026-09-23) and `cac169194e` (2026-09-24) the scene did
not start at all, so that window cannot count as crash-free evidence. The
groups are treated as obsolete.

A report still distinguishes a session that ended while hidden (salvaged
from `fix/web-server-errors`, 2026-09-28). The heartbeat records
`document.visibilityState`, and `handleVisibility` now beats on every
visibility change, so the record always says whether the page was hidden
when it stopped. The render loop is paused while hidden, so a session whose
last beat says `hidden` was closed by the browser in the background (a mobile
browser reclaiming the tab, which does not always set `document.wasDiscarded`),
not killed by the renderer. It is reported as "World reloaded after the browser
closed it in the background" and, like a discard, does not count toward the
safe-mode reboot; a session that stopped while visible is still "World crashed
and reloaded". The beat on becoming visible keeps a crash right after the tab
returns from being read as a background close.

Since 2026-10-09 the World sends both of those reports as kind `discard`
instead of `crash`. `client_error_handler` (`app/src/entry.py`) still stores
them in `error_log`, but with `notify=False`, so they raise no "New javascript
error group" ping. Each report carries its own device and scene readings, so
until then every one of them opened a new group and pinged. The ping of 2026-10-09 05:29
(group 520, `client crash [world] World reloaded after the browser closed it in
the background`, Apple M4, desktop pointer, 16 GB) was one: a desktop browser
dropped a hidden tab without `pagehide`, most likely when the browser quit and
restored its session or when the tab was discarded to save memory. The group can
be deleted from `/admin?table=error_log`. A session that stopped while visible
is still kind `crash` and still pings.

## Lobby kiosks moved to the website (2026-09-29)

Four Office lobby props were removed from the World and now live in the app
dashboard, where they are ordinary pages that work on phones and need no
WebGL:

| Removed from the lobby | Now on the website |
| --- | --- |
| Feedback kiosk (`forkmesh-office-feedback-kiosk`) | "Send feedback" in the dashboard account menu and sidebar footer |
| Task bounty desk (`forkmesh-office-task-bid-kiosk`) | `/dashboard/bounties` |
| Link Lab (`forkmesh-office-link-kiosk`) | `/dashboard/links`, "Submit a link" |
| Link rewards board (`forkmesh-office-link-rewards`) | `/dashboard/links`, "Recent links and rewards" |

The Link rewards board was the live lobby screen beside the Link Lab: the
four newest public link submissions with their submitter, reach score,
potential visits, the tiny SOL appreciation estimate, paid or unpaid state and
a QR code of the submitter's Solana address. Clicking it opened the Link Lab.

What went with them: four stands and four canvas faces (1080×660 twice,
1280×740 and 1400×900, about 3.6 million texels), their click targets in
`scene/runtime/pointer.js`, the lobby proximity entry in
`scene/districts/office-displays.js`, `linkSubmissionRewardTexture` in
`scene/leaderboards.js`, and the three dialogs and `loadLobbyLinkBoard` in
`world.js`.

Requests the World no longer makes:

- `GET /api/world/link-kiosk`, fetched each time a visitor walked up to the
  Link Lab (at most once a minute) and again whenever the dialog opened;
- `POST /api/world/link-kiosk` (submit a link);
- `POST /api/feedback` with `source: "world"`;
- `POST /api/tasks` with `kind: "bid"`, and the Office task refresh that
  followed it.

The server APIs are unchanged apart from the new `web` feedback source
([docs/app/feedback-bounties-links.md](../app/feedback-bounties-links.md)).
Founder emails already sent link to `/world/?landmark=office&feedback=1`;
the World now forwards that address to `/dashboard?feedback=1`, which opens
the dashboard feedback dialog. The Office task wall still marks bids with its
"Bid" badge. Tests: `world/tests/test_world_removed_boards.py`.

## Office floors doubled: 340 × 180 (2026-09-29)

The user asked for every Office floor to be twice as large in each horizontal
direction, with everything else derived from the footprint.

### Dimensions

| | Before | After |
| --- | --- | --- |
| Floor footprint (`OFFICE_WIDTH` × `OFFICE_DEPTH`) | 170 × 90 | 340 × 180 |
| Walkable interior (`officeInteriorContains`, avatar radius 0.46) | x ±83.89, z ±43.89 | x ±168.89, z ±88.89 |
| Floor area | 15,300 | 61,200 |
| Storey height (`OFFICE_FLOOR_HEIGHT`) / tower height | 16 / 64 | 16 / 64 (unchanged) |
| Roof deck / rooftop floor | y 48 | y 48 |
| Tower centre (`OFFICE_ISLAND_CENTER`, landmark position) | (0, 0, -215) | (0, 0, -260) |
| Front facade and entrance, World z | -170 | -170 (`OFFICE_ENTRANCE_Z`) |
| Back facade, World z | -260 | -350 |
| Elevator centre x (`OFFICE_ELEVATOR_CENTER_X`) | 18 | 36 |
| Campus island radius (`OFFICE_ISLAND_RADIUS`) | 110 | 207 |

The storey height stays 16. People, desks and tables keep their size, so a
taller storey would only add empty air above them, push the roof, zip-line
anchor and elevator head 16 units higher per floor, and push the drone course
up with them. The screenshots with a 16-unit storey look right: the ceiling
sits about four avatar heights above the floor, as before.

### How it is derived

`world-office-tower.js` keeps the old 170 × 90 room as a reference plan
(`OFFICE_PLAN_WIDTH`, `OFFICE_PLAN_DEPTH`). Every furniture position used to be a
literal in that plan; it now goes through `officePlanX(x)` / `officePlanZ(z)`,
which scale a plan coordinate by `OFFICE_WIDTH / OFFICE_PLAN_WIDTH` and
`OFFICE_DEPTH / OFFICE_PLAN_DEPTH`. Objects keep their own size: only their
positions move away from the centre. Setting `OFFICE_WIDTH` and `OFFICE_DEPTH`
back to 170 and 90 reproduces the old layout.

Three rules decide which helper a position uses:

- **Free-standing furniture** scales from the centre with `officePlanX/Z`:
  the lobby logo fountain (now at (-36, -4)), the reception desk (z -73), the
  lounge table and seats on every interior floor ((0, 34), seats at x ±92),
  the five Engineering workstations (x -72, -36, 0, 36, 72), the three rooftop
  patio tables (x -80, 0, 80 at z 16), System Capacity on Engineering
  (`SYSTEM_CAPACITY_ENGINEERING_POSITION`, now (-48, 0, 32)), the lobby portal
  on the upper floors, the ceiling light grid, the two ambient point lights
  (their range scales too) and the three lobby lights, the elevator and the
  floor-warp destination.
- **Wall-mounted things stay on their wall** and scale only along it: the rear
  wall placards, the Marketing task and initiatives boards, the Engineering
  debug panel, the lobby attendance and clock-in boards (now
  `±(OFFICE_WIDTH / 2 - 0.5)`), the Marketing attendance calendar, the floor-warp
  doorways (`OFFICE_FLOOR_WARP_DOOR_START_X = -OFFICE_WIDTH / 2 + 3.8`) and the
  aquarium, which keeps its 2.1-unit gap to the west wall
  (`OFFICE_AQUARIUM_POSITION`, now (-167.9, 0, -21); `OFFICE_AQUARIUM_BOUNDS`
  is built from it). The reef control panel is a child of the tank, so it
  moved with it. The Marketing roster desks keep their 7.8 units from the
  front glass; their column spacing scales.
- **Things tied to another object** follow it: Noah and his nameplate follow
  the reception desk (`OFFICE_RECEPTION_DESK`), and the QA board stays at the
  desk's left front corner.

Collision reads the same constants the meshes do. `FLOOR_OBSTACLES` is now
built from `OFFICE_LOBBY_FOUNTAIN`, `OFFICE_RECEPTION_DESK`, `OFFICE_LOUNGE`,
`OFFICE_ENGINEERING_STATION_XS` (with the station half-sizes) and
`OFFICE_ROOFTOP_TABLES`, and `office-rooftop.js`, `office-floors.js`,
`office-lobby.js` and `office-displays.js` place their meshes from the same
exports, so a collider and its mesh cannot drift apart again. The shell,
slabs, ceilings, roof glass, walkable bounds, elevator shell, doors, exits,
camera limits and far proxy already read `OFFICE_WIDTH`/`OFFICE_DEPTH`. The
five facade columns scale their positions and stay five, so the draw count
does not grow. The rear wall and roof glass barriers now build their boxes
through `resources.primitive`.

### Outside the building

The entrance stays at World z -170, so the town path to the door, the front
garden, the gym, the swings, the instance booth and the benches did not move.
The tower grows north and sideways instead, and its centre moves from z -215
to -260. The back corners (±170, -350) are 16 units inside the edge of the hex
ring road. Scanning every mesh in the running scene against the new footprint
found only the zip line and the approach path (both derived) and the city
land, desert and road meshes the tower always stood on.

- **Landscaping** (`social-packages.js`) goes through `officeCampusX/Z`: a
  plan point inside the old walls scales, one outside them keeps its distance
  from the wall. The side trees stay 12 from the side walls, the back trees
  28–33 behind the back wall, the front bushes 7 and the flowers 12 in front
  of the entrance, now spread across the wider front.
- **Zip line.** The roof mast is at `ZIP_LINE_ROOF_LOCAL` =
  `(OFFICE_WIDTH / 2 - 7, officePlanZ(-8))` = (163, -16), World (163, 58.9,
  -276). The harness station follows it, and the cable and the beach landing
  mast re-aim from the new start as before (landing at (481.4, 5, -101.2)).
- **Drone course.** The loop is a circle of radius 290 round the town tile.
  Before, its centreline came no nearer than 58.8 units to the tower shell (at
  a back corner). No placement of a 340 × 180 tower inside the town keeps that
  margin horizontally: the tower would have to sit within 231 of the town
  centre at every corner, which puts its front in the Town Square. The course
  now flies over the tower instead: `DRONE_COURSE_ALTITUDE` =
  `OFFICE_TOWER_HEIGHT` + `DRONE_COURSE_TOWER_CLEARANCE` (59) +
  `DRONE_COURSE_CLIMB` + `DRONE_COURSE_WEAVE` = 151, so the rings sit
  between 123 and 176. The nearest approach to the shell is now 73.2, and to
  the zip-line cable 93.1 (it was 55.5). Radius, phase, rings and LOD are
  unchanged.
- **Campus surface.** `OFFICE_ISLAND_RADIUS` is the tower's half diagonal plus
  the same 14 units of margin (207). It now reaches z -53 and covers the whole
  old bridge span; the city lawn under it was walkable anyway.
- **Distances measured from the tower centre** keep their distance from the
  front glass, because the centre moved 45 units back:
  `OFFICE_EXTERIOR_LOD_DISTANCE` = `OFFICE_DEPTH / 2 + 190` (235 → 280),
  `ENCLOSURE_EXTERIOR_KEEP_RADIUS` = `OFFICE_DEPTH / 2 + 155` (200 → 245) and
  the compact-renderer Office residency `enter`/`exit` =
  `OFFICE_DEPTH / 2 + 105 / 130` (150/175 → 195/220).
- `world-data.js` has no imports, so its landmark position is the literal
  `[0, 0, -260]`; the travel list and map buttons read it from there.
  `test_world_frontend.py` and `test_world_remote_offices.py` pin it next to
  `OFFICE_ISLAND_CENTER`.

### Sparse floors

Nothing new was added. With four times the floor area the floors now read as
sparse, the lobby most of all after main removed the four east-wall kiosks in
the same period. Engineering has one row of five workstations in a 340-wide
room, the rooftop has three patio tables and the zip-line pad, and Marketing
has the meeting table plus the roster desks along the front glass. If the
floors should feel busier, the natural next step is a second workstation row
or more patio tables, placed through the same constants.

### Verification

- `python3 -m pytest -q -p no:cacheprovider world/tests`: 1371 passed, 1
  skipped after merging main. The tests that pinned Office coordinates read
  the new constants or the doubled values (`test_world_office_tower.py`,
  `test_world_office_frontend.py`, `test_world_frontend.py`,
  `test_world_landscape.py`, `test_world_marine_aquarium.py`,
  `test_world_remote_offices.py`, the direct-geometry allow-list) and
  `world/browser_tests/tests/world.spec.js` aims its Office cameras and
  expectations at the moved fixtures.
- Reachability: a 0.5-unit flood fill with the scene's own
  `officeScenePointIsWalkable` from the elevator reaches every walkable cell
  on every floor (about 237,000–239,000 per floor, 4.2× the old count), from
  x -168.5 to 168.5 and z -88.5 to 92.5 (the cabin).
- In headless Chromium on the RX 580 (dev server, fixture APIs), a visitor
  walked through the lobby doors, rode the elevator from the lobby to each
  floor and back, walked each floor wall to wall along two rows and two
  columns (x reached ±168.6, z ±88.3, never stuck), and walked out of the
  lobby into town.
- Screenshots before and after (exterior from the square and from above, each
  floor from both corners, the elevator, the door walk, the rooftop and zip-line
  start, the drone course beside and above the tower) were checked by eye.

### Performance gate

`tools/world_bench.py --gate` on the RX 580. The poses were not moved: the
three Office poses still stand at World z -207, which is now 37 units inside a
deeper room.

- The three Office goldens (office-lobby, office-rooftop, office-engineering)
  were re-recorded with `--bless` after checking the frames; the old ones are
  under `golden/<slug>/archive/2026-09-30T01-5*`. The rooftop golden already
  failed on main (20 %) because the Infrastructure floor had lowered the roof.
- Four more goldens change because the tower is bigger, and were left for the
  merge: spawn (14.6 %) and arrival-stats (9.7 %) look north and now have the
  wider tower across the top of the frame, town-overview (3.9 %; 1.9 % on main
  already) shows the whole tower, and beach (1.5 %) sees the zip-line landing
  mast, which moved with the cable. Bless them with
  `python3 tools/world_bench.py --bless --pose spawn --pose arrival-stats
  --pose town-overview --pose beach` when this merges.
- Budget: no Office pose exceeds a ceiling. The failures left (spawn,
  arrival-stats and town-overview triangles, members-circle draws) also fail
  on main at the branch point, so `world/perf-budget.json` is unchanged.

## Private instance booth removed (2026-09-29)

The walk-in "Instance launcher booth" (the pink and yellow tent with the
rotating globe, the "LAUNCH INSTANCE" placard and the "CLICK TO CONFIGURE"
screen at `INSTANCE_GARDEN_POSITION` (-52, 0, -140), on the town side of the
Office front garden) is gone. The load log's "Instance launcher booth" element
(13 objects, 11 meshes) no longer registers.

Removed:

- its meshes, materials, globe texture, screen painter and globe animation in
  `scene/districts/office-boards.js`, its `registerWorldElement` and
  compact-district registration, and `INSTANCE_GARDEN_POSITION` in
  `scene/gameplay-constants.js`;
- the click target (`instance-launch-booth` in `scene/runtime/pointer.js`),
  the walk-in trigger in `scene/runtime/animate.js`, the `instanceBooth`
  context getter and the `onInstanceBoothSelect` callback in `scene/core.js`
  and `world.js`;
- the booth assertions in the World tests. The rest of
  `world/tests/test_world_instance_booth.py` (launcher dialog, secret handoff,
  federated portals, front garden, node plaza, Start your world) now lives in
  `world/tests/test_world_instance_launcher.py`, whose first test asserts the
  booth stays gone.

API calls that stopped: walking into or clicking the booth opened the launcher,
which fetched `GET /api/world/instances` and `GET /api/world/installs` and
polled both every 30 s while it stayed open. Those booth-triggered calls no
longer happen. No endpoint became unused: the same two calls still run when
the launcher is opened from a Start your world here sign, and
`/api/world/instances` is also part of the World's full data refresh.

Kept, and why:

- The Cloudflare launcher dialog (`data-world-instance-launcher`,
  `toggleInstanceLauncher`, `continueInstanceLaunch`,
  `validateInstanceLauncher`, `downloadInstanceSetupCard`, the directory
  polling and its `.world-instance-launcher` CSS). The Start your world here
  signs open it through their "Cloudflare key" choice, and the Start your
  world panel reuses the `.world-instance-launcher` styles. Its eyebrow now
  reads "START YOUR WORLD HERE" instead of "PRIVATE INSTANCE BOOTH". The
  launcher stays a flat, non-modal region so walking and the rest of the World
  remain available behind it. (Removed on 2026-10-07 with the Start your
  world panel: the plot signs now open the `/start` page,
  [INSTANCES.md](../INSTANCES.md). The instance directory used to be polled
  only while that launcher was open; it now loads once in the deferred,
  post-interactive work and then every 5 minutes
  (`startInstanceDirectoryPolling(5 * 60 * 1000)`), so federated portals and
  the "new independent World" celebration show without the launcher.)
- The Vultr mirror button and the SSH and Vultr self-setup flow, which never
  went through the booth.

The front garden path the booth stood on is unchanged. Load-log time before
the booth's registration (the tail of the Office interior build) is now charged
to the next registered element. The spawn and town-overview goldens may show
the empty spot from far away; re-bless them with the next bench run if the gate
flags them.

Checked in headless Chromium (dev server, fixture APIs):
`/var/tmp/world-instance-booth/booth-before.png` and `booth-after.png` show
the booth spot before and after; the page logged no console errors, and the
Start your world "Cloudflare key" choice still opens the launcher.

## Office tower becomes a hexagon (2026-09-29)

The user asked for the Office to be about 75% smaller in width and length,
keep its tall rooms, and become a regular hexagon like the town.

### Dimensions

| | 340 × 180 box | Hexagon |
| --- | --- | --- |
| Shape | rectangle | regular hexagon, flat side at the front |
| Across (x, corner to corner, `OFFICE_WIDTH`) | 340 | 85.45 (`2 × OFFICE_HEX_RADIUS`) |
| Front to back (z, flat to flat, `OFFICE_DEPTH`) | 180 | 74 (`2 × OFFICE_HEX_APOTHEM`) |
| Side length (`OFFICE_HEX_RADIUS`) | – | 42.72 |
| Floor area | 61,200 | 4,743 |
| Walkable interior (`officeInteriorContains`) | x ±168.89, z ±88.89 | hexagon with apothem 35.89 |
| Storey height / floors / tower height | 16 / 4 / 64 | 16 / 4 / 64 (unchanged) |
| Tower centre (`OFFICE_ISLAND_CENTER`, landmark) | (0, 0, -260) | (0, 0, -207) |
| Front door, World z | -170 | -170 (`OFFICE_ENTRANCE_Z`) |
| Back face, World z | -350 | -244 |
| Elevator (`OFFICE_ELEVATOR_CENTER_X/Z`) | (36, +90), front facade | (12, -37), back face |
| Campus island radius (`OFFICE_ISLAND_RADIUS`) | 207 | 57 |

**Why 85 across.** Read literally, "75% smaller in width and length" makes
each horizontal dimension 25% of 340 × 180: 85 × 45. A regular hexagon cannot
be 85 wide and 45 deep, so the width sets it: 85.45 corner to corner is 25.1%
of 340, and the depth follows from the shape (74 flat to flat, 41% of 180).
The apothem is 37 so the tower centre lands on the whole number z -207. At this
size every floor still works, so there was no reason to go larger: the lobby
holds the reception desk (30 wide), the logo fountain, a lounge, the 27-unit
aquarium with its reef panel, two 27-wide boards and four warp doors; Marketing
holds the meeting table, three boards and up to 18 roster desks; Engineering
holds five workstations, System Capacity and the debug panel; the rooftop holds
three patio tables and the zip-line station. The flood-fill test below reaches
every walkable cell from the elevator on every floor.

**Orientation.** The town hexagon has corners pointing north and south (its
flat sides face east and west, `WORLD_HEX_TILES`). Copying that orientation
would point a corner at the Town Square and put the door on a face turned 30°
away from the approach path, which would have to move. The tower is therefore
the town hexagon turned by 30°: corners point east and west, and a flat side
faces the door, the path and the square. `OFFICE_HEX_SIDES` lists the faces
(`front-right`, `front`, `front-left`, `back-left`, `back`, `back-right`) with
their outward normals, tangents and the `rotationY` that turns a wall-mounted
plane to face the room; `OFFICE_HEX_CORNERS` lists the six corners.

### What follows the hexagon

- **Geometry.** Floor slabs and ceilings are one extruded hexagon each, with a
  notch for the elevator shaft on the back face, built through the new
  `resources.prism(THREE, outline, height)` so they are cached by the resource
  authority. The glass walls, the emissive floor bands (now on all six faces),
  the rooftop glass barriers and the six corner columns are box segments laid
  along each face. The front face keeps the concrete sill strips, the header
  band, the ground-floor glazing and mullions around the door, and the
  per-floor glazing above. The far proxy is rebuilt from these meshes as
  before (43 draws, budget 64).
- **Collision.** `officeHexExtent(x, z)` (the largest distance along the six
  face normals) replaces every box test: `officeInteriorContains`,
  `officeTowerShellContains` and `officeTowerShellExit` (the flight shell now
  names the six faces), the town-side wall clamp in `constrainTownOfficeWalls`
  (pushed out along the face the walker came through), the roof-jump exit, the
  quadcopter parking keep-out and the third-person camera clamp, which adds
  the four slanted faces to its box.
- **Elevator.** The glass car and shaft sit on the back face at x 12, half
  outside the tower as before, and are turned 180° so the doors open into the
  room; the outward pane in `OFFICE_ELEVATOR_SHELL` moved to the -z side. The
  elevator head, air conditioner and ladder ride on the shaft.
- **Aquarium.** The tank stands on the back-left wall (`OFFICE_AQUARIUM_WALL`)
  2.1 units from the glass, turned by `OFFICE_AQUARIUM_ROTATION_Y`.
  `OFFICE_AQUARIUM_BOUNDS` is now in the tank's own frame and
  `officeAquariumLocalPoint(x, z)` converts a floor point into it for collision
  and the visitor-proximity reaction. The reef panel is still a child of the
  tank.

### Layout

Walls are addressed with `officeHexWallPoint(side, along, inset)`.

- **Lobby.** Door in the front face; reception desk across the middle at z -13
  with Noah behind it and the QA board at its left front corner; logo fountain
  front-left (-19, 11) and lounge front-right (19, 11); attendance board on the
  front-left wall, clock-in board on the front-right wall, aquarium on the
  back-left wall, greeting board on the back-right wall; the four warp doors on
  the left of the back wall and the elevator on its right.
- **Marketing.** Meeting table in the centre; task board on the back-left wall,
  initiatives board on the back wall, attendance calendar on the back-right
  wall; roster desks fill rows along the front glass, each row as wide as the
  hexagon allows there (5, 6, then 7 desks).
- **Engineering.** Five workstations in a row across the widest part
  (x -28 to 28, z -4); System Capacity front-left at (-15, 17); debug panel on
  the back-left wall.
- **Every interior floor.** Lounge front-right; the floor placard on the
  front-left wall; the lobby portal beside the elevator; a ceiling light grid of
  14 fixtures kept 6 units inside the walls.
- **Rooftop.** Three patio tables at z 16; zip-line mast near the east corner
  at `ZIP_LINE_ROOF_LOCAL` (33.7, -4); elevator head at the back.

### Outside the building

The door stays at World z -170, so the approach path, the front garden, the
gym, the swings and the benches did not move; the tower centre moved from
-260 to -207.

- **Landscaping** (`social-packages.js`) is placed from the faces: nine trees
  12–13 units off the four slanted faces and 28–33 behind the back face, six
  bushes 7 in front of the front face beside the path, eight more 5 off the
  slanted faces, and eight flowers 12 in front of the door.
- **Zip line.** The roof mast is at World (33.7, 58.9, -211) and the cable and
  beach landing mast re-aim from it as before (landing at (477.5, 5, -91.4)).
- **Drone course.** Back to its original heights, 32–85
  (`DRONE_COURSE_ALTITUDE` = 60; `DRONE_COURSE_TOWER_CLEARANCE` is gone).
  Sampling the loop centreline against the hexagonal prism, the nearest
  approach to the tower is 89.9 (the original 170 × 90 tower had 58.8) and to
  the zip-line cable 61.4 (originally 47).
- **Landmark.** `world-data.js` has the literal position `[0, 0, -207]`. The
  World has no minimap; the map rail buttons and the travel list read this
  landmark position.
- **Distances measured from the tower centre** are still `OFFICE_DEPTH / 2 +`
  a constant, so they keep their distance from the front glass
  (`OFFICE_EXTERIOR_LOD_DISTANCE` 227, `ENCLOSURE_EXTERIOR_KEEP_RADIUS` 192,
  compact-renderer residency 142/167).
- The plan helpers (`OFFICE_PLAN_WIDTH/DEPTH`, `officePlanX/Z`,
  `officeCampusX/Z`) are gone; positions are written in tower-local units.

### Lobby-only exhibits hide off the lobby

The aquarium and the four floor-warp doors hang off the interior root, not a
floor group, so hiding the other floors never hid them: a visitor on
Engineering still drew the whole reef under the opaque slab. The wide tower
kept them out of frame; the hexagon puts them in view of every upper-floor
camera. `syncOfficeFloorVisibility` now hides both while the visitor is inside
on another floor, and shows them again in the lobby and from town (where the
far proxy is built). On the Engineering bench pose this took draws from 220
to 99 and on the rooftop pose from 127 to 104.

### Verification

- `python3 -m pytest -q -p no:cacheprovider world/tests`: 0 failures. Tests
  that pinned Office coordinates read the hex constants
  (`test_world_office_tower.py`, `test_world_office_frontend.py`,
  `test_world_frontend.py`, `test_world_landscape.py`,
  `test_world_remote_offices.py`, `test_world_office_meeting_frontend.py`,
  `test_world_reef_control_panel.py`). The new
  `test_every_floor_is_reachable_from_the_elevator_inside_the_hexagon` floods
  each floor on a 0.5 grid with `officeScenePointIsWalkable` from the elevator
  cabin: every walkable cell is reached (about 15,500–17,300 per floor),
  nothing walkable lies outside the hexagon or the cabin, and the door and all
  six corners are reached. `world/browser_tests/tests/world.spec.js` aims its
  Office cameras and expectations at the new fixtures (not run here: the
  browser suite needs the Worker).
- In headless Chromium on the RX 580 (dev server, fixture APIs) a visitor
  walked in through the doors, rode the elevator from the lobby to each floor
  and back, walked outward from the centre to all six walls and all six corners
  on every floor (within 0.3 of the wall limit everywhere, except the lobby's
  back-left wall where the aquarium stops the walker 3 units short, as it
  should), and walked out of the lobby into town. No page errors.
- Screenshots (exterior from the square, close to the door, from above, from
  the back and wide; each floor from the front, back, side and back wall; the
  elevator; the doors walk-in; the rooftop from above and the zip-line start;
  the elevator head; the drone course beside and above the tower) were checked
  by eye.

### Performance gate

`tools/world_bench.py --gate` on the RX 580.

- The three Office goldens (office-lobby, office-rooftop, office-engineering)
  were re-blessed after checking the frames.
- The lobby pose camera stands 7 units inside the door looking at the back of
  the room, which in the hexagon includes the aquarium: its coral garden, rock,
  sand and reef panel are about 220 of the pose's 296 draws. The wide tower
  kept the tank 168 units to the side. The lobby's draw ceiling rose from 94 to
  296, triangles from 32,180 to 46,296 and geometries from 453 to 480; render
  time stays inside its ceiling (p90 13–15 ms against 22). Batching the
  aquarium's static coral would bring the draws back down and is the natural
  follow-up.
- Other goldens that change with the tower and are left for the merge: spawn
  (8.6 %) and arrival-stats (6.0 %) look north at the smaller tower,
  town-overview (3.2 %) shows the whole tower, and beach (0.9 %) sees the
  zip-line landing mast, which moved with the cable. Bless them with
  `python3 tools/world_bench.py --bless --pose spawn --pose arrival-stats
  --pose town-overview --pose beach` when this merges.

## Round 7 load

From the 2026-09-30 cold-cache log (`scene 1.25 s`, longest slice 66 ms,
Office pieces with 0 visible objects costing 55–59 ms each, the visitor's own
`/api/accounts/<name>` fetched twice).

- **Office aquarium in steps.** `createOfficeMarineAquarium` is now a wrapper
  that drains the generator `officeMarineAquariumSteps`; the scene build calls
  `createOfficeMarineAquariumSliced(THREE, animated, sceneSlice)`, so the tank,
  sand, coral, sea grass, school, food, particles and light each end at a
  yield and the 12 ms slice rule can pause between them. Measured step costs on
  the RX 580: 0.5 / 0.6 / 0.3 / 2.0 / 3.1 / 10.4 (sea grass) / 4.3 / 0.2 /
  0.4 / 0.8 / 0.9 ms. The reef control panel gets its own slice.
- **Logo fountain in slices.** Three extra `sceneSlice()` pauses (after the
  basin and light, after the F/M faces, after the extruded chrome plates).
- **Far proxy tail sliced.** `mergeObjectBatchesSteps` also yields once per
  finished batch mesh, not only per visited object, so building the merged
  geometry no longer runs as one block at the end.
- **Logo fountain animation culled.** Its ambient callback returns at once
  unless every ancestor of the fountain is visible; from Town (far tier) the
  cube and water no longer rotate unseen, which also stops their matrices from
  being re-marked every visual tick.
- **Your avatar split.** The system capacity platform ends its own slice
  before `createAvatar`; the avatar itself is still a ~40 ms block
  (`scene/avatars.js`) and is the longest remaining slice.
- **Asset textures decode off the main thread.** `projectAssetTexture` loads
  through `THREE.ImageBitmapLoader` (`imageOrientation: "flipY"`,
  `texture.flipY = false`), so the concrete path and beach horizon arrive as
  decoded `ImageBitmap`s and the first draw that uses them no longer pays a
  synchronous decode (the 13 ms "town-path-north-office" upload). Browsers
  without `createImageBitmap` keep `TextureLoader`. Golden frames unchanged.
- **One self profile request.** `fetchAccountProfile(account)` is the single
  path for the visitor's own profile: the chest tab and the face-image
  fallback both call it with the same `?viewer=` key (the key the boot prefetch
  already warmed) and a 60 s `maxAge`, so the second 47 KB fetch and its JSON
  parse are gone. A follow or unfollow reloads with `fresh: true`.
- **Not changed.** Deferring the aquarium and logo fountain until approach was
  left out: `ctx.officeAquarium`, `ctx.aquariumPanel` and the logo reflection
  are read unguarded from the animate loop, pointer, presence and exit code,
  and the Office far proxy merges the furnished interior, so a late interior
  would need a far-proxy rebuild. The first-view warm-up passes are mostly real
  program creation for the first view; each resume pass also re-renders the
  shadow map (`warmFirstView` resets `shadowMap.needsUpdate`), which is the
  next thing to cut.

Measured with `tools/world_bench.py --pose spawn --probe` (boot log), RX 580,
warm local server, one run each: scene work 441 ms → 385 ms, longest scene
slice 49 ms → 44 ms (the avatar), first frame 1550 → 1404 ms, interactive
2527 → 2380 ms; spawn golden 0 %, draws 207 unchanged.

## Round 7 diagnostics

The 2026-09-30 copied log contradicted itself; these changes make the next one
match the frame and name its culprits.

- **Build on every log.** The optimization report read only the App worker's
  `/api/version` answer and printed "build unknown" when it had not arrived.
  It now prefers the World shell's own stamp (`forkmesh-world-version` and the
  revision from `forkmesh-world-deploy`, both injected by
  `tools/next_deploy.py`), then `/api/version`, then the raw fingerprint, and
  says "not stamped (local or dev server)" when there is none. The load
  summary's first line carries the same label (`readWorldDeployBuild`,
  `worldBuildLabel` in `world-boot.js`), and each stored load record keeps it.
- **Cold cache called out.** When the previous load had another build or the
  network bytes more than doubled, the "Slower than the previous load" line
  says so and its suggestion drops to `info` ("Cold cache, not a code
  regression by itself"). A deploy always makes the next load cold, so the
  1.06 s → 2.62 s line of that log was mostly this.
- **Findings and the budget table use one window.** `worldProfiler.sample()`
  reset its window whenever anything asked, and `renderDiagnostics()` runs on
  demand (after `/api/version`, for instance), so a sample could cover a few
  frames. One 3.6 ms build-stamp repaint over a ~27 ms window became
  "132.8 ms/s, Ambient 3.70 ms per frame". A reading now needs 1 s and 30
  frames; a shorter request returns the last full reading. The copied report
  re-audits with the current reading instead of reusing the audit from
  minutes earlier, which is why its CPU figure (14.8 ms) disagreed with the
  table under it (7.3 ms). The Frame budget heading states the window.
  Callback offenders list calls, average and peak next to ms/s.
- **Ambient callbacks named by element.** Core wraps the `animated` list with
  `worldProfiler.watchCallbacks`, and every scene element mark
  (`bootTrace.onSceneElement`) hands the callbacks pushed since the previous
  mark to that element (`claimCallbacks`). The minified
  `B<b.nextRefreshAt||…` line reads "ForkMesh Office" now; `#n` tells several
  callbacks of one element apart. Callbacks added after the scene build keep
  the source-snippet label.
- **Matrix finding.** The settled-matrix walk already freezes every object
  that stops moving, so the old "set matrixAutoUpdate = false; 0 objects
  never moved" advice was always 0. `settled-matrices.js` now counts walks,
  visits, recomposes, freezes and thaws (`settledMatrixWalk`, also on
  `scene.userData`). The audit reports objects visited and recomposed per
  frame, thaws per second and the elements that own objects outside settled
  subtrees; the static-object advice appears only when that count is non-zero.
  Render stalls add "N objects walked, M recomposed" to the matrix-walk part,
  so a 44 ms walk shows whether a new subtree was walked in full.
- **Long frames.** `Response.prototype.json` is traced (URL and resolve time,
  64 kept), so "Response.json.then" culprits and the Main-thread blockers rows
  name the API path. A long frame during the scene build lists the scene
  elements whose work ended inside it ("scene build: Office logo fountain
  59 ms, …"). An unattributed frame whose time is mostly before rendering names
  the largest module, style or API body that arrived just before it
  ("parsing or compiling world-scene.js 209 KB"), and says that LoAF lists
  only scripts over 5 ms, which is what "no script attribution" usually means.
- **Compact.** The six "First view shader warm-up · pass N" lines are one
  line with the per-pass split, and scene lines no longer print
  "0 objects · 0 meshes" for marks that pass no roots.
- **Label semantics.** A scene mark's time is the work since the previous
  mark, so "First view shader warm-up · pass 2" measures pass 1's warm-up.

## Round 7 runtime

Frame cost from the round 7 log (RX 580, Members Circle view, 104 draws,
`renderer.render` 6.2 ms average). Measured with `tools/world_bench.py` plus
probes that toggle subtrees and list each main-pass draw by owner. Hiding
whole districts moved the render time by at most 1.3 ms. Draw count was what
the render time tracked (about 20 µs a draw here), not object count, because
batched figures and settled groups outside the view are already skipped
during the render.

- **Office build stamp.** The `B<b.nextRefreshAt…` ambient callback is the
  Office build stamp. Every 30 s it made a new 1640×160 canvas texture, swapped
  `material.map` and set `material.needsUpdate`, which cost 3.6 ms in one
  frame. It now repaints the same canvas (`repaintCanvasTexture`) and only
  while every ancestor is visible. While the stamp is hidden (inside another
  district, beach, or the far LOD) it checks again every 2 s
  (`OFFICE_BUILD_STAMP_HIDDEN_RETRY_MS`). A stamp that arrives while hidden
  is painted on the next visible tick. The "132.8 ms/s" figure itself was a
  sampling-window artifact (see Round 7 diagnostics).
- **Campfire in four draws.** The five flame tongues were five meshes with
  five `ShaderMaterial`s sharing one program. They are now one instanced
  draw: the shared plane plus per-instance `aFlame` (speed, phase, curl,
  opacity), `aShape` (the layer's width-scaled rotation, height, index) and
  `aOffset`. The vertex shader applies the sway in the layer's own axes before
  scaling and turning it, so the shape matches the separate meshes. The seven
  smoke puffs, which the CPU moved every tick, are one quad mesh. Its vertex
  shader runs the same age, scale, roll, drift and fade maths from
  `uTime`/`uGrowth`. The three logs and nine coals are two `InstancedMesh`es
  with instance colours. The coals were nine unique `MeshStandardMaterial`s
  whose `emissiveIntensity` the callback set. They now carry a per-instance
  `aCoal` emissive colour and phase, which the fragment shader turns into the
  same flicker. `uCoalClock` is `time*0.009` wrapped to 2π on the CPU, so
  float precision holds over long sessions. The members-circle pose went from
  87 to 66 draws with an unchanged golden frame. Additive flames do not
  depend on draw order. The 0.1-opacity smoke no longer sorts puff against
  puff, and the difference is not visible.
  A first version baked the merged vertices in JavaScript loops. Those loops
  run cold in the interpreter during the scene build and added about 10 ms to
  the campfire slice. Instancing leaves only a few dozen values to fill, and
  the builder is now cheaper than the old one (about 6 ms cold in node,
  against 7 ms before).
- **Shadow casters.** `setShadows` gives `castShadow` only to meshes with at
  least one visible, non-transparent material. A transparent label, ring,
  pin or glass pane cast a solid rectangle into the shadow map, and a hidden
  hit proxy cast nothing but was still sorted. The figure batches for the
  badge and back-name plates stop casting too. At spawn this is 2725 to 1777
  casters, with the golden frame unchanged (0.03%, as on base). Casters only
  cost on the 10 s shadow refresh frames, which were the 50 ms+ stalls in the
  log's "shadow map 59 draws" line.
- **Back-facing plates.** The settled culler now also collects one-sided
  plates in settled subtrees: `PlaneGeometry` with every vertex at one local
  z and normal +Z, a single `FrontSide` material, and no shader or
  `onBeforeCompile` hook. On main-pass frames it hides the plates whose front
  faces away from the camera (`hideBackFacingPlates`) and restores them after
  the render, like the group cull. The GPU would drop those triangles anyway,
  but three.js still paid a full draw call for each. The test uses the
  inverse-transpose sign (`det < 0` flips, as three.js's `frontFaceCW` does),
  so mirrored plates stay correct. Shadow-refresh frames keep every plate.
  The mirror cabinets' four panels, sign backs and plaques are the bulk.
  Spawn went from 207 to 177 draws, arrival stats from 199 to 170 and the
  town overview from 327 to 283, all with unchanged golden frames.
- **Resumed warm-up passes skip the shadow map.** `warmFirstView` re-flags
  `shadowMap.needsUpdate` when it finishes, so every resumed first-view pass
  (passes 2 to 7 in the log, 16 to 31 ms each) rendered the shadow map
  again. Resumed passes now pass `shadows: false`. The first pass still
  warms the shadow programs, and the first real frame refreshes the map.
- **Duplicate materials.** The six "identical" Members Circle ring materials
  are the phase-slotted activity rings (`avatarRingMaterial`, one per
  colour and pulse slot); round 6 showed that sharing them splits the crowd
  mirror. The audit signature now includes `userData.ringPhase`, so they are
  no longer reported.
- **Not done.** The 944 figure materials belong to the per-figure source
  meshes that the figure batches hide during the render. They cost memory
  but no draws. The Office far proxy still draws about 41 materials from
  230 units away: 20 of them are textured sign plates that would need an
  atlas, and the build stamp is one of them. The mirror cabinet panels are
  one canvas per cabinet and side. Batching them needs an atlas that
  follows the cabinet rebuilds in `updateNetworkNodes`. The back-face cull
  already halves them.

## Back name board worn on the shirt (2026-10-07)

The back name board (name, PR/issue/discussion counts, activity and task
links) hung off the avatar root at `y = 2.94, z = 0.68`: 0.37 behind the
torso's back face and half above the shoulders, so from behind it read as a
sign floating over the person. It was placed there to clear the jetpack, which
is only shown in flight.

It is now a child of the torso, like the chest card, at
`(0, 0.47, 0.33)` scaled 0.628, so it is 1.08 x 0.51 across the top of the
1.1 x 1.5 back and turns with the look-direction lean. The owner-only work
board (`forkmesh-self-work-back-badge`) moved with it, scaled 0.63 to
0.67 x 0.83 under the name board and above the operator belt, so the two do
not overlap. In flight the jetpack covers part of both, as it would a printed
shirt. Figure batches copy `matrixWorld`, so the batched name board follows
the new parent unchanged.

## Related docs

- [docs/app/README.md](../app/README.md) (the App),
  [sessions](../app/auth-sessions.md),
  [footprint](../app/worker-footprint.md)
- [docs/deploy/README.md](../deploy/README.md),
  [docs/deploy/auto-deploy.md](../deploy/auto-deploy.md),
  [docs/PLAN.md](../PLAN.md), [docs/OPERATIONS.md](../OPERATIONS.md),
  [docs/CUTOVER.md](../CUTOVER.md)
- [world/README.md](../../world/README.md),
  `world/public/world/elements/README.md`
- `www/docs/world-community-services.md`
- `www/docs/superpowers/` (historical Office and chat designs)
