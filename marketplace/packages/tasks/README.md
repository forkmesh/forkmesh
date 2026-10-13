# Tasks plugin

The bundled Tasks package owns the dashboard route, navigation contribution,
permissions, package metadata, and web entrypoints for ForkMesh's shared
organization task catalog. Its host-provided service remains `/api/tasks` so
the web app, desktop client, agents, MCP, and World office all operate on the
same encrypted task records.

The package is discovered exclusively through `marketplace/catalog.json`.
Removing that catalog entry removes its staged dashboard page and navigation.

Task boards can be filtered by assignee, and tasks can carry images shown on
the desktop and the web. On the desktop, Tasks answers the top search box
(`desktop/src/MainWindowTasks.cpp`, `desktop/src/MainWindowSearch.cpp`).
