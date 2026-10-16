# Tasks page: status tabs, member avatars and layout

The web Tasks page shows seven status tabs (Open, In progress, Mine, QA, Done,
Trash, All) as a segmented control: icon, label and count. The labels hide
when the toolbar is narrower than 46rem (a container query), leaving the
icons and counts. A people strip (`[data-tasks-people]`: Everyone, each
member's avatar, Bot, Unassigned) shows each person's open task count.
Clicking an avatar filters to that person; clicking it again returns to
Everyone.

## Layout (Tracker)

In October 2026 the page was redesigned from two candidate layouts the user
compared in an interactive prototype ("A · Tracker" and "B · Focus sheet"); they
picked Tracker, modelled on Linear and Plane issue views. Every earlier feature
stays; only the arrangement changed. The reasons: the old detail pane showed
all twelve actions at once with most of them disabled, boxed every section, and
gave status, priority and progress no shape to scan by.

- Header: "Tasks" title, one-line muted description, a muted stats line
  (`[data-tasks-count]`, in progress, bot queue, awaiting QA), and on the right
  Clear closed, Refresh and the single green New task button
  (`data-variant="cta"`).
- Filter bar: search, the segmented status control and the people strip
  (`[data-tasks-people]`), all on one row.
- Workspace: one bordered box, list on the left (up to 34rem) and detail on the
  right. Both panes scroll on their own inside a box sized to the window
  (`calc(100dvh - 17rem)`, at least 34rem); under 960px the box stacks and the
  page scrolls.
- List: rows are grouped by state (In progress, Bot queue, Ready, Done) under
  sticky headers that collapse on click (`taskCollapsedGroups`). Trash is not
  grouped. A row is one line: priority icon, status ring, title, work item and
  comment counts, the `#department` label, image thumbnails, assignee avatar and
  a short age (`3d`). The ring (`taskRingHtml`) is the row's progressbar: its
  inner pie fills with progress, it is blue while in progress or queued (dashed
  for the bot queue) and a green check when done. J and K move the selection.
- Priority icon (`taskPriorityHtml`): P1 to P5 is Urgent (amber square), P6 to
  P15 High (three bars), P16 to P40 Medium (two), above that Low (one).
- Detail body: crumbs (repository, `#department`, the first 8 characters of
  the task ID, which copies the full ID), the title, then the action bar:
  one solid button for the likely next step (Start, Complete, Remind, Mark
  undone or Restore, `taskActionLayoutHtml`), up to three other actions that
  apply now, and a More menu holding the rest. Actions that do not apply stay
  in the menu, greyed out, so nothing disappears. Below come the check-in and
  remind panels (boxed, `data-panel`), Description, completion note, work
  items, images, completion history and Activity.
- Detail side panel (`.task-detail-aside`): Status, Priority, Assignee, Created
  by, Follows, Repository and Routing; Progress with its slider; the persistent
  reminder switch and interval; Timing; Quality assurance with How to test;
  Agent run, Bounty request, Latest check-in and Last reminder when present. It
  moves under the body when the detail pane is narrower than 44rem (container
  query).
- Empty, loading and error states use one centred blankslate
  (`.task-blankslate`).

Colours come from the dashboard tokens scoped as `--task-*` variables on
`[data-view="tasks"]`, so both themes follow `shell.html`; subtle surfaces use
`--dashboard-subtle-rgb` because `--dashboard-secondary-rgb` is white in the
light theme.

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
