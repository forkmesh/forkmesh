# Desktop: the Tasks page

Part of [the desktop docs](README.md). Code: the page is the Tasks package's
own module, `marketplace/packages/tasks/content/desktop/TasksPage.{h,cpp}`
(`TasksPage`, `buildOrganizationTasksSection()`, `OrganizationTaskRowDelegate`,
`taskDetailHtml()`, `taskFieldsHtml()`), built as `build/packages/tasks.so`; the
catalog and the signed relay requests in
`desktop/src/OrganizationTaskStore.{h,cpp}`; the window's side of the page
(quick add, agent launch, move to issue, attach worktree, the window commands)
in `desktop/src/MainWindowTaskBridge.cpp`; work items in
`desktop/src/MainWindowTaskWorkItems.cpp` with testable rules in
`desktop/src/TaskWorkItems.{h,cpp}`; agent links in
`desktop/src/MainWindowAgentTaskLink.cpp`; Markdown in
`desktop/src/TaskMarkdown.{h,cpp}`.

## Page and window

The page is an ordinary native package page for `builtin:tasks`: it is on the
rail under the key `tasks` while the package is installed, and it never
includes `MainWindow.h` or `MainWindowInternal.h`
([package modules](package-modules.md#plugins-built-apart-from-the-window)).
What the rest of the app needs from tasks stays in the core:

- **`OrganizationTaskStore`** holds the catalog (tasks, actor, members,
  departments, manager flag), makes every `/api/tasks` request (session token,
  else the key-signed proof from `organizationTaskProof()`), pages the catalog
  and remembers the open count behind the rail badge (`tasks/openCount`). The
  window creates it at startup (`initOrganizationTaskStore()`); the page finds
  it with `OrganizationTaskStore::current()`. Agents, the agent task picker,
  Stats, Cleanup, Branches and quick add read the store, so they work whether
  or not the page has been built, or the package installed.
- **Catalog reads.** The window's one unprompted read is 25 s after launch
  (`refreshOrganizationTaskBadge()` → `refreshOnce()`); a `tasks` push calls
  `handleEvent()`, which re-reads or, while a read is in flight, remembers to
  re-read when it lands. Every answer is `catalogLoaded`: the window links
  local agent sessions to tasks, refreshes branch links and the quick-add
  assignees; the page renders it. The page renders the store's last answer
  when it is first built instead of asking again.
- **The page's own state**: its working copy of the list with this device's
  optimistic edits laid over catalog reads (an edit wins until a read at least
  as new by `updatedAt` arrives), per-task timelines and comment drafts,
  images, filters and the view.

The page reaches the window through `NativePackageHost`:

| Call | Command | What the window does |
| --- | --- | --- |
| `windowQuery` | `task.agentSessions` | the local agent sessions linked to a task (id, provider, model, status, branch) for badges, spinners and the branch column |
| `windowQuery` | `task.repositoryLogos` | the `logo.*` file at the root of each local checkout |
| `windowCommand` | `prompt.append` | adds the task's text to the prompt box |
| `windowCommand` | `agent.show` | opens an agent session |
| `windowCommand` | `issue.show` | opens an issue in the repository view |
| `windowCommand` | `task.cleanupWork` | cleans this desktop's work items of a deleted task |
| `windowCommand` | `task.attachWorktree` | picks a linked worktree and attaches it |
| `windowRequest` | `task.workPlan` | inspects this desktop's work items before a delete |
| `windowRequest` | `task.startAgent` | starts an agent on the task with the prompt box's settings |
| `windowRequest` | `task.fileAsIssue` | confirms, then files the task as an issue |
| `accountPicture` | | a member's profile picture |

The `task.*` queries answer only a package whose manifest grants
`organization.tasks.read`, and the `task.*` commands and requests only one
granted `organization.tasks.write` (`nativePackageHost()` in
`MainWindowMarket.cpp`); `task.composeFromText` stays open because Notes
asks for it.

The window asks the page through host commands: `task.open` (task links in
transcripts, agents and teammates), `task.created` (quick add and the
issue-to-task move show the created task), and `task.composeFromText` (a note
turned into a task; the window computes the title and details). Agent changes,
avatar arrivals and theme changes reach every package page as
`agentSessionsChanged()`, `accountPicturesChanged()` and `themeChanged()`.

## Layout

- **Toolbar.** New task, Refresh (captioned with the list's age), Delete closed,
  View, sort, then icon tabs **Mine / Open / Closed / All** captioned with
  counts (the word is the accessible name). The page opens on Open; opening a
  task by id switches to All if the current tab or person filter would hide it
  (`revealOrganizationTaskStatus()`). Status messages sit at the right end of
  the row.
- **People strip** (`organizationTaskPeople`): Everyone, each organization
  member (you first), Bot and Unassigned, each with an open-task count. Clicking
  a face filters to that person.
- **Search.** The top search box filters the list while the page is visible
  (`setHostSearchQuery()`), matching title, details, assignee, repository,
  department, status, how-to-test and id, and lists matching tasks in its
  dropdown from any page (`searchHits()`, from the store when the page has not
  been built).
- **Rows** (`OrganizationTaskRowDelegate`). Full view: a progress rail, the
  creator's 40 px picture with an assignee or agent badge, priority and title,
  a line of field columns (status icon, department, repository, branch, QA,
  age), and up to three attachment thumbnails taken from
  `attachments[].thumbnail` (no extra requests). Compact view: one line with
  the assignee's 24 px picture on the left (the agent's model avatar for a
  task with an agent, a dashed placeholder when unassigned), the title, then
  the branch, the status icon and the age. The age is the bare short form
  (`19m`, `taskCreatedAge()`), without a clock or "ago"; status is an icon
  only, its word lives in the hover card. The branch is the first work item,
  else the branch of the local agent session linked to the task. Zen view:
  title only. The view is stored in
  `tasks/listView` (`compact`, `zen`, `full`) via the shared
  `desktop/src/ListViewMode.{h,cpp}`, written only when it changes.
- **Continuous scroll.** The catalog is loaded whole; rows are added 100 at a
  time (`kOrganizationTaskPageSize`) when the view nears the end
  (`appendOrganizationTaskRows()`). Filters reset to the top; refreshes keep
  the scroll position.
- **People pictures** come from the window's avatars (`accountPicture`)
  through `taskPersonFace()` (real picture, else a seeded stand-in), refreshed
  by `accountPicturesChanged()` when avatars arrive. No network call while
  painting.
- **Agent badge and spinner.** `organizationTaskAgentFace()` picks the newest
  local session linked to the task (`task.agentSessions`, re-read on each
  render and spinner tick), else the relay's `agent` object on a Bot task, and
  draws its model avatar (`agentFaceIcon()`). While that agent runs, a fireball
  orbits the badge.
- **Detail pane.** Icon action buttons (Edit, Start/Stop, Complete or Mark
  undone, Assign Bot, Start agent, Add to prompt, Send to QA, Follow up, Move
  to issue, Comment, Check in, Delete) above the text, a quick comment box
  under the text, and a fixed 300 px right column.
- **Right column.** An editable form (`organizationTaskProperties`): Status,
  Priority, Progress, Assignee, Department, Team, Repository and Follows task.
  A change saves at once with the same optimistic PATCH as Edit
  (`commitOrganizationTaskProperty()` → `patchOrganizationTask()`); Status
  posts start, stop, complete or undo instead. Spin boxes use keyboard
  tracking off, so typing saves on Enter or focus loss and each arrow click
  saves, with no debounce timer (the no-timer rule below). A field the relay
  would refuse is disabled with the reason as its tooltip. The editors are drawn
  borderless until hovered or focused (a stylesheet on
  `organizationTaskProperties`, so it holds in both the Classic and Next
  designs), so the column reads as a list. A text field that
  has focus is not overwritten by a refresh, and switching tasks saves any
  edit still in a text field. Read-only facts (destination, creator, agent
  session, QA, last reminder, work items, id) follow underneath.
- **From an issue.** The issue view's **Move to task** goes the other way
  ([issue-to-task](../app/issue-to-task.md)).

## The no-timer rule

Nothing on the page runs on a timer (`test_nothing_in_the_tasks_page_runs_on_a_timer`),
with one exception: the agent spinner (`updateOrganizationTaskSpinTimer()`,
`tickOrganizationTaskSpinners()`) runs only while the list is visible, a row
has a working agent, and the view is not Zen. Each tick re-checks only spinning
rows, stops itself when none remain, and never sends a request. Updates come
from events (task pushes, agent status changes, page show, Refresh).

## Markdown in the detail pane

Details, How to test and Completion note render through
`forkmesh::taskmarkdown::toHtml` (`QTextDocument::setMarkdown` with
`MarkdownDialectGitHub | MarkdownNoHTML`), so raw HTML is literal text.

- Links keep an anchor only if `isSafeLink()` accepts them (`http`/`https` with
  a host, or `mailto`); internal schemes and `javascript:`/`file:`/`data:`
  become plain text.
- Images are never loaded; they render as `[image: <url>]`.
- Task-list boxes in Details and How to test are clickable where Edit is
  allowed. `TaskCheckboxClickFilter` hit-tests via `taskItemIndexAt()`;
  `toggleTaskItem()` flips the n-th box in the source (skipping fenced code)
  and refuses if its count disagrees with Qt's parse. The save is the same
  optimistic PATCH as Edit (`settleOrganizationTaskEdit()`).

Test: `forkmesh-task-markdown-tests`.

## Work items and delete cleanup

Every branch and worktree the desktop makes for a task is attached to the
task's `workItems` (API: [task-progress.md](../app/task-progress.md)) and
cleaned up when the task is deleted.

### Attaching

- An agent run linked to a task attaches `{repository, branch, worktree, node}`
  when its worktree exists (`attachOrgTaskWorkItemForSession`). `node` is
  `machineNodeName()`.
- The desktop sends `PATCH /api/tasks/<id>` with only `{"attachWorkItems": [...]}`;
  the relay merges atomically. Without a session token the request is signed
  (`forkmesh-org-task-update-v1` over the body digest). Attaches per task are
  queued one at a time.
- **Attach worktree** attaches an existing linked worktree by directory.
- A 404 `task_not_found` is an expected answer: queued attaches are dropped and
  the work stays local.

### The local cache

`QSettings` key `tasks/workItemsCache` holds this node's work items per task,
written before the relay call. A task must be missing from two complete catalog
reads in a row before it counts as deleted, because the paged catalog can skip
a task edited mid-read.

### Cleanup

When a task is deleted (Delete, Delete closed, Move to issue) or seen gone
after a refresh, each local item is inspected on a worker
(`TaskWorkItems::inspect`). It is refused and left alone when:

- the worktree is not listed by `git worktree list --porcelain` for a known
  repository;
- it is the main checkout, contains it, is or contains `$HOME`, or overlaps any
  ForkMesh data, config, ssh, state, logs, cache or backup folder;
- the branch is `main`, `master`, the current or default branch, or invalid;
- the worktree is detached or on another branch than recorded;
- a branch-only item's branch is checked out somewhere or gone.

A **clean and merged** item (empty `git status --porcelain --untracked-files=all`
and branch an ancestor of the default branch) is removed with
`git worktree remove` and `git branch -d`. Anything else prompts (**Delete task
and work** / **Delete task, keep work**, or a non-modal notice for remote
deletions), is re-inspected before removal, and is backed up first:

1. A dirty worktree's full state is committed with a temporary index onto
   `refs/forkmesh/wip/tasks/<task id>/<branch>`.
2. `refs/forkmesh/trash/tasks/<task id>/<branch>` is set to that commit (or the
   branch tip) and read back.
3. Only then `git worktree remove --force` and `git branch -D`. If the backup
   fails, nothing is removed.

Every action and refusal is logged with a `Tasks:` prefix.

Restore a backup:

```
git -C <checkout> branch <name> refs/forkmesh/trash/tasks/<task id>/<branch>
git -C <checkout> worktree add .worktrees/<dir> <name>
```

A dirty-worktree backup is one extra commit (`git reset --soft HEAD~1` restores
the uncommitted changes). Backups are local refs, never pushed.

Test: `forkmesh-task-work-items-tests` (temp repos, sandboxed `HOME`/XDG dirs,
`FORKMESH_HOME`/`FORKMESH_PROJECTS_DIR` unset).

### Delete closed

Sends every closed task, reviewed ones first, retrying backoffs. Tasks the
relay refuses (`task_delete_blocked`, e.g. awaiting QA) are set aside; an
organization owner is then offered a forced move to the trash. The result is
shown as a notice and the list re-renders. Trash semantics:
[../app/task-trash.md](../app/task-trash.md).

## Agents and tasks

- An agent without a task gets **Add task** (rail button and list menu):
  **Create task from this agent** (owned: follows the agent's status, closes
  with the run, deleted with the agent) or **Attach to an existing task...**
  (sets `orgTaskId` and `orgTaskAttached`; not closed when the run finishes;
  **Detach from task** drops the link).
- Deleting an agent trashes its task; if the relay refuses with
  `agent_running`, the desktop first reports the run `stopped`
  (`POST /api/tasks/<id>/agent-status`).
- Runs without a task are still reported in the signed agent-status batch
  (provider, model, status, start time only) so the web office can show them.

## Comments and activity

Comments are written in the quick comment box under the detail text
(`organizationTaskQuickComment`); Ctrl+Enter or Comment posts it, and the
Comment action button focuses it. An unposted draft is kept per task while
you move through the list, and a failed post puts the text back.
A posted comment appears at once ("Sending…") and is replaced by the reply's
`task.history` and `task.replies`; a failure restores the previous timeline.
Activity shows full comment bodies and read receipts (`readBy`) recorded by the
relay when a member opens the task. A cached timeline older than 30 s
(`kOrganizationTaskHistoryStaleMs`) is re-read in the background.

## Key-signed task writes

A desktop holding only its account key signs task requests instead of using a
session token. Proofs bind the task and, for non-empty bodies, the body digest:
`forkmesh-org-task-update-v1`, `forkmesh-org-task-remind-v1`,
`forkmesh-org-task-checkin-v1`, `forkmesh-org-task-reply-v1`,
`forkmesh-org-task-history-v1`, `forkmesh-org-task-agent-status-v1`. Timers and
QA verdicts still need a session token. Tests:
`app/tests/test_org_task_signed_writes.py`.
