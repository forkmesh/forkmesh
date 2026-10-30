# Tasks plugin

The bundled Tasks package owns the dashboard route, navigation contribution,
permissions, package metadata, and web entrypoints for ForkMesh's shared
organization task catalog. Its host-provided service remains `/api/tasks` so
the web app, desktop client, agents, MCP, and World office all operate on the
same encrypted task records.

The package is discovered exclusively through `marketplace/catalog.json`.
Removing that catalog entry removes its staged dashboard page and navigation.

Task boards can be filtered by assignee, and tasks can carry images shown on
the desktop and the web. On the desktop, the Tasks page is its own module
(`content/desktop/TasksPage.cpp`, built as `tasks.so`) and answers the top
search box from the board it holds; the catalog and the signed relay requests
stay in the app (`desktop/src/OrganizationTaskStore.cpp`).
