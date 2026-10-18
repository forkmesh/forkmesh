# Synapse

`marketplace/packages/synapse` (Market id `synapse`, entry point
`builtin:synapse`, rail label **Synapse**, icon `brain`) shows how the files of
a repository connect, as a 3D brain. It is our own take on Madge: every file
that imports or is imported by another file is a neuron, every import is a lit
fibre from the importing file to the imported one, and files in a circular
dependency glow red and pulse. It works for every language the shared engine
resolves imports for (see [codebase-analysis.md](codebase-analysis.md)), not
only JavaScript and TypeScript, on any repository open in ForkMesh or any
folder picked with **Folder…**.

## Code map

| Path | Responsibility |
| --- | --- |
| `content/desktop/SynapseLayout.{h,cpp}` | Pure logic on a worker: the graph (`buildGraph`), lobes, the brain-shaped 3D force layout (`runLayout`), and the queries: cycles and readable cycle paths, orphans, leaves, most imported, most importing, N-hop neighbourhoods |
| `content/desktop/SynapseView.{h,cpp}` | The 3D view: an OpenGL surface (`QOpenGLWidget`, shaders for GL 2.1 and GLES 2) with a QPainter software renderer for everything else, camera, picking, labels, legend |
| `content/desktop/SynapseFeature.{h,cpp}` | The rail page: `RepoInsightBar`, the controls, the right-hand panel, search; runs the layout on a worker and registers `builtin:synapse` |
| `content/desktop/tests/test_synapse.cpp` | `forkmesh-synapse-tests`, plus the preview mode used for the Market screenshot |

## The page

`RepoInsightBar` sits on top (title **Synapse**, subtitle "How files connect",
repository picker, **Folder…**, **Re-analyze**, status). Its action row holds:

- **Colour**: Module (one colour per lobe), Language, Health (the shared
  healthy / problematic / unhealthy colours) or Hotspot (change frequency ×
  size, slate to orange). Files in a circular dependency are red in every mode.
- **Show**: Everything, Circular dependencies only, Orphans, Selected
  neighbourhood. Orphans are not part of the import graph, so choosing it lays
  the brain out again with the isolated files included (the result is cached
  per snapshot, so switching back is instant).
- **Depth**: 1, 2 or 3 hops of neighbourhood around the selected file.
- **Auto-rotate** (on by default; Space toggles it) and **Pulses**.
- **Fit** (or F) frames what is visible.

The status line ends with Synapse's summary, for example
`22,481 imports · 42 circular dependencies · 1,070 orphans`.

The right-hand panel has a **Find a file…** box (`/` or Ctrl+F focuses it; it
completes on any part of the path and Enter or choosing a completion selects
the file and flies to it) and below it:

