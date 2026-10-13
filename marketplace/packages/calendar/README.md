# Calendar plugin

The bundled Calendar package owns the dashboard route, navigation
contribution, permissions, package metadata, and web entrypoints for personal
and organization calendars. Its host-provided `/api/calendar` service is
shared with the desktop client and reminder scheduler.

The package is discovered exclusively through `marketplace/catalog.json`.
Removing that catalog entry removes its staged dashboard page and navigation.

On the desktop, Calendar answers the top search box
(`desktop/src/MainWindowCalendar.cpp`, `desktop/src/MainWindowSearch.cpp`).
