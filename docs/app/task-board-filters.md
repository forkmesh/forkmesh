# Tasks page: icon tabs with counts and member avatars

The web Tasks page shows seven status tabs (Open, In progress, Mine, QA, Done,
Trash, All) as lucide icons with counts, and a people strip
(`[data-tasks-people]`: Everyone, each member's avatar, Bot, Unassigned) with
each person's open task count. Clicking an avatar filters to that person;
clicking it again returns to Everyone.

## Count rules (`taskFilterCounts`, `taskPeopleOpenCounts`)

- Tab counts follow the selected person and the search box.
- **Mine** always counts your own open tasks; picking it resets the person.
- **Trash** has no count until the trash is loaded (no extra request per view).
- Avatar counts are open tasks only and ignore search and tab.

## Where it lives

The toolbar markup exists in two files that must change together:
`app/public/dashboard/partials/views/tasks.html` and
`marketplace/packages/tasks/content/web/view.html`;
`app/public/dashboard/tasks/index.html` is composed from the first. Behaviour is in
`app/public/dashboard/js/09-tasks.js`; rebuild with
`python3 app/tools/build_dashboard_assets.py`. Tests:
`app/tests/test_dashboard_tasks_management.py`,
`app/browser_tests/tests/dashboard-task-progress.spec.js`.

The desktop ([Tasks page](../desktop/tasks-page.md)), iOS (`TasksListModel`)
and Android (`TasksLogic.kt`) apply the same rules.
