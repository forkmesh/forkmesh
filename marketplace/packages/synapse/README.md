# Synapse

See how the files of a repository connect, as a 3D brain. Every file that
imports or is imported by another file is a neuron, every import a lit fibre
from the importing file to the imported one, and files in a circular
dependency glow red. It works for every language ForkMesh's analysis engine
reads, on any local repository or folder.

## How to use it

- Open **Synapse** from the rail. It follows the repository you have open; pick
  another one or **Folder…** in the header.
- Drag to orbit, right-drag to pan, scroll to zoom, click a file to select it,
  double-click to fly to it, Esc to clear. Space toggles auto-rotate, F fits.
- A selected file lights what it imports (cyan) and what imports it (purple).
  **Depth** widens that to 2 or 3 hops; **Show → Selected neighbourhood** hides
  the rest.
- **Show → Circular dependencies only** keeps the loops; clicking a loop in the
  panel isolates it and flies there. **Show → Orphans** shows files nothing
  imports and that import nothing.
- **Colour** switches between module, language, code health and hotspot.
- The panel lists the selected file's imports, importers, external packages
  and loop, or, with nothing selected, the loops, orphans and the most imported
  and most importing files. `/` or Ctrl+F finds a file.

## How it is computed

- Imports come from the shared engine (`dependsOn`); loops are its strongly
  connected components, shown as the shortest readable cycle
  (`a → b → c → a`).
- Files are grouped into lobes by module (folder). A deterministic force layout
  shapes them into a brain: repulsion (Barnes–Hut above 1,500 files), springs
  along imports, a pull toward the lobe, a cortex force that keeps files in a
  folded shell under the surface, and a fissure between the hemispheres.
- Sphere size grows with log(lines of code) and the number of importers.

Details: [docs/marketplace/synapse.md](../../../docs/marketplace/synapse.md).