- With a file selected: name, path, language, lines, health, module, **Open
  file** (opens it in ForkMesh's editor), **Fly to**, **Clear**, the loop it
  belongs to as a readable path (`a.cpp → b.cpp → c.cpp → a.cpp`, with
  **Show only this loop**), **Imports** and **Imported by** as clickable lists,
  and the external packages it uses (imports that did not resolve to a file of
  the repository).
- With nothing selected: counts (connected files, imports, circular
  dependencies, orphans, leaves, modules) and the lists **Circular
  dependencies** (largest first; a click isolates that loop and flies to it),
  **Most imported**, **Most importing** and **Orphans**.

The window's top search box, while the page is visible, highlights matching
files in the brain and dims the rest; its popup lists matching files and
choosing one selects it and flies to it.

## The brain

### Graph

Nodes are the files that take part in the import graph (both directions;
self-imports ignored); the Orphans view adds the isolated files. Edges are the
engine's `dependsOn` links, importer → imported, one per pair. Files are grouped
into lobes with `buildModuleGraph` (the engine's automatic folder depth); if
that is not available the layout groups by folder itself, choosing the
shallowest depth that gives a handful of groups.

Cycles come from `Snapshot::cycles` (strongly connected components). For each
one Synapse keeps the edges inside it and a readable loop: the shortest cycle
through the member with the alphabetically first path, found by a breadth-first
search restricted to the component. A component can be much larger than that
loop (type references in Swift or Kotlin often make one component of hundreds
of files); the panel says how many files take part.

- **Orphans**: nobody imports them and they import nothing in the repository.
- **Leaves**: they import nothing in the repository but are imported.
- **Most imported / most importing**: ranked by importers / imports.
- **Neighbourhood**: breadth-first up to N hops along imports and, separately,
  along importers, keeping the hop distance of each file.

### Layout

The brain is an ellipsoid wider front to back (half length 124) than across
(half width 96, narrowing slightly toward the front) or tall (76 above the
centre, 58 below, so the base is flatter). The two hemispheres are split by a
central fissure along the x = 0 plane.

1. **Lobe anchors.** Lobes start on a Fibonacci lattice over the sphere, ordered
   by module layer (foundation modules toward the back) and size, then relax
   for 160 steps: lobes repel in proportion to the square root of their file
   counts and are pulled together by springs weighted by log(imports between
   them), staying on the unit sphere. Each anchor is then placed halfway
   through the cortex shell, at least a quarter of the width away from the
   fissure, which fixes the lobe's hemisphere.
2. **Seeds.** Each file starts near its lobe anchor with a deterministic
   jitter from an FNV-1a hash of its path (never `qHash`, whose seed changes
   per process), within a radius that grows with the cube root of the lobe
   size. A single-lobe graph spreads its files over both hemispheres instead.
3. **Forces**, for 300 iterations (fewer above 5,000 files, down to 120):
   - repulsion k²/d between all pairs, exact up to 1,500 files and a
     Barnes–Hut octree (θ = 0.85, leaves of up to four files) above;
   - log springs along imports, k·log(d/k), so long cross-lobe fibres pull
     gently instead of collapsing the brain;
   - a pull toward the lobe anchor;
   - the cortex force: below 0.4 of the brain radius files are pushed out,
     above the folded outer surface they are pushed in, so the graph forms a
     shell that reads as a brain. The outer surface is lowered by a groove
     pattern, sin(10θ + 2.6 sin(5φ + 1.3θ)), that suggests gyri;
   - the fissure force keeps each file on its hemisphere's side.

   The step is capped by a temperature that cools quadratically, and every
   step ends with hard limits (inside the brain, on the right side of the
   fissure), so the same input always gives the same positions.

   The ideal distance k is the cube root of the shell volume per file. The
   repulsion runs in parallel over fixed chunks with `QtConcurrent::blockingMap`;
   each file sums its own forces in a fixed order, so threads do not change
   the result.
4. **Growing.** The worker publishes positions about every 45 ms; the page
   shows them as they come, so the brain visibly grows into shape. Only one
   update is in flight at a time (later ones are skipped until the GUI thread
   has taken it), updates are posted with `QMetaObject::invokeMethod` and
   checked against a `QPointer` and a generation number, and a new snapshot or
   repository cancels the running layout.

On this repository (5,700 files, 4,630 connected, 22,481 imports) the layout
takes about five seconds on a worker in a debug build.

### Rendering

Both renderers draw the same scene:

- **Nodes** are lit spheres sized by log(lines of code) and the square root of
  their importers. On the GPU each node is a camera-facing quad (four vertices,
  the corners expanded in the vertex shader); the fragment shader rebuilds the
  sphere normal from the quad position and lights it with a key light, a fill
  light, a specular highlight and a fresnel rim, then draws a soft additive
  glow halo three times the radius. Files in a cycle are red and pulse.
- **Fibres** are quadratic béziers that bow 26% of the way toward the brain
  centre, drawn as screen-space ribbons with additive blending and depth fade.
  A signal travels along each fibre from importer to imported. With more than
  1,500 imports, signals run only along highlighted fibres. At most 16,000
  fibres are drawn on the GPU and 5,000 in software, chosen by a stable hash
  so the sample does not flicker; highlighted and cycle fibres always come
  first. Loops of more than 40 files tint their fibres red instead of drawing
  them all in full red, so one huge component does not hide the rest.
- **Atmosphere**: a dark blue gradient with a vignette and a glow behind the
  brain, a field of stars far away that moves with the orbit (parallax),
  "cortex dust" (1,800 faint points on the folded brain surface) so the shape
  reads even for small repositories, and depth fog that fades far nodes.
- **Labels** (QPainter over the GL pass): the hovered and selected files, up to
  28 neighbours (nearest hops first), the ten most imported files when nothing
  is selected, and lobe names; labels that would overlap are skipped. A legend
  at the bottom left explains the colours; a hint at the bottom right lists
  the mouse gestures; a hover card shows the path, imports, importers and
  lines.

The GPU is used when Qt's OpenGL widgets are built in (`FORKMESH_HAVE_OPENGL`)
and a context starts. Like the agent activity view, the offscreen and minimal
platforms and a failed context use the software renderer, and so does a shader
that fails to compile. `FORKMESH_SYNAPSE_GPU=0` forces software;
`FORKMESH_SYNAPSE_GPU=1` skips the platform check (for testing). The shaders
use `#version 120` with `attribute`/`varying` on desktop GL and `#version 100`
with `precision mediump float` on GLES, chosen from
`QOpenGLContext::isOpenGLES()`; no VAOs. Nodes are drawn in batches of 16,384
quads with a shared 16-bit index buffer so GLES 2 works; fibres are one triangle
strip with degenerate joins. The fibres ignore the depth buffer so spheres in
front do not cut them into dashes; halos and spheres use it.

The software renderer sorts nodes back to front and paints cached sphere and
halo sprites (radial gradients rendered once per colour and size), fibres as
alpha paths with additive composition, and the same atmosphere. It runs at
about 30 frames a second; while something moves it thins ordinary fibres to
about 1,200 and skips ordinary halos above 1,500 nodes, then repaints at full
quality once the scene is still.

### Interaction

Left drag orbits, right drag (or Shift+drag) pans, the wheel zooms toward the
point under the pointer, a click selects (picking projects node centres on the
CPU; the nearest front node wins), a click on empty space clears, a double
click flies to the file (eased over 0.9 s) or, on empty space, fits. Esc clears
the selection and an isolated loop. Auto-rotate pauses while you drag and for
2.5 s after you stop. The render timer runs only while the view is visible and
something moves (rotation, a flight, signals, pulsing cycles).

A selection lights the files it imports in cyan and the files that import it
in purple (#bc8cff); everything else dims. Fibres along the neighbourhood
(each step one hop further from the selected file) are highlighted.

## Tests and preview

`forkmesh-synapse-tests` covers the queries on hand-built snapshots (cycle
path and text, orphans, leaves, rankings, neighbourhoods by direction and
depth), graph building (isolated files, cycle edges, lobes), layout
determinism, bounds and hemispheres, lobe separation, the Barnes–Hut path and
cancel, picking, the software render not being blank in every colour mode, the
show modes, and the page offscreen (layout on a worker, search hits, orphan
re-layout, cycle isolation).

With `FORKMESH_INSIGHT_PREVIEW_ROOT` and `FORKMESH_INSIGHT_PREVIEW_OUT` set the
test binary instead renders the page at 1600 × 1000 in the dark theme through
the software renderer: the brain seen from above and in front so both
hemispheres show, with a well-connected file facing the camera selected (the
score is log(1 + importers) × log(1 + imports), weighted by how much the file
faces the camera). `FORKMESH_INSIGHT_PREVIEW_VIEW` can be `overview` (nothing
selected), `cycles` (the largest loop isolated), `language`, `health` or
`hotspot`.

## On phones

Synapse runs on iPhone and Android; the shared parts are in
[codebase-analysis.md](codebase-analysis.md#on-phones).

| Platform | Model | Page |
| --- | --- | --- |
| iOS | `FMCodeInsight/Synapse{,Math,Models,Layout,Scene,Camera}.swift` | `ForkMesh/Features/CodeInsight/Synapse*.swift` |
| Android | `core/.../codeinsight/Synapse*.kt` | `app/.../feature/codeinsight/Synapse*.kt` |

The models port `SynapseLayout` (graph, lobes, the Barnes–Hut force layout
with the same seed and operation order, cycles and cycle paths, orphans,
leaves, rankings, neighbourhoods) and are checked against
`native/fixtures/insight/synapse.json`, written by
`FORKMESH_SYNAPSE_GOLDEN=<fixture> forkmesh-synapse-tests`. The graph, lobes
and queries match exactly; positions match to rounding (see
[codebase-analysis.md](codebase-analysis.md#parity-tests)). Laying out 4,600
files with 18,000 imports takes about 2.4 s in an iOS release build on a
desktop CPU, off the main thread with progress.

On the phone the brain has no GPU path: a software perspective projection
draws it on a canvas every frame, always on the dark space background (the
glow only reads on dark); controls and sheets follow the app theme. Layouts
are cached per snapshot and per "with orphans or not".

- Neurons are depth-sorted and sized by degree; tiny ones are batched dots,
  larger ones pre-rendered spheres with halos. Fibres are batched by colour
  and depth, at most 5,000 at once (dimmed ones capped at 1,200) as in the
  desktop software renderer, thinned while anything moves (to about 2,000
  on iPhone and 1,200 on Android).
- One finger rotates, pinch zooms, a tap picks the nearest neuron within
  28 pt (empty space clears), a double tap fits. Auto-rotate is on and pauses
  while touching; Reduce Motion (Android: animator duration scale 0) turns
  off auto-rotate, pulses and the cycle glow and makes camera moves jump.
- Chips choose **Colour** (Module, Language, Health, Hotspot) and **Show**
  (Everything, Circular only, Orphans, Neighbourhood with depth 1 to 3).
  **Find a file** suggests files and flies to the one picked.
- Tapping a neuron selects it and shows a small card; **Details** opens the
  sheet, **Overview** opens the lists.
- The sheet shows the selected file (path, language, lines, health, module,
  **Open file**, **Fly to**, **Clear**, its loop as `a → b → c → a` with
  **Show only this loop**, Imports, Imported by, external packages) or the
  overview (counts, circular dependencies, most imported, most importing,
  orphans). It is also the screen-reader view: every fact is reachable
  without the canvas.
