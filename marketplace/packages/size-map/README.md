# Size Map

The repository **Size map** tab as a removable Market package: a sunburst
of what takes disk space in the repository folder, any folder you pick or a
whole drive, with git-ignored files hidden or shown.

The feature code stays in the desktop (`MainWindow::buildSizeMapTab`,
`DirectorySizeScan`); this package is its on/off switch. Removing it hides
the tab, and the footer's disk sparkline opens the resource diagnostics
instead. Projects created before 2026-10-17 keep it installed.

See [docs/marketplace/codebase-analysis.md](../../../docs/marketplace/codebase-analysis.md).
