# World (`world/`)

ForkMesh World is the three.js town at `world.forkmesh.com` where accounts,
repositories, mirrors and agents appear as places and avatars, with global
chat, the Office (encrypted meeting rooms) and community boards. The browser
code is static. It is served by the core nodes: Caddy serves the built files
from `world/dist` in the current app release, and the App on the same node
answers `/api/world/*`, the World Durable Objects and `/health/mobile`. The
desktop embeds the same page.

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
| `world/public/world/world-floor-notes.js` | Floor notes: placement chip, floor cards, thread panel, @mention suggestions |
| `world/public/world/world-profiler.js` | Admin profiler: frame phases, per-callback cost, draw-call attribution, GPU timer queries, scene audit |
| `world/public/world/world-resource-cache.js` | Shared geometry/material caches, instanced items, instanced mirrors |
| `world/public/world/world-debug-log.js` | Admin debug log: counters, ring buffer, error/console/long-animation-frame/HTTP observers |
| `world/public/world/world-mesh.js` | World mesh: directory normalising and the watch/visit sockets to neighbouring worlds ([World mesh](#world-mesh)) |
| `world/public/world/world-boot.js` | Load log: boot timeline, scene-element and shader-compile trace, load suggestions |
| `world/public/world/world-territory.js` | An instance's own hexagon: settings from `/api/instance`, the ForkMesh elements it hides, crossing URLs and arrival parameters, the bug list ([INSTANCES.md](../INSTANCES.md#an-instance-world-is-its-own-hexagon)) |
| `world/public/world/scene/instance-territory.js`, `scene/territory-geometry.js` | The instance hexagon's scene (plaza, sign, buildings, bug terrarium, the ForkMesh hexagons next door) and the road-crossing geometry |
| `world/public/world/world-brand.js` | On a white-label instance, renames "ForkMesh" in canvas-painted scene text to the instance name ([INSTANCES.md](../INSTANCES.md#the-world-on-an-instance)) |
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
- **Serving.** Caddy routes each request by the generated
  `world-routes.caddy`: `/`, `/world` and `/world/` get the shell
  (`/world/index.html`, `no-store`) from `world/dist`; `/health/mobile` goes
  to the App host; a path in `run_worker_first` goes to the App, except the
  App-owned scripts and assets World uses, which are served from the App's
  `dist`; any other existing file is served from `world/dist`; everything
  else goes to the App. Python never serves World's static bytes.
- **Backend.** `/api/world/*` is served by the App from `world/src`.
  Presence and chat run in the `ForkMeshWorld` object over `/api/world/ws`;
  Office meetings in `ForkMeshOfficeRoom`. The Durable Objects run clustered
  across the core nodes. World chat is global: one `#general` for everyone.
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
  `/api/world/ws`: the [World mesh](#world-mesh).

## Configuration

`WORLD_ORIGIN` is `https://world.forkmesh.com`: in `deploy/fleet.toml`
`[production.vars]` (rendered as `FORKMESH_VAR_WORLD_ORIGIN` by
`deploy/core/render.py`), and with the same value in `app/wrangler.toml` and
`www/wrangler.toml`. The served host is `world_host` in `deploy/fleet.toml`
`[production]`. `WORLD_MESH_HUB` (default `https://app.forkmesh.com`) is
where an instance reads the World mesh directory; an App without
`INSTANCE_NAME` is the hub itself. `WORLD_NEIGHBOUR_SCENE` (optional, on an
instance) is the scene its single hexagon shows in the hub's World (see
Neighbour scenes below). `world/wrangler.toml` still carries legacy Worker settings;
the served stack reads only its `run_worker_first` list and `[build]`
command. It holds no secrets.

## Build, run, test

```sh
python3 world/tools/world_dev_server.py          # http://127.0.0.1:8788/world/
python3 -m pytest -q world/tests
cd world/browser_tests && npm ci && npm test     # @critical suite
npm run test:device-tiers                         # adaptive-rendering matrix
python3 tools/world_bench.py --gate              # performance gate (see below)
```

`world/tests/test_world_dev_server.py` drives `WorldDevHandler` over a
`socketpair`, whose buffer on macOS is 8 KB. The handler writes all of
`index.html` (about 22 KB) before it returns. If the test called the handler
on its own thread before reading, `sendall` blocked with nobody reading and
`pytest world/tests` hung at that test (15 Oct 2026). So the test runs the
handler on a thread while the client reads, and both sockets have a 10 s
timeout. The server itself is not affected, because `ThreadingHTTPServer`
gives each connection its own thread.

The `@critical` suite has a 55 s global budget (`playwright.config.js`,
pinned by `app/tests/test_offline_ci_workflow.py`). The GitHub workflow runs it
with `npm test -- --global-timeout=0`, bounded by the job's `timeout-minutes`,
as `test:device-tiers` already is: `ubuntu-latest` has 4 vCPUs and no GPU, so
Chromium draws with SwiftShader at 120–250 ms a frame and the six tests take
two to three minutes. The 55 s cap failed every run of
`.github/workflows/world-browser-e2e.yml` before a test could report
(22 Oct 2026).

On a software renderer the browser specs gate on frames, not milliseconds:

- The input-response test keeps its 100 ms budget on hardware; under
  SwiftShader it allows twelve measured frames (100 ms is six at 60 Hz,
  doubled for SwiftShader's frame-to-frame jitter) and drops its first camera
  drag, which carries about 700 ms of one-time work (816 ms against ~130 ms for
  the next four on the mobile viewport).
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
([docs/deploy/README.md](../deploy/README.md)): the release carries
`world/dist` and `world-routes.caddy`, and Caddy is reloaded when the release
switches. World API changes ship in the same release. `tools/next_deploy.py`
writes `world_deploy_status` for the "ForkMesh is deploying now" notice
([deploy/auto-deploy.md](../deploy/auto-deploy.md#world-deploy-notice)).

## Security

- Elements get only `THREE`, validated params and an allowlisted
  same-origin `net()`; they have no other I/O.
- Chat content is encrypted by the chat client; the Office uses the same
  channel authorization as `/chat` (`www/docs/security/sealed-chat.md`).
- The local speech bridge: `www/docs/security/world-local-speech-bridge.md`.
- Optional elements that read data (for example the status board, which
  reads `/api/status/world`) declare the endpoint in their manifest and
  disclose it before purchase.

## Rendering architecture

### Scene modules

`world-scene.js` is a facade that re-exports `scene/`. `scene/core.js`
holds `createWorldScene`: renderer, camera, lights, shared state, the
district build order and the public API.

The instance hex module keeps portal arrival exports at module scope and
builds the shared flower roads in `flowerRoadGeometry()`. The scene bundler
passes its diagnostics through to the deployment log so syntax errors identify
the source file and line. The bundle and instance portal tests cover this
boundary after the shared-map and portal changes are combined.

A module exports up to two functions:

- `functions({ THREE, <callbacks>, ctx, fns })` declares the area's inner
  functions and returns the ones other code calls. `core.js` calls every
  `functions()` first and merges the results into `fns`, so any function can
  call any other. Calls into another module go through `fns.name(...)`.
- `build({ ...values, ctx, fns })` holds a district's construction and runs
  at its fixed point in the build order, so meshes, random draws and
  registrations happen in a stable order. An `async build` that awaits
  `sceneSlice()` yields at the same points each load.

`ctx` is the shared state seen from a module: a getter per `core.js` binding
and a setter for each one it assigns. It is a class instance, not an object
literal, so accessors stay in V8's fast mode.

Rules for new code:

- A new district or Office area gets `districts/<name>.js` and a call in
  `core.js` at the right point in the build order.
- A build that calls a module function reading the district's own objects
  must publish them on `ctx` as soon as they exist.
- Imported bindings cannot be assigned; shared module state changes through
  setters (`setCanvasTextureScale()`, `setDeferredCanvasPaints()`).
- Every file under `scene/` must be listed in `world-loader.js`'s
  modulepreload array (`test_world_scene_modules.py`).
- A top-level World module may only import scene modules that are bundle
  entries; `world/tests/test_world_bundle_entries.py` bundles the real tree
  and fails on any unresolved scene import.
- Source-text tests read the scene through `app/tests/world_scene_source.py`,
  which concatenates the facade and modules and strips `ctx.`/`fns.`
  prefixes.

### Resource authority

`scene/resources.js` is the one place scene code builds geometries and
materials:

- `resources.primitive(THREE, "BoxGeometry", 1, 2, 3)`: a built-in
  parameterised geometry, cached by type and arguments.
- `resources.geometry(THREE, key, factory)`: any other geometry, cached under
  an explicit key. `resources.prism(THREE, outline, height)` for extrusions.
- `resources.material(THREE, params, type)`: a shared material, cached by
  type and parameters.
- `resources.uniqueMaterial(THREE, params, type)`: a material the caller owns
  and may change (fades, tints, swapped maps).

Cached resources get a no-op `dispose` (`markSharedResource`), so tearing
down one mesh cannot free a buffer the rest of the scene draws. Share a
geometry only when its arguments are constants; share a material only when
nothing changes it after it is built. `resources.sharedMaterialDrift(THREE)`
lists shared materials whose values no longer match their key (the browser
harness expects it empty). With no type, materials default to
`MeshLambertMaterial` for rough non-metal surfaces and `MeshStandardMaterial`
for specular ones.

`world/tests/test_world_resource_authority.py` counts every
`new THREE.*Geometry(` and `new THREE.*Material(` outside `resources.js` and
fails when a file exceeds its allow-list entry; the allow-list only shrinks.

### Instancing and culling

- **Instanced mirrors** (`createInstancedMirrorSet`) draw repeated static
  meshes as `InstancedMesh` batches; instanced items map `instanceId` hits
  back to the logical object. Each batch keeps an owner source
  (`userData.mirrorOwnerSource`) for profiler attribution.
- **Figure batches.** Members Circle figures and live peers are drawn as
  shared instanced batches with per-member texture layers, near/far detail
  chosen from pixels per world unit, and compacted instance lists.
- Instanced batches keep frustum culling on and recompute their bounding
  sphere when instances move.
- **Settled matrix walk.** Objects that have not moved for 30 frames get
  `matrixAutoUpdate = false` and their subtree is skipped until a real
  change.
- **Settled group culler.** `createSettledGroupCuller` hides out-of-view
  settled subtrees only during the main render call and restores them in
  `finally`. Subtrees with lights are never grouped; shadow frames render
  everything.
- **Distance detail without pop-in.** Nothing is hidden or swapped for a
  different-looking stand-in by distance. The Office far tier
  (`rebuildOfficeFarProxy()`) merges the visible furnished tower into
  batches keyed by material state, with plain canvas signs packed into a
  per-state atlas (`OFFICE_FAR_ATLAS_SCALE` 0.5, refreshed when a sign's
  texture version changes). `OFFICE_DISTANT_DRAW_BUDGET` bounds it. Objects
  with `userData.farProxySkip` stay out of it.
- **Phone districts.** Compact renderers release GPU buffers of districts in
  `COMPACT_DISTRICT_RESIDENCY`; a district stays resident while the visitor is
  within its radius or its bounds are in the camera frustum (with enter/exit
  margins). `updateCompactDistrictView()` runs before every render, so a
  district the camera turns to is drawn in its first on-screen frame.
  When a territory visit moves the Office and the beach next door,
  `moveCompactDistrictsWithin()` moves their residency centres with them and
  re-measures their bounds. Until 2026-10-22 they were judged where they used
  to stand, so on phone tiers (and any desktop the ladder put there) the moved
  Office and beach blanked out while in view and stayed resident out of view.
- **Office aquarium on demand.** The lobby reef is built in idle slices when
  the camera comes within the Office detail distance, warmed off-scene, then
  attached; it is released after `OFFICE_AQUARIUM_RELEASE_MS` (15 s) beyond
  the build distance plus `OFFICE_AQUARIUM_RELEASE_MARGIN`.
  `whenOfficeAquariumReady()` starts or joins the build. Its point lights are
  budgeted through `updateLocalPointLightBudget()`, which pads with dark
  slots so the light count never changes.

### Loading pipeline

- **One loader.** The loading card is static markup in `index.html`, outside
  `<forkmesh-world>`, with inline styles and fixed ledger rows matching
  `WORLD_BOOT_STEPS`. It stays hidden until the document is parsed
  (`data-parsed="true"`). `<forkmesh-world>` ships `data-world-booting inert`
  at opacity 0; at 100 % `revealWorld()` fades the card, removes both
  attributes, records `WORLD_BOOT.interactive()` and fires
  `forkmesh:world-interactive`. The WebGL fallback, an import failure and a
  10 s watchdog also remove the card.
- **Sliced construction.** `createWorldScene` yields between slices; frames
  hold up to 32 ms of construction work. Construction-time canvases queue
  their painter; first-view plates paint before the reveal and the rest in
  idle slices.
- **First view compiled before reveal.** `prepareFirstView()` warms the
  programs the spawn view needs; `firstViewHoldUntil` holds the main render
  until it returns (or 4 s).
- **Program pacing** (`installProgramPacer`). After the first view,
  `compileShader`/`linkProgram` are queued and one program links per frame.
  A queued program reports not ready through `COMPLETION_STATUS_KHR`, so
  three.js defers the draw; any synchronous read forces the link. Without the
  extension the pacer does nothing.
- **Upload budget** (`createUploadBudget`, `createStripedUploads`). A token
  bucket of 6 MB per 16.7 ms is shared by idle warm-up uploads, the
  construction-canvas painter and main-view repaints. Canvas and image
  textures of 4 MB or more upload in 2 MB row stripes. Kiosk repaints swap
  the new texture in only when fully uploaded (`stripedUploads.swapMap`).
- **Lazy extras.** Optional features (floor notes, packages) load after the
  scene is built; a failed module fetch leaves the World running.
  `initialWorldModuleBytesSoft` in `world/tools/build_worker_footprint.py`
  fails the build when the first-load module graph grows.

### Diagnostics

- **Load log** (`world-boot.js`, `window.__forkmeshWorldBoot`). Records from
  time zero: navigation timing, buffered paint/resource/long-animation-frame
  entries, `performance.mark`/`measure` pairs per ledger step, the GPU
  string, the first rendered and first interactive frame, per-element scene
  construction cost and shader compile stats. 1.5 s after interactive it
  disconnects every observer and ranks suggestions
  (`computeWorldLoadSuggestions()`), adding `world.auditScene()` findings in
  an idle callback when the panel is visible. Capped at 400 lines; nothing
  runs per frame after load.
- **Crash guard and scene stats.** The crash-guard beat runs for every
  visitor and re-reads the diagnostics at most every 4 s, including the scene
  stats, whose walk runs in idle slices. The heaviest three World
  elements (once a minute) are counted inside the same sliced walk; they used
  to come from one synchronous `listWorldElements()` call that blocked an
  idle callback for up to half a second on slower machines.
- **Visibility.** Shown to admin tickets and to anyone with Settings → Debug
  → "Show debug panel"; otherwise recorded but hidden. The panel is
  draggable, can collapse to a pill, and "Copy log" copies the summary,
  "Optimization suggestions", the event lines and the optimization audit.
- **Performance bar and profiler** (`createWorldProfiler`, only while an
  admin bar is shown): frame phases, per-callback cost, draw-call attribution
  by World element, GPU timer queries; `auditWorldScene` finds duplicate
  geometries/materials, instancing candidates and culling opt-outs.
- **Render stall monitor** (`scene/render-stalls.js`): splits frames over
  50 ms into program compiles, texture uploads, shadow and reflection draws,
  and scene update parts (matrix walk, animation, labels, camera). Long
  frames without scripts are split into pre-render tasks, callbacks and
  style/layout/paint.

## Traps

- Editing `world/worker.js` changes nothing live; change `caddy_routes.py` or
  `world_health.py` too. `world/deploy.sh` runs a legacy wrangler deploy and
  does not deploy World to the core nodes.
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
  canvas.

## User interface

### UI chrome

Everything that is not the 3D scene is a light, low-chrome UI in
`world/public/world/world.css`: light tokens in the `:root` blocks, green only
for buttons and blue for inputs, focus, links and selection, and a
`.fm-world` component layer at the end of the file that new chrome rules
belong in. Panels have fixed sizes (Settings is one centered modal; phones get
a full-screen sheet). In-world panels are canvases painted through a proxied
2D context (`worldPanelContext`) that maps colours to the light theme; art
that must keep its colours uses `canvasArtTexture()`. Icons are inline SVG
(`WORLD_UI_ICON_PATHS`, `worldIconSvg()`).

### Notices are inline

The World has no toasts.

- **Feedback for an action** goes through `notice(message, { kind, at })`. A
  capture-phase listener remembers the control the visitor last used;
  `notice` writes a `<p class="world-inline-status" role="status">` into the
  closest host (`WORLD_NOTICE_HOSTS`: form, fieldset, section, article,
  aside, details, dialog). Host chains are stored as selectors so a result
  arriving after a panel re-render still finds its panel; the line stays
  until replaced or the view changes.
- **Actions with no panel** use `hudNotice`, one line under the HUD island's
  action bar; status lines clear after 12 s, errors stay until dismissed.
- **Unsolicited notices** (pings, announcements, celebrations, a new World
  build) go to the chat transcript through `notifyChatArea`.
- Persistent state with its own controls (the update notice, the runner
  attention pill) stays as is.

### HUD island

One `<section data-world-hud-island>` holds the avatar action toolbar, the
chat feed (`#fullChatMessages`, two lines collapsed, expandable) and the
composer with a Chat or Agent switch (always Chat after a reload). It stays
above the software keyboard (`--world-chat-covered-bottom`).

- **Channel switcher:** `#general` and the World rooms
  (`ForkMeshDashboardChat.spaces()`) switch in place (`switchSpace`); private
  channels and DMs (`/api/chat/channels`, `/api/chat/direct-messages`) open
  `/chat`. The last room is kept in `forkmesh.world.chatChannel.v1`.
- **Agent prompts:** use the owner agent queue
  (`/api/repo/<owner>/<repo>/agent-sessions`): `GET` lists `targets` and
  `models` (cached ten minutes); `POST` sends `{prompt, provider, model,
  targetNode}` with the Desktop or Runner target always named. Models come
  from `ORG_AGENT_MODEL_ALIASES`; choices persist in
  `forkmesh.world.agentComposer.v1`.

### Restore everything and start fresh

Settings > Debug > "Restore everything and start fresh"
(`startWorldFresh()`) cancels pending position and preference writes, posts
empty settings to `/api/world/preferences`, removes every local and session
storage key starting with `forkmesh.world.`, `forkmesh.worldComposer.` or
`forkmesh-world-` (except saved views and the loader's reload guard), and
reloads. The login, theme, consent and API origin stay.

### Crash reports

The heartbeat records `document.visibilityState` on every visibility change.
A session whose last beat was `hidden` is reported as "World reloaded after
the browser closed it in the background"; one that stopped while visible is
"World crashed and reloaded". Background closes and discards are sent as
kind `discard` (stored in `error_log` without a notification) and do not
count toward the safe-mode reboot; visible crashes are kind `crash`.

## The town

### Layout

- **City.** The city lawn is a rounded hexagon, apothem
  `CONTINUOUS_CITY_RADIUS` (365), corner radius
  `CONTINUOUS_CITY_CORNER_RADIUS` (70), flat edges east and west.
  `roundedHexagonGeometry()` draws it and `roundedHexagonContains()` bounds
  walking, so the drawn and walkable edges match.
- **Desert ring.** Six desert hexagons (`DESERT_HEXES`) of the same shape tile
  around it at `WORLD_HEX_PITCH` = 2 × 365 + `WORLD_ROAD_WIDTH` (10).
  `DESERT_PLOTS` holds twelve plots, each with a "start your own instance"
  sign that opens `<site>/start?plot=<n>` ([INSTANCES.md](../INSTANCES.md)).
- **Roads.** `createHexRoadNetwork()` draws a 10-unit road around every hex
  in three draws (asphalt, edge lines, instanced centre dashes).
  `createRoadGraph()` builds the matching graph (24 nodes, 30 roads) for
  bikes.
- **Neighbouring worlds.** Every World is a seven-hex "flower" (its city and
  its six desert hexes) on one shared map, and other worlds sit on the
  flowers around it. You can see them and walk straight in. See
  [World mesh](#world-mesh).
- **Instance hexagons.** `INSTANCE_HEX_SLOTS` (`scene/layout.js`) is a second
  ring of 12 tiles. An instance with its own world gets a meadow hexagon with
  roads, its logo as a ground decal, and a landmark board that opens the
  instance's address. `refreshInstanceDirectory()` reads
  `/api/start/instances` every 5 minutes and keeps entries with
  `world: true`, an https address, a 32-hex id and an unused slot. Nothing is
  built while no instance has a hexagon. An entry with a `worldUrl` is a
  portal: the board travels to that instance's own World at
  `?portal=<slot>`, and an instance's World draws one "ForkMesh World"
  hexagon that leads back; `?portal=<n>` places the arriving visitor in front
  of hexagon `n`
  ([../app/instance-federation.md](../app/instance-federation.md#world-connectivity)).
  On an instance with its own hexagon (a territory), the World is drawn as
  that one hexagon with the ForkMesh hexagons next door, and walking across
  its road travels to the ForkMesh World (`checkTerritoryCrossing`); while a
  territory is active `placeWorldMesh` draws no world-mesh neighbours, so the
  two layouts never overlap. Off a territory the world-mesh flowers below
  replace the old road crossing into an instance hexagon
  ([../INSTANCES.md](../INSTANCES.md#an-instance-world-is-its-own-hexagon)).
- **Bounds.** `desertWalkSurfaceContains()` is the union of all hexes grown
  by the road width plus junction discs, the hexes of every placed
  neighbouring world included (`setWorldMeshFlowers()`). `WORLD_RADIUS` (1900) bounds remote
  avatars, spawns, flight and the server's saved-view check; the relay's
  `WORLD_COORD_LIMIT` is 16000.
- **Ground logo.** `groundLogoTexture()`/`applyGroundLogo()` (`scene/town.js`)
  patch the city grass shader to tint the ForkMesh mark into the lawn, faded
  by camera height; no extra mesh or draw.

### Office tower

The Office is a four-storey regular hexagon (`world-office-tower.js`) with a
flat side facing the town path.

| | Value |
| --- | --- |
| Across, corner to corner (`OFFICE_WIDTH` = 2 × `OFFICE_HEX_RADIUS`) | 85.45 |
| Front to back, flat to flat (`OFFICE_DEPTH` = 2 × `OFFICE_HEX_APOTHEM`) | 74 |
| Storey height (`OFFICE_FLOOR_HEIGHT`) / floors / tower height | 16 / 4 / 64 |
| Tower centre (`OFFICE_ISLAND_CENTER`) | (0, 0, -207) |
| Front door, World z (`OFFICE_ENTRANCE_Z`) | -170 |
| Elevator (`OFFICE_ELEVATOR_CENTER_X/Z`) | (12, -37), back face |

- Furniture positions are written in a reference plan and mapped with
  `officePlanX/Z`; wall-mounted objects stay on their wall
  (`OFFICE_HEX_SIDES` gives each face's normal, tangent and `rotationY`).
- `officeHexExtent(x, z)` drives every collision test
  (`officeInteriorContains`, `officeTowerShellContains`, wall clamps, camera
  clamp). Colliders and meshes read the same exported constants
  (`FLOOR_OBSTACLES`).
- Floors: lobby (reception, logo fountain, lounge, aquarium with reef
  controller, boards, warp doors), Marketing, Engineering (workstations,
  System Capacity, debug panel) and the rooftop (patio tables, zip-line
  station).
- A build stamp under the `FORKMESH OFFICE` sign shows the running World
  version, commit and deploy age from the `forkmesh-world-version`,
  `forkmesh-world-built` and `forkmesh-world-deploy` metas (falling back to
  `/api/version`).

### Board ring

Leaderboard Island's boards stand on one ring
(`scene/districts/billboard-circles.js`, sized by `BILLBOARD_RING` in
`scene/layout.js`). It replaced three separate circles (hub, social,
leaderboards), which read as three plazas and fetched everything on a circle
as soon as you walked onto it.

- **Layout.** Boards are grouped by section (`sections`: hub, social,
  leaderboards; a placement's `circle` option names the section, so existing
  packages keep working) and then by `order`. Each board gets the arc it
  needs, the spare arc is shared evenly, and an `entryGap` is left open
  towards the causeway. `minRadius` (38) already fits every board, X and
  Reddit banners included, so the ring is sized once and does not regrow as
  boards are placed at boot.
- **Refresh pads.** Every board with a `feed` has a small semicircle in front
  of it (`padRadius` 2.2). A board fetches only when you step onto its own
  pad: `updateBillboardPads()` runs with the other proximity checks at
  12.5 Hz, rejects anyone outside the pad band with one distance test, and
  reports `onBillboardNearby({ feed, refetch })` / `onBillboardAway({ feed })`
  to the shell. Leaving needs `padExit` more distance than entering, so
  standing on the edge cannot flap the request. Each board has its own
  cooldown (`cooldownMs`, 60 s; the QA board 5 s), so walking along the
  leaderboard cards refreshes each card once rather than the whole family.
  The shell's `startBillboardWatch(feed)` maps a feed to its loader; the QA
  deck polls only while you stand on its pad, and the Mastodon and social
  banner countdowns refetch only while you stand on their pads.
- **Spinners.** Only the board whose pad started a request spins. Feeds that
  share one request (all leaderboard cards read `/api/leaderboards`) still
  repaint together from that one snapshot, but only the approached card
  shows the spinner. A request started elsewhere (a QA action, copying a
  referral link) spins every board of that feed.
- **Draw cost.** The ring is one textured apron (a unit circle scaled to the
  ring), one rim, and one `InstancedMesh` holding every pad, so all pads are a
  single draw call and the active pad is lit by its instance colour (`muted`
  idle, `accent` underfoot). The rim and pads are flat decals with
  `polygonOffset`, so they never z-fight the apron. Geometry comes from the
  resource authority (the pad is a cached half-disc, laid flat by its
  instance rotation).

### Members Circle

Members sit in rings around a campfire by lifecycle band; members with a
custom avatar sit in the innermost band (`MEMBER_CIRCLE_AVATAR_BAND`). Each
band has a ground label with its name and count. A live peer's photo face is
used unless the peer explicitly chose the generated face; failed photo loads
retry three times.

### Mirror yard

Mirror-node cabinets are built from `loadFountainData()`.
`mirrorNodeServices(node)` turns the signed mirror record into up to six
services (`sync`, `git`, `browse`, `merge`, `actions`, `agents`), drawn as one
instanced batch of gears on the cabinet roofs: brass and spinning when
healthy, red and stopped otherwise. Rotation runs only near the yard, when
the element is shown and without reduced motion.

### Floor notes

`world-floor-notes.js`: a tap on empty town ground offers "Add a note here";
notes are `THREE.Sprite` cards (the 24 nearest within 48 units). Each note is
a thread; `@name` pings existing accounts (at most 10 per post, 25 per reply
including earlier participants) with a link to `?note=<id>`. Changes send a
signed control request (`/api/world/floor-notes-changed`, HMAC, one minute) to
`ForkMeshWorld`, which broadcasts `{"type": "floor-notes", "id": ...}`.

- API: `GET /api/world/floor-notes` (80 most recently active), `GET ?id=<id>`,
  `POST` with `action` `create`, `reply` or `delete`. Text ≤ 500 characters,
  positions bounded (|x|, |z| ≤ 1500, −20 ≤ y ≤ 200), 20 notes per account,
  one post per 5 s, 100 replies per thread.
- Storage: `world_floor_notes`, `world_floor_note_replies`; author and text
  encrypted in `data`, keyed by `account_bi`, removed with the account.

## World mesh

All the Worlds (forkmesh.com and every self-hosted instance with its own
World) share one map, so you can see the worlds around you and walk into
them without changing page.

- **Map.** Each World is a flower of seven hexes: its city plus the six
  desert hexes. Flowers tile the plane without gaps. The hub (forkmesh.com) is
  at the origin. Directory slot `n` is `worldMeshSlotOffset(n)`
  (`scene/layout.js`): slots 0–5 are the six flowers touching the hub, 6–11
  the next six. Every client draws itself at the centre and each other world
  at `offset(world) - offset(self)`. An instance therefore sees the hub and
  its fellow instances where they really sit.
- **Single hexagons.** A slot 0–4 listed in the hub's `WORLD_MESH_HEX_SLOTS`
  (comma-separated, `0` in the production fleet) is published with
  `"shape": "hex"` and drawn as
  one of the town's petals: it takes the place of desert petal
  `NEIGHBOUR_HEX_PETALS[n]` (`scene/layout.js`; petal p lies at 60°·p, as in
  `DESERT_HEXES`) and shares an edge with the town. Slot 0 is petal 1, slot 4
  petal 5; petal 0 keeps the beach. That petal's sand, cacti and sign step
  aside (`replacePetals` in `desert-plots.js`), the roads stay as they are,
  and the town no longer claims the ground (`worldMeshOwnerAt`). From a
  distance the petal shows the neighbour's published scene (Neighbour scenes
  below). Stepping off the road onto its land puts the page in that World,
  laid out the way the instance's own World draws itself (Standing in an
  instance's hexagon below). On the shared road strip the walker stays in
  the World they came from. A spot on a neighbour's hexagon is not saved as
  the World position (`neighbourHexLandAt`, and nothing is saved while the
  page stands in a hexagon), and a position saved there before is replaced
  by the fresh arrival at the campfire (`leaveRestoredNeighbourHex`), so
  opening the World lands in it, at the last spot walked in it. The page used to
  load `<world>/world/?from=forkmesh&slot=n&pos=x,z` instead, which reloaded
  the whole World on every crossing; `worldMeshCrossingURL()` still builds
  that URL for a neighbour's own page. Arriving from such a page,
  `?from=<host>&slot=n&pos=x,z` from a listed hexagon places the walker just
  outside the edge they crossed, facing away from the petal with the
  third-person camera behind them, and the parameters are removed.
- **Neighbour scenes.** A single-hexagon neighbour may publish what stands
  on its hexagon at `GET <world>/api/world/neighbour-scene` (public, CORS,
  cacheable). The hub lists that URL as `scene` on hex entries in
  `/api/world/mesh`, and its World fetches it without credentials and reads
  it with `normalizeNeighbourScene()` (`world-neighbour-scene.js`): version
  1, at most 64 KB and 48 objects, types `plaza`, `sign`, `house`,
  `terrarium` (up to 32 bugs), `lamp` and `tree`, positions within 330 of
  the hexagon centre in World axes, `facing` in radians, `#rrggbb` colours,
  plain text, and https links only. Bug icons must sit on the neighbour's own
  origin, so a scene cannot make every hub visitor's browser call third-party
  hosts; any other icon is dropped and the bug shows its initial. The body is
  refused when it is over 64 KB in bytes, by `content-length` or once read.
  `buildNeighbourScene()` draws it with shared primitives; a scene that
  arrives after its hex layer was replaced is ignored, and when it is missing
  or refused the logo hexagon stays. A `circuit` ground gets a tiled canvas of
  traces in the accent colour, which glow faintly (`neighbourGroundMaterial`);
  `plain` is a flat colour. The terrarium glass does not write depth, or it
  hid every bug drawn after it. A house, its label and each bug with an https
  `url` are `instance-hexagon` picks, so clicking one opens that link in a new
  tab.
- **Bug icons through home.** An instance's raw-file route sends no CORS
  header, so a cross-origin icon cannot be drawn into a WebGL texture and
  every bug fell back to its initial. The World loads each icon from its own
  server instead: `GET /api/world/neighbour-icon?world=<id>&path=<path>`
  (`neighbourIconURL()`, `world_neighbour_icon_handler`). The server answers
  only for a hexagon neighbour in its directory that lists a scene (never
  itself), builds the URL as that neighbour's `world` origin plus the path
  (`neighbour_icon_source()`: starts with one `/`, at most 300 URL-safe
  characters, no `#`), fetches it without credentials and with redirects
  refused (4 seconds, 64 KB), and relays it only when the bytes are PNG,
  JPEG, GIF, WebP or ICO (`icon_type()`; never SVG). The reply carries the
  sniffed type, `access-control-allow-origin: *`, `nosniff`, a sandbox CSP
  and an hour of public cache; the server keeps up to 96 icons in memory for
  an hour, and a failure for a minute.
- **Publishing a scene.** Every instance World serves that route
  (`world_neighbour_scene_handler`, `neighbour_scene()` in `world_mesh.py`)
  with `access-control-allow-origin: *` and a 5-minute public cache; the
  hub answers 404, since it is never anyone's neighbour. Without settings
  it publishes a default scene: a plaza with lamps, a sign with
  `INSTANCE_NAME` and the World's host, and a ring of trees at 280 (outside
  the landmark at 0.55 of the apothem, inside the 330 reach). An operator
  can replace it by putting their own scene JSON in `WORLD_NEIGHBOUR_SCENE`.
  The server only checks the outline (64 KB, version 1, 1–48 object
  entries) and falls back to the default if the outline is wrong; the hub's
  World still checks every field.
- **Directory.** `GET /api/world/mesh` (`world_mesh.py`) answers
  `{self, worlds:[{id, name, host, world, api, logo, slot}]}`. The hub builds
  it from `instance_directory` (`world=1`, visible, slot 0–11) plus its own
  entry (`id: "forkmesh"`, slot −1). An instance fetches the hub's copy from
  `WORLD_MESH_HUB` server-side and caches it (5 minutes, 30 seconds after a
  failure). `self` is the answering world, matched by host.
- **Drawing.** `createInstanceHex()` draws a neighbour as a meadow centre with
  its logo, landmark and label, six sand hexes and one road net, all as
  shared geometry. The neighbour's roads sit 0.02 below the home roads so the
  shared border strip does not z-fight. Clicking the landmark explains how to
  walk in; it opens no tab. When the World the page stands in is itself a
  single hexagon, every neighbour is drawn as a single hexagon too
  (`updateInstanceHexes`): from an instance on hub slot 0 (petal 1) the hub
  is one hexagon on the instance's petal 4, mirroring how the hub draws the
  instance. Drawing the hub as a whole flower there laid its sand petals over
  the instance's own town.
- **Ownership.** `worldMeshOwnerAt()` decides whose ground the player stands
  on. The home hexes win the shared road strip; past it, the neighbour's
  flower owns the point. The page decides with a margin of 8
  (`WORLD_MESH_SWITCH_MARGIN`), so a walker on the strip's midline does not
  switch Worlds back and forth; a single hexagon needs the walker on its land
  (`onLand`), which already leaves the strip between.
- **Watch sockets.** `WorldMeshLinks` (`world-mesh.js`) opens
  `wss://<api>/api/world/ws?watch=1` to up to four neighbours whose centre is
  within 1700 units (released at 1900). The relay accepts a watcher only from
  a mesh origin, tags it `watch` and sends a `welcome` snapshot. It then
  forwards only `join`, `presence`, `move` and `leave` frames, and pings keep
  the socket alive. Watchers are not participants: they get no avatar, are
  not counted as online, and have their own cap (`WORLD_MAX_WATCHERS`, 128).
  Their peers are shifted by the neighbour's offset and drawn in the space
  `world-mesh`, which carries the district radius. They are named
  `name@host`, except visitors from this world, who keep their plain name.
- **Visits.** Stepping onto a neighbour's flower or single hexagon swaps that world's watch
  socket for a presence socket. The client sends its profile as
  `town-square`, and its moves after subtracting the offset, so the
  neighbour's own visitors see it walk in from their edge. Meanwhile the home
  socket stays open (chat, Office and the rest are unchanged), but its
  presence switches to the space `visiting` and its moves stop. Home viewers
  hide `visiting` peers and see the visitor through their own watch socket
  instead, so nobody is drawn twice. Walking back reverses the swap. A notice
  says which world you entered or returned to.
- **Hand-off.** The home presence turns `visiting` only once the neighbour's
  relay has welcomed the visit socket (`onVisitAdmitted`, `admittedId` in
  `WorldMeshLinks`). Until then, and again whenever that socket drops, moves
  keep going to the home socket converted back to home's frame
  (`homeFrameMovement()`), while the visit link keeps the latest move to send
  on open. The watch link's residents stay drawn until the visit's own
  welcome replaces them. Before 2026-10-22 the switch happened as soon as the
  walker crossed, so for the seconds a visa and socket took (or for good, if
  the neighbour refused the socket) the walker was drawn in neither World.
- **Standing in an instance's hexagon.** Until 2026-10-21 stepping onto
  OWASP BLT only drew its published scene, a stand-in with a plaza, a sign,
  a house and a terrarium, and the rest of BLT's World (its members,
  repositories, padlocks and shield, benches, trees, its residents as
  residents) was not there. That evening the page briefly switched into BLT
  as if it were a flower, and a walker stood in ForkMesh's town filled with
  BLT's members. Now the page enters an instance's World as that World draws
  itself (`docs/INSTANCES.md`, An instance World is its own hexagon):
  - As soon as the hexagon is placed, the page reads the instance's
    `/api/instance` (`neighbourInstance()`: CORS, no credentials, no
    redirects, 6 seconds; the federation client's path list stays as it is)
    and its neighbour scene, and `territoryForNeighbour()` (`world-territory.js`)
    turns them into the same territory settings the instance builds from:
    name, style, accent, houses and the terrarium. The logo is the
    directory's logo, else `/instance-logo.png` through
    `/api/world/neighbour-icon`; the bugs are the scene's terrarium list with
    their icons through the same relay, since the instance's raw-file route
    sends no CORS header. Until this has loaded (or when it fails; it is
    retried after a minute) stepping onto the land stays a visit to the
    published scene.
  - `enterMeshWorld()` then calls the scene's `enterTerritoryVisit()`
    (`scene/instance-territory.js`): it builds the hexagon with
    `buildInstanceTerritory()` at the town's centre (or shows the copy built
    ahead, below), moves ForkMesh's
    scenery (`TERRITORY_NEIGHBOUR_ELEMENTS`) next door by
    `forkmeshShiftAround()`, switches off `TERRITORY_HIDDEN_ELEMENTS` (only
    those the visitor had on), walks the hexagon and ForkMesh's tiles, and
    recentres the walker by minus the hexagon's offset. The Members Circle,
    repository portals and other town reads come from the instance
    (`reloadMeshRootData()`), its residents arrive over the visit socket,
    the houses and terrarium are walled and clickable as on the instance,
    and home's peers stay visible next door. ForkMesh's Office stays home's
    (`setOfficeOwner(null)`).
  - The hub's single-hexagon layer is cleared while the page stands there
    (`placeMeshHexRoot`), and home is the one neighbour, at the shift the
    territory reports. Stepping onto ForkMesh's ground (its tiles grown to
    the road's midline, `territoryOwnerAt()`) returns home:
    `leaveTerritoryVisit()` moves the scenery back, gives back its clicks,
    turns the hidden elements on again and keeps the built hexagon, hidden,
    for the next visit.
  - The hexagon is built before anyone steps in. Once the territory has
    loaded, an idle callback calls `prepareTerritoryVisit()`, which builds it
    and adds it to the World hidden (`userData.groundStandby`), so the GPU
    warm-up compiles its materials and uploads its plates in idle time and the
    crossing frame only shows it. Building, compiling and uploading it on the
    crossing frame cost a half-second stall. A standby hexagon is skipped by
    the ground-height walk, has no clicks, and its terrarium bugs only move
    while it is shown in the scene.
- **Standing in another World.** A flower neighbour runs the same town, so
  its real town is this page's town with that World's data. Single-hexagon
  neighbours are entered as their own hexagon (see above). Before,
  stepping onto a neighbour kept the page in the home World, and the
  neighbour stayed a placeholder: a meadow with a logo, or its published
  scene, with nobody but passers-by from the watch socket. Now
  `enterMeshWorld()` (`world.js`) moves the page into the flower whose ground
  the walker steps onto:
  - The scene recentres on it. `shiftPlayer(dx, dz)` (`scene/runtime/camera.js`)
    moves the walker and camera by minus that World's offset, keeping their
    heading, and `placeWorldMesh()` lays the mesh out from that World's
    slot, so the full town stands where its hexagon was and the World left
    behind becomes the neighbour hexagon. The switch is refused inside a
    building, a package district or on a ride; the walker then visits as
    before.
  - The town's content is read from that World (`fetchMeshRootJSON`): the
    reads in `MESH_ROOT_READS` (`world-federation.js`: member directory,
    inactive members, arrival stats, organizations, events, fediverse boards,
    network overview, repositories, leaderboards) go to its API through the
    federation client without credentials, and `reloadMeshRootData()` reloads
    them on every switch. Reads in `MESH_ROOT_HIDDEN` (repository imports and
    `/api/repo/...` details, ads, the reward fund, the security board, media
    spaces) answer `not_in_this_world` there, so another World's town never
    shows home's data. Everything else (session, chat, notifications,
    rewards, any write) stays with home. A read that started before a switch
    is dropped (`world_changed`).
  - The visit socket to that World is the page's presence there, and its
    residents are drawn with their activity and status like home peers.
    Home's own socket stays open with presence `visiting`; its peers outdoors
    are drawn shifted onto home's hexagon (`homeFramePlayer`), so people on
    both sides of the border stay visible.
  - The Office in that town is its owner's (`setOfficeOwner`), entered as a
    visitor ([federation.md](federation.md)).
  - The saved position is kept in home's frame, so a reload puts the walker
    back on the same ground and the page switches again.
  A notice names the World entered (`Entering <name> · <host>`) or returned
  to.
- **Visas (the session trick).** A signed-in user's account lives on their
  home world only. Before a visit the client asks home for a visa:
  `POST /api/world/visa {destination}`. It needs a session and an allowed
  origin, and the destination must be a neighbour in the directory. The visa
  is an HMAC token over `{name, home, destination, nonce, issuedAt,
  expiresAt}`, valid for 60 seconds. It travels as `?visa=` on the socket.
  The destination admits the socket only when its `Origin` is a neighbouring
  world equal to the visa's `home`. It then posts the visa to
  `<home api>/api/world/visa/verify` (4-second timeout, redirects refused).
  Home checks the signature, lifetime and destination and returns the name.
  The visitor joins as `Registered` under that name, with the visa nonce as
  its single-use ticket nonce. A guest travels without a visa and joins as a
  guest. Either way the relay sets the public field `home` to the origin's
  host. The visitor's account key is namespaced (`name@home`), so it never
  displaces a local account with the same name. Visitors get no node belt, no
  face image, no wallet and no admin rights.
- **Limits.** The page shows at most 32 peers per neighbouring world. Flight
  and rides keep `WORLD_RADIUS`, so travel is on foot. Peer coordinates on the
  home socket are clamped to ±1900 (they were ±512, which also clamped people
  walking the desert ring). Federated relay portals in the fediverse district
  are a separate list and still open a tab.

## Activities

- **Quadcopter flight** (`updateQuadcopterRide()`): flies where the camera
  looks; holding forward accelerates from 42 to 150 units/s.
- **Drone racing** (`scene/drone-race.js`, `scene/runtime/drone-race.js`):
  ten rings and a gate weaving around the coaster; passes are segment tests
  against each ring plane; the best lap and its ghost persist under
  `forkmesh.world.droneRace.v1`. Movement frames carry `race: {ring, lap,
  best}`, which the relay (`sanitize_message()` in `world/src/world.py`)
  clamps and never puts in presence snapshots.
- **Coaster** (`scene/coaster.js` maths, `scene/districts/coaster.js` meshes,
  `scene/runtime/coaster.js` ride): a lap around the town hexagon with loops,
  a ballistic jump, corkscrew and helix; the track is resampled into typed
  arrays and drawn as LOD chunks with one shared material; the train is one
  `InstancedMesh`. Presence frames carry `COASTER_RIDING_ACTIVITY`.
  In first person, `placeCoasterEye()` puts the eye `FIRST_PERSON_EYE_HEIGHT`
  along the seat's up axis and takes the seat's whole orientation, so the view
  pitches, banks and inverts with the car instead of staying level on world up.
  Drags while riding are kept as `ride.lookYaw` (seat-relative yaw, reset to
  forward when first person is switched on) and `firstPersonPitch` is applied
  in the seat frame, so a glance to the side holds through loops where
  `followHeading()` stops tracking the world heading. `seatRider()`
  re-orthogonalises the track's interpolated up against its tangent so the
  seat quaternion is a clean rotation. Test:
  `test_the_first_person_eye_turns_over_with_the_seat_and_keeps_a_glance`.
- **Bikes** follow the road graph (`districts/roads.js`); tapping left or
  right before a junction picks a branch, holding leaves the road, and riding
  onto a road along it rejoins. Tests: `world/tests/test_world_bike_junctions.py`.
- **Parachute** (`parachuteSinkVelocity()`), **gym** (`poseAvatarArms()`),
  **avatars** (the back name board is a child of the torso).
- **Office aquarium reef controller** (`scene/aquarium-panel.js`): a physical
  panel of buttons, a dial, switches and a lever on the tank (feed, tap,
  follow, school mode, backdrop, glass response, light), picked only in the
  lobby through invisible hit pads tagged `interactive: "aquarium-control"`.
  Settings persist in `localStorage` (`forkmesh-world-aquarium-*`); fish
  steering is a boids step over typed arrays (`scene/aquarium-schooling.js`).

## Related docs

- [docs/app/README.md](../app/README.md) (the App),
  [sessions](../app/auth-sessions.md),
  [footprint](../app/worker-footprint.md)
- [docs/deploy/README.md](../deploy/README.md),
  [docs/deploy/auto-deploy.md](../deploy/auto-deploy.md),
  [docs/PLAN.md](../PLAN.md), [docs/OPERATIONS.md](../OPERATIONS.md)
- [world/README.md](../../world/README.md),
  `world/public/world/elements/README.md`
- `www/docs/world-community-services.md`
