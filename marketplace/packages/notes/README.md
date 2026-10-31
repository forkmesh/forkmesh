# Notes plugin

The bundled Notes package owns the dashboard route, navigation contribution,
permissions, package metadata, and web entrypoints for encrypted collaborative
Markdown notes. Its host-provided `/api/notes` service and live note room are
shared by the web and desktop clients.

The package is discovered exclusively through `marketplace/catalog.json`.
Removing that catalog entry removes its staged dashboard page and navigation.

On the desktop, Notes opens in its own window and answers the top search box
(`content/desktop/NotesPage.cpp`, built as its own module, `notes.so`).
