# Ember

File-level hotspots for any repository on this computer or phone, our take on
CodeScene's hotspot map. Every source file is a circle sized by its lines of
code, nested inside its folders. Tabs colour the circles by code health
(green, yellow, red), change frequency, refactoring targets, change coupling,
knowledge (main author and former contributors), bug-fix commits and
language.

## How to use it

- Open **Ember** on the rail. It analyzes the repository you have open, or
  any other one picked in the header (**Folder…** analyzes any folder).
- Click a folder to zoom in, click outside it or press Esc to zoom out. Use the
  breadcrumb to jump back up. Scroll to zoom and drag to pan.
- Click a file to see its card: health, commits, size, main author,
  knowledge, defects, complexity, functions, duplication, imports and its
  health issues. The complexity trend comes from git history. Yellow links on
  the map and the Coupling tab show the files it changes together with.
  Double-click a file, or press **Open file**, to open it in the editor.
- Narrow the map with the filename search (`/` or Ctrl+F), the author filter,
  the code-health range and the commit threshold. Files that do not match are
  dimmed, not hidden.
- **Refactoring targets** ranks the files to improve first.

## How scores are computed

- **Code health** (1–10, 10 best) comes from the shared engine. It drops for
  long or complex functions, deep nesting, many parameters, large files,
  duplicated blocks and import cycles. 8 and up is healthy, 4 and up
  problematic, below 4 unhealthy.
- **Hotspot** (0–1) combines change frequency in the history window
  (default one year) with file size.
- **Refactoring target score** is `hotspot × (10 − health)`; the top 15 are
  ranked.
- **Change coupling degree** is the share of the two files' commits that
  changed both.

Without git history only Code health and Languages are available.

## On phones

Also on iPhone and Android: open it from Home → Insights, a repository's
menu, or a repository copied onto the phone. The phone analyses a copy of the
repository fetched into the app (or the copy already on the phone) with a
port of the desktop engine, so the numbers are the same. See
`docs/marketplace/codebase-analysis.md#on-phones`.

Full documentation: `docs/marketplace/ember.md`.
