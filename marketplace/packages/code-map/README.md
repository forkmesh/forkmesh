# Code Map

The repository screen's **Code map** button as a removable Market package.
It opens a window with the whole codebase at the browsed branch as a
zoomable treemap, each file's source drawn in its cell, with a toggle to a
rotatable 3D view where a folder's height follows its size.

The feature code stays in the desktop (`desktop/src/MainWindowCodeMap.cpp`,
`CodeMapModel`, `CodeMapView`); this package is its on/off switch. Removing
it hides the button. Projects created before 2026-10-17 keep it installed.

See [docs/marketplace/codebase-analysis.md](../../../docs/marketplace/codebase-analysis.md).
