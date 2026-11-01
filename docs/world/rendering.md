# World rendering

Part of [World](README.md). How the three.js scene is put together and kept
fast: scene modules and the build order, the one resource authority for
geometries and materials, instancing and culling, the loading pipeline and
the diagnostics panels. The performance gate and render traps are on
[development](development.md#performance-gate).

## Scene modules

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

## Resource authority

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

## Instancing and culling

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
  re-measures their bounds; otherwise the moved Office and beach would blank
  out while in view.
- **Office aquarium on demand.** The lobby reef is built in idle slices when
  the camera comes within the Office detail distance, warmed off-scene, then
  attached; it is released after `OFFICE_AQUARIUM_RELEASE_MS` (15 s) beyond
  the build distance plus `OFFICE_AQUARIUM_RELEASE_MARGIN`.
  `whenOfficeAquariumReady()` starts or joins the build. Its point lights are
  budgeted through `updateLocalPointLightBudget()`, which pads with dark
  slots so the light count never changes.

## Loading pipeline

- **One loader.** The loading card is static markup in `index.html`, outside
  `<forkmesh-world>`, with inline styles and fixed ledger rows matching
  `WORLD_BOOT_STEPS`. It stays hidden until the document is parsed
  (`data-parsed="true"`). `<forkmesh-world>` ships `data-world-booting inert`
  at opacity 0; at 100 % `revealWorld()` fades the card, removes both
  attributes, records `WORLD_BOOT.interactive()` and fires
  `forkmesh:world-interactive`. The WebGL fallback, an import failure and a
  10 s watchdog also remove the card.
- **Compact retry before the fallback.** `prepareWorldRenderer()` opens the
  WebGL2 context early. If the full plan's context cannot be created (a
  software GL such as Mesa llvmpipe, or a GPU process that fails
  `BindToCurrentSequence`), it tries once more with the compact plan
  (no antialias, default power preference) and, if that one opens, switches
  the visit to safe mode so `createWorldScene` builds the matching compact
  renderer. The "could not start the 3D renderer" fallback shows only when
  both fail. A context whose `WebGLRenderer` throws is released at once.
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

## Diagnostics

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
  stats, whose walk runs in idle slices. The heaviest three World elements
  (once a minute) are counted inside the same sliced walk, not by a
  synchronous `listWorldElements()` call, which blocked for up to half a
  second on slow machines.
- **Visibility.** Shown to admin tickets and to anyone with Settings → Debug
  → "Show debug panel"; otherwise recorded but hidden. The panel is
  draggable, can collapse to a pill, and "Copy log" copies the summary,
  "Optimization suggestions", the event lines and the optimization audit.
- **Performance bar and profiler** (`createWorldProfiler`, only while an
  admin bar is shown): frame phases, per-callback cost, draw-call attribution
  by World element, GPU timer queries; `auditWorldScene` finds duplicate
  geometries/materials, instancing candidates and culling opt-outs. Both
  copy paths (the drawer's Copy button and the boot log's report provider)
  go through `perfCopyReport()`, which samples a fresh profile window and
  re-runs the audit on it, so the findings and the Frame budget table
  describe the same frames.
- **Render stall monitor** (`scene/render-stalls.js`): splits frames over
  50 ms into program compiles, texture uploads, shadow and reflection draws,
  and scene update parts (matrix walk, animation, labels, camera). Long
  frames without scripts are split into pre-render tasks, callbacks and
  style/layout/paint.
