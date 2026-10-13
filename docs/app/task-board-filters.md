# Tasks page: icon tabs with counts and member avatars

Added 2026-10-06 (agent 1406, continuing cloud run
`dcf1f9e448ab5b4cc450605b2cf12d18`). The request was "make the tabs like icons
with counts and add avatars for org members so we can check others tasks".

The seven status tabs on the web Tasks page (Open, In progress, Mine, QA, Done,
Trash, All) used to be text buttons. Next to an "All assignees" dropdown they
overflowed on phones, and you had to open a filter to see how much work it held.
The organization's members were only names in that dropdown.

## What the page shows

- **Status tabs** are lucide icons (`circle-dot`, `activity`, `user-round`,
  `flask-conical`, `circle-check`, `trash-2`, `list`), each with a count. The
  tab name is in `title` and `aria-label`, for example "Open: 3".
- **People strip** (`[data-tasks-people]`) replaces the dropdown with one row:
  Everyone, each member's avatar (you first, then A to Z), Bot and Unassigned.
  Each button shows that person's **open** task count. Clicking an avatar shows
  only their tasks; clicking it again goes back to Everyone. Avatars use
  `taskAvatarHtml` and `hydrateTaskAvatars`, the same initials-then-profile-PNG
  path as the task rows.

## How counts are computed (`taskFilterCounts`, `taskPeopleOpenCounts`)

- Status tab counts follow the selected person and the search box. Selecting
  `@wendy` turns "Open" into Wendy's open tasks.
- **Mine** always counts your own open tasks, whoever is selected. Picking Mine
  resets the person to Everyone, as the dropdown did.
- **Trash** has no count until the trash has been loaded. Its count is not part
  of the board read, and fetching the trash only to fill a badge would add a
  relay request per page view (see the de-polling notes).
- Avatar counts ignore the search box and the status tab. They answer "how much
  open work does this person have", which is what you want before you click.

## Where the markup lives

The toolbar is copied into three files, and all three must change together:

- `app/public/dashboard/partials/views/tasks.html` (the dashboard partial),
- `app/public/dashboard/tasks/index.html` (hand-maintained, not rebuilt by
  `build_dashboard_assets.py`),
- `marketplace/packages/tasks/content/web/view.html` (the packaged view the
  router serves; the package went to 1.0.5).

The behaviour is in `app/public/dashboard/js/09-tasks.js`. Run
`python3 app/tools/build_dashboard_assets.py` afterwards to rebuild
`dashboard-page-tasks.js` and the chunk hash in `dashboard.js`. Tests:
`app/tests/test_dashboard_tasks_management.py` and
`app/browser_tests/tests/dashboard-task-progress.spec.js` ("status tabs are
icons with counts and member avatars filter the board").

## Desktop and phones (2026-10-06)

The same request was then carried to the other clients, with the same rules:
tab counts follow the selected person, Mine always counts your own tasks,
avatar counts are open tasks only, clicking the selected face again shows
everyone, picking a person leaves Mine, and picking Mine resets the person.

- **Desktop (Qt):** Mine / Open / Closed / All tiles are captioned with their
  count, and a people strip under the toolbar replaces the "All assignees"
  menu. See [Tasks page](../desktop/tasks-page.md#icon-tabs-with-counts-and-the-people-strip-2026-10-06).
- **iOS:** `TasksListModel` (FMTools) owns the people, counts and selection
  rules; `TasksScreen` draws icon tabs (`FmSegmentTab.icon`) and a chip row
  using `MessagesPersonAvatar`. See `native/ios/docs/FMTools.md`.
- **Android:** `TasksLogic.kt` (core) owns the same rules; `TasksScreens.kt`
  draws icon tabs (`FmSegmentTab.icon` in `FmExplorer.kt`) and a
  `TaskPeopleStrip` using `FmAvatar` with `ApiService.accountAvatarPng`.

The phones count a user task with no assignee name as Unassigned, which is
the label their rows already showed for it. The web counts that task under
no one.
