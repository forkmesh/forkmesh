# Desktop: the Tasks page

Part of [the desktop docs](README.md). Code: `desktop/src/MainWindowTasks.cpp`
(`buildOrganizationTasksSection()`, `OrganizationTaskRowDelegate`,
`taskDetailHtml()` and `taskFieldsHtml()`), and for work items
`desktop/src/MainWindowTaskWorkItems.cpp` with the testable rules in
`desktop/src/TaskWorkItems.{h,cpp}`.

## Layout (2026-09-25 visual pass)

The page is one toolbar, the task list on the left and the selected task on
the right.

- **One toolbar row.** New task, Refresh, Delete closed, sort, Mine, then the
  Open / Closed / All tabs, all left-aligned. Below it is the people strip
  (see "Icon tabs with counts and the people strip" below). The page opens on
  the Open tab (see "Open is the default tab" below). The divider under the
  toolbar and the separate status line are gone. Action messages ("Task
  created.", errors) and the open/closed summary sit at the right end of the
  same row.
- **Refresh shows the age of the list.** Its caption reads "just now", "5m ago"
  and so on, and its tooltip gives the exact time. The old "Private catalog
  refreshed at HH:MM:SS" text is gone. The caption is recomputed when the page
  is shown, when a row is selected and after every read, not on a timer: the
  page keeps its no-timer rule (`test_nothing_in_the_tasks_page_runs_on_a_timer`).
  The one exception is the working-agent spinner (see "Agent avatar and
  spinner" below), which only runs while it has something to draw.
- **No page search box.** The top search box filters the list while the
  Tasks page is visible (`syncOrganizationTaskPageSearch()`, the same pattern the
  Team page uses). It matches title, details, assignee, repository, department,
  status, how-to-test and task ID. The dropdown's task hits match the same
  fields, so a task found from another page is found here too. Opening a
  created or linked task clears the filter so the task cannot be hidden.
- **Two-line rows, drawn like the issues list.** A thin progress rail on the
  left edge fills bottom-up with the task's `progress` (0-100). Next to it is
  the creator's profile picture (a generated stand-in when the account has
  none, see "Real profile pictures" below) with the assignee as a small badge on its
  bottom-right corner (the agent's own avatar for an agent task, see below; a
  sparkle for a Bot task no agent has picked up yet; no badge when unassigned
  or self-assigned). The title is on the first line, after the priority number.
  Under it, each field is its icon plus a label in a fixed-width column, so the
  columns line up from row to row: status, department, repository, attached
  branch (the first work item, "+N" when there are more), QA state (only once
  QA has said something), then how long ago the task was created. An empty
  field keeps its column. Labels are cut short to fit their column. The hover
  card still spells every field out.
- **Attached images on the row (2026-09-27).** A task with images shows up
  to three 34 px square previews at the right end of its row, spanning both
  lines. The title and field columns get shorter to make room. A fourth image
  is shown as "+1" over the third preview. The previews are the 48 px
  thumbnails the task list response already carries (`attachments[].thumbnail`),
  so the list makes no extra image requests. An attachment without a thumbnail
  (GIFs, and tasks created on the web before 2026-09-27) is a paperclip tile.
  Once the detail pane has downloaded that image, the row uses it instead
  (`refreshOrganizationTaskRowThumbnails`). Row data: `kTaskThumbnailsRole`
  and `kTaskAttachmentCountRole`. Test:
  `test_qt_task_rows_paint_attached_image_thumbnails` in
  `app/tests/test_task_pagination.py`.
- **Actions as icon buttons above the task.** The detail pane's actions are
  icon-over-caption buttons in a wrapping row, like the page toolbar: Edit,
  Start/Stop, Complete or Mark undone, Assign Bot, Start agent (green; the
  "start agent with prompt settings" launch), Add to prompt, Send to QA,
  Return to list, Follow up, Move to issue, Comment, Check in, Delete (red).
  Complete and Move to issue show only on open tasks and Mark undone only on
  closed ones. Before this change, Move to issue and Mark undone were stacked
  in the same grid cell and drew over each other.
- **Fields in a right-hand column.** Status, priority, department, team,
  destination, repository, assignee, creator, follows-task, agent session, QA
  fields, updated and task ID are in a fixed 300 px column next to the main
  text. The main text is the title, details, how to test, completion note,
  attachments and activity. Long values such as the task ID wrap anywhere
  rather than widen the column.
- **Pager text.** The old "1–60 of 60 · page 1 of 1" label was built with
  `QStringLiteral` over UTF-8 escapes and printed "1â60 of 60 Â·". Labels
  with non-ASCII text are built with `QString::fromUtf8`. The pager itself is
  gone; see the next section.

## Work items and delete cleanup (2026-09-25)

Every branch and worktree the desktop makes for an organization task is
attached to the task's `workItems` (API: [task-progress.md](../app/task-progress.md#work-items-branches-and-worktrees-2026-09-25)).
Deleting the task deletes them, with the safety rules below.

### Attaching

- An agent run linked to a task gets its worktree under
  `<checkout>/.worktrees/<id>-<slug>` (or `agent-<id>-<slug>`), from
  `AgentRunner` (signal `worktreeReady`) or from the Claude Code / Codex
  transcript path (`startCliTranscriptWorktree`). When the worktree exists and
  the session has its task id (`recordOrgTaskFields` runs when the task-open
  reply lands, which may be later), `attachOrgTaskWorkItemForSession` attaches
  `{repository: owner/name, branch, worktree: absolute path, node}`.
- `node` is this machine's node name (`machineNodeName()`), or the account
  name when that is not a valid node name.
- The desktop sends `PATCH /api/tasks/<id>` with only
  `{"attachWorkItems": [...]}`, the queued items, and the relay merges them
  into the task's list atomically. Until 2026-09-27 it read the list (the
  loaded board or a catalog read), merged locally and sent the whole
  `workItems`, so two nodes attaching at once could drop one item; see
  [task-progress.md](../app/task-progress.md#work-items-branches-and-worktrees-2026-09-25).
  With a session token it sends
  the token; otherwise it signs `forkmesh-org-task-update-v1` over the body
  digest (`organizationTaskProof` now answers PATCH; priority-only and
  progress-only bodies still get no proof, as before). Attaches to one task are
  queued, one request at a time.
- **Attach worktree** in the detail pane attaches an existing worktree: pick
  its directory, and it must be a linked (not main) worktree of a repository
  this desktop has. Its checked-out branch is recorded. Enabled for managers,
  the assignee and the creator, the same people the relay lets edit the list.
- The detail pane's field column shows **Work items** as `branch @ node`.
- Every attach is logged (`Tasks: attached ...`), and so is a failure.
- **Attaching to a deleted task is an answer, not a failure (2026-10-02).** An
  agent whose task was trashed on the board still carries its `orgTaskId`, so
  its first attach PATCH got 404 `{"error":"task_not_found"}`. That logged
  `net CUSTOM ERR 404`, `Organization tasks: PATCH ... failed` and `Tasks:
  attaching ... failed`, and each ERR line raised a ping. Now
  `requestOrganizationTasks` takes `expectedHttpStatuses`: it marks them on the
  request (`markExpectedHttpStatus`, so the net log reads `EXPECTED 404`) and
  leaves the failure line to the caller. The attach passes `{404}`, drops the
  task's queued attaches on `task_not_found` and logs `Tasks: task ... is no
  longer on the board; ... stays local.` Deleting a task (the Tasks page and an
  agent's attached task) passes `{404}` too, since both already treat
  `task_not_found` as done. Test:
  `test_desktop_attach_to_deleted_task_is_expected` in
  `app/tests/test_org_task_signed_writes.py`.

### The local cache

`QSettings` key `tasks/workItemsCache` holds, per task id, the work items whose
`node` is this desktop, plus how many complete board reads in a row have not
listed the task. Every complete catalog read (`hasMore` false on the last page)
refreshes it (`reconcileOrganizationTaskWorkItems`). An attach is written to the
cache before the relay write, so a task deleted on the website before the write
lands is still cleaned.

A task has to be missing from **two** complete reads in a row before it counts
as deleted. The catalog is paged by `updated_at`, so a task edited while the
pages are read can slip between two pages once; a real deletion is missing
every time.

### Cleanup

Delete and Delete closed move tasks to the relay's 30-day trash
([../app/task-trash.md](../app/task-trash.md)); the relay refuses closed
tasks still awaiting QA, and Delete closed offers an owner to move those anyway
([below](#delete-closed-finishes-the-run-and-says-what-it-did-2026-10-09)). When a task is deleted from
the Tasks tab (Delete, Delete closed, Move to issue), or is noticed as gone on
a refresh (trashed elsewhere), the desktop takes the task's items
whose `node` is this desktop (from the task, the DELETE response and the
cache) and checks each one on a worker thread (`TaskWorkItems::inspect`).

Refused, left alone and logged:

- The worktree is not listed by `git -C <repo> worktree list --porcelain` for a
  repository this desktop knows (a matching `owner/name` first, then every
  local repository). A directory that exists but is not listed is never
  touched.
- The worktree is the main checkout (the first porcelain entry, or a bare
  entry), contains the main checkout, is `$HOME` or contains it, or is inside
  (or contains) a ForkMesh data, config, ssh, state, logs or cache dir: the
  layout's roots, `~/projects/forkmesh/{data,config,ssh,state,logs}`,
  `~/.config/ForkMesh`, `~/.local/share/ForkMesh`, `~/.forkmesh`, `~/.ssh` and
  `~/projects/forkmesh-backup`.
- The branch is `main`, `master`, the main checkout's current branch, the
  default branch (`origin/HEAD`, else `main`, else `master`) or not a valid
  branch name.
- The worktree is detached or on a different branch than recorded.
- A branch-only item (or one whose worktree directory is already gone) whose
  branch is checked out in some worktree, or that no longer exists.

Otherwise the item is **clean and merged** when `git status --porcelain
--untracked-files=all` is empty and `git merge-base --is-ancestor` finds the
branch in the local default branch or `origin/<default>`. Those are removed
with no prompt: `git worktree remove <path>`, then `git branch -d <branch>`.
If `git branch -d` refuses (the main checkout is on another branch and git
disagrees about "merged"), the branch is kept and logged.

Anything else holds work that would be lost:

- Deleting in the desktop: the Delete confirmation lists the dirty files and
  unmerged commits (up to 8 of each) and offers **Delete task and work**,
  **Delete task, keep work** and Cancel. A task with no local work items, or
  only clean merged ones, is deleted without a prompt, as before.
- Noticed after a website deletion (or after Delete closed / Move to issue): a
  non-modal notice with **Delete** and **Keep**. Keep leaves everything and
  drops the task from the cache.
- Before anything is removed the item is inspected again, so a worktree that
  changed while the notice was open is left alone.

Deleting unmerged work first writes a backup:

1. If the worktree is dirty, a WIP commit of its whole state (tracked edits and
   untracked files, not ignored files) is built with a temporary index
   (`read-tree HEAD`, `add -A`, `write-tree`, `commit-tree -p HEAD`) and stored
   on `refs/forkmesh/wip/tasks/<task id>/<branch>`. The worktree's own index
   and branch are not touched.
2. `refs/forkmesh/trash/tasks/<task id>/<branch>` is set to that commit, or to
   the branch tip when the worktree was clean, and read back. The temporary
   WIP ref is deleted.
3. Only then `git worktree remove --force <path>` and `git branch -D <branch>`.
   If writing the backup fails, nothing is removed.

Every action and refusal is written to the app log (`logSystem`, lines starting
`Tasks:`), and a notice summarises the count.

### Restoring a backup

```
git -C <checkout> branch <name> refs/forkmesh/trash/tasks/<task id>/<branch>
git -C <checkout> worktree add .worktrees/<dir> <name>
```

A backup from a dirty worktree is one commit on top of the branch tip: `git
reset --soft HEAD~1` in the restored worktree turns it back into uncommitted
changes. List backups with `git for-each-ref refs/forkmesh/trash/`, and delete
one with `git update-ref -d <ref>` once it is not needed. Backups are local
refs; they are not pushed.

### Tests

`forkmesh-task-work-items-tests` (`desktop/tests/test_task_work_items.cpp`,
run with `QT_QPA_PLATFORM=offscreen`) uses temp repos only, with `HOME` and the
XDG dirs pointed at a temp dir and `FORKMESH_HOME` / `FORKMESH_PROJECTS_DIR`
unset. It covers the digest vector shared with the relay test, branch
validation, merge and de-duplication, porcelain parsing, every path and branch
refusal, clean-and-merged removal, backup-then-force removal of a dirty,
unmerged worktree (the backup holds the edits and the untracked file, and
restores with `git branch`), and branch-only items.

## Agents without a task in the office (2026-10-04)

A run with no task is still reported to the relay so the web office can draw
it beside its owner: `syncOrgTaskAgentStatus` and `flushOrgTaskAgentStatus`
include it in the same signed agent-status batch with an empty `task`, while
it is live and for 8 hours after it finishes (`agentPresenceWanted`), never
once deleted. Only provider, model, bot, strength, session number, status
and start time are sent. A refused presence-only batch pauses these reports
for 30 minutes. The relay side is in `docs/office/README.md`, "Agents
without a task".

## Creating or attaching a task from an agent (2026-09-27)

Until now an agent only got an organization task when it was started with one
(the prompt's task option, or Start agent on the Tasks page). An agent started
without one can now get a task afterwards. Code:
`desktop/src/MainWindowAgentTaskLink.cpp`.

- **Where.** The agent detail's rail button that opens a linked task now also
  shows on an agent with no task, captioned **Add task**
  (`syncAgentViewTaskButton()`). It opens a menu with **Create task from this
  agent** and **Attach to an existing task...**. The Agents list's right-click
  menu has the same two actions. External Claude Code sessions and cloud rows
  do not get them: their session numbers do not name a local session the relay
  could link.
- **Create task.** A small dialog asks for the title (the issue title, else the
  prompt's first line, else "Agent session #N"). The request is the one an
  agent started with a task sends (`postOrgTaskForSession()`, shared with
  `openOrgTaskForSession()`): the prompt as details, assigned to the agent's
  kind, with the run's provider, model and status. The agent owns this task,
  as if it had been started with it: it follows the agent's status, closes when
  the run finishes, and is deleted with the agent. When the run has already
  finished, the dialog has **Close the task with this run's summary** (on by
  default); unticked, the task stays open. A failure is shown as a notice, not
  only logged as on the automatic path.
- **Attach to an existing task.** A picker lists the open tasks (the loaded
  board when it is under 30 seconds old, otherwise a catalog read), tasks on
  the agent's repository first, with a filter over title, details, repository,
  assignee, status, department and id. Attaching sets the session's
  `orgTaskId` and a new `orgTaskAttached` flag, attaches the agent's branch and
  worktree to the task's work items (the worktree is looked up with
  `cachedSessionWorktree()` when this run of the desktop did not create it),
  reports the agent's status (the relay records it only on agent-assigned
  tasks), and puts the agent's avatar on the task's row.
- **An attached task is not the agent's.** The task was somebody's before the
  agent was attached, so:
  - it is not closed when the run finishes (`completeOrgTaskForSession()`
    skips attached links), unless **Close the task with this run's summary**
    was ticked when attaching an already finished run;
  - deleting the agent does delete it (moves it to the trash, see
    "Deleting an agent deletes its task" below), and the delete confirmation
    lists it;
  - **Detach from task** (right-click in the Agents list) drops the link. The
    task stays, and so do the work items already on it. The agent can then be
    linked again.
- **Persistence.** `orgTaskAttached` is saved with the session only when it is
  true, so existing session files load unchanged as owned links.

### Tests

`forkmesh-agent-task-link-window-tests` (`forkmesh-window-tests
--agent-task-link-only`, `checkAgentTaskLink()` in
`desktop/tests/test_window_resize.cpp`) checks that an unlinked agent offers
both actions in the rail button and the list menu, that an owned link offers
none, that external and unknown sessions cannot be linked, that a malformed id
is refused, that attaching sets the link and the attached flag, and that the
flag round-trips through the session JSON, that the attached agent's avatar
shows on a person's task, and that Detach unlinks it.

## Deleting an agent deletes its task (2026-09-30)

Issue #876: deleting an agent failed with "Could not delete attached task …;
agent session was kept: task delete blocked". The relay refuses to trash a task
whose run looks live (`task_delete_blocked`, `blockedBy: ["agent_running"]`,
see `docs/app/task-trash.md`), and the agent being deleted is that run.

`deleteAgentSessionOrgTask()` (the busy-guard retry from issue #874, see
`docs/app/task-trash.md`) gained two steps:

- When the refusal names `agent_running`, it reports the run `stopped` with
  `POST /api/tasks/<id>/agent-status` (key-signed with
  `forkmesh-org-task-agent-status-v1` when there is no session token) before
  the forced DELETE. With the run no longer live nothing blocks the delete, so
  it succeeds for a member who is not an organization owner too.
- Attached tasks are deleted with the agent. The confirmation lists them, and
  the attached flag is cleared with the link. The user asked for Delete on an
  agent to take its task with it; a trashed task can be restored for 30 days.

## Continuous scroll (2026-09-27)

The list no longer has Previous / Next buttons. It shows the first 100
matching tasks (`kOrganizationTaskPageSize`), and when the last visible row is
within ten rows of the end (`kOrganizationTaskLoadAheadRows`), the next 100 are
added below (`appendOrganizationTaskRows()`). The whole catalog is already
loaded, so this only adds rows to the table. It makes no network request.

- The label under the list reads "100 of 450 · scroll for more" until every
  match is shown, then "450 tasks".
- A search, filter, sort, or status tab change goes back to the first 100 rows
  at the top (`m_organizationTaskShownRows = 0`).
- A refresh or a task edit keeps the rows already loaded and the scroll
  position, so the list does not jump back to the top.
- Opening a task by id (a link, a created task, the search dropdown) loads
  enough batches to include that task and selects it. The old code worked out
  the page from the task's position in the unsorted catalog, so it could land
  on the wrong page.

## Request logging (2026-09-27)

`requestOrganizationTasks()` used to write two INFO lines per call ("GET
/api/tasks?offset=75 requested." then "... completed."). A full catalog read
pages 25 at a time, so one refresh filled the log with dozens of these lines.
The WEB log already records each request, so these lines were removed. A failed
request still logs `Organization tasks: <method> <path> failed: <error>.`

## Agent avatar and spinner on task rows (2026-09-27)

A task an agent works on now shows which agent it is, and whether it is
working right now, without opening the task.

- **The agent's avatar in the assignee badge.** When a task has an agent, the
  badge on the creator's identicon is the agent's avatar, drawn a little larger
  than a person badge (16 px instead of 13) so the artwork reads. It is the
  same avatar the Agents list draws for that run: `agentStatusModelIcon()`, the
  provider/model face (or the Auto face for Claude Auto).
  `MainWindow::organizationTaskAgentFace()` picks the agent:
  1. the newest local agent session linked to the task (`orgTaskId`, the same
     one `localAgentSessionIdForTask()` shows in the fields column), whatever
     the task's `assigneeKind` is, so a person's task they started an agent on
     shows that agent;
  2. otherwise, for a task assigned to the Bot, the `agent` object the relay
     keeps on the task (`provider`, `model`, `status`), for a run on another
     desktop.
  A Bot task with neither still shows the sparkle: there is no agent yet whose
  face could be drawn.
- **A spinner while the agent works.** While that agent's status is `running`
  (and it is not merged, and the task is not closed) the purple fireball the
  Agents list uses for a running run (`agentRunningFireballPixmap()`) orbits
  the badge, the same way the Issues list shows a working agent on an issue.
  A queued, waiting, finished, failed or stopped agent shows only its avatar.
- **When it updates.** Rows are resolved when they are drawn, when a local
  agent linked to a task changes status (`updateAgentStatusCell()`), when the
  agent list reloads, and when the page is shown. A run on another desktop
  updates when the task list does (a pushed task event or Refresh); the page
  still never polls the relay for it.

### The spinner timer and the no-timer rule

The page's rule is that nothing on it runs on a timer. The spinner needs one
to move, so the rule now allows exactly one, and only while it has something
to draw (`updateOrganizationTaskSpinTimer()`, `tickOrganizationTaskSpinners()`):

- it is started only when the task list is visible and at least one row on it
  has a working agent;
- each tick (110 ms, 20 degrees, the Issues list's cadence) re-checks only the
  rows that were spinning. A row whose agent has stopped stops spinning on that
  tick, and when none are left the timer stops;
- when the list is hidden (another page is shown) the next tick stops the
  timer instead of idling on a slow tick the way the Agents list does. Showing
  the page again starts it again if an agent is still working;
- it never reads the catalog or sends a request.

`test_nothing_in_the_tasks_page_runs_on_a_timer` now checks this shape: one
`new QTimer` in `MainWindowTasks.cpp`, no `QTimer::singleShot`, the start
guarded by visibility and a working agent, and a tick that stops itself and
makes no requests.

### Fix: row roles lost in the continuous-scroll change

The continuous-scroll change moved row filling into
`appendOrganizationTaskRows()` and left out the progress, creator and branch
roles, so rows had lost their progress rail fill, creator identicon and branch
column. They are set again there.

### Tests

`forkmesh-task-agent-avatar-window-tests` (`forkmesh-window-tests
--task-agent-avatar-only`, `checkTaskAgentAvatar()` in
`desktop/tests/test_window_resize.cpp`, run with `QT_QPA_PLATFORM=offscreen`)
loads three tasks into the real window: one linked to a running local agent
session, one assigned to the Bot with a finished run reported by the relay, and
one assigned to a person. It checks that the first two rows carry exactly the
`agentStatusModelIcon()` pixmap for their agent and the person's row carries
none; that only the running agent's row spins and the timer runs; that a tick
moves the fireball; that when the agent finishes the next tick stops the row
and the timer, and the painted row loses its purple fireball but keeps the
avatar; that the spinner comes back when the agent runs again; and that the
timer stops while the page is hidden and resumes when it is shown.

## Markdown in the detail pane (2026-09-28)

The detail pane renders a task's Details, How to test and Completion note as
Markdown: headings, bold and italic, inline code and fenced code blocks, lists,
GitHub tables, links and blockquotes. The text arrives with its line breaks
since the tasks API stopped collapsing them ([app/task-markdown.md](../app/task-markdown.md)).

`desktop/src/TaskMarkdown.{h,cpp}` (`forkmesh::taskmarkdown::toHtml`) feeds the
text to `QTextDocument::setMarkdown` with `MarkdownDialectGitHub |
MarkdownNoHTML`, so raw HTML in a task (`<script>`, `<img onerror>`) is shown as
literal text rather than parsed. Before `toHtml()` it rewrites the document:

- A link keeps its anchor only when `isSafeLink()` accepts it: `http:` or
  `https:` with a host, or `mailto:`. Anything else (`javascript:`, `file:`,
  `data:`, relative paths, and the pane's own `forkmesh-agent:` and
  `forkmesh-task-image:` schemes, which the pane's click handler acts on)
  becomes plain text, so task text can never open an agent or an image dialog.
- Images are never loaded: `QTextBrowser` resolves an image name against local
  files, so a Markdown image becomes the italic text `[image: <url>]`, linked
  only when the URL is a safe link.
- The importer's fixed blue link colour is dropped so links take the theme's
  link colour.

Only the `<body>` contents are returned and placed inside the pane's own HTML.
The pane's `anchorClicked` handler now opens safe links with
`QDesktopServices::openUrl`; before, it ignored every link that was not an
agent or task-image link.

### Tests

`forkmesh-task-markdown-tests` (`desktop/tests/test_task_markdown.cpp`, runs
with `QT_QPA_PLATFORM=offscreen`) checks headings, tables (right-aligned column
included), ordered and bullet lists, bold, italic, inline and fenced code
(escaped), safe links, and that `javascript:`, internal schemes, `file:` links,
raw `<script>`/`<img>` and Markdown images never reach the HTML as live markup.

## Ticking checklist boxes in the detail pane (2026-10-07)

A GitHub task-list item in Details or How to test (`- [ ] build`, `1. [x] test`,
`> - [ ] quoted`) is a clickable box in the detail pane. Clicking it ticks or
clears that item and saves the task with the same optimistic PATCH as **Edit**
(`/api/tasks/<id>` with only `details` or `howToTest`); a failed save puts the
old text back. Boxes are clickable only where Edit is: the user can manage
tasks and the task is neither active nor closed, because the server answers
`active_task_cannot_be_updated` / `completed_task_cannot_be_updated` otherwise.
Completion-note boxes stay read-only (that text changes only through Complete).
The cursor becomes a pointing hand over a box that can be clicked.

How it works:

- `TaskCheckboxClickFilter` (`MainWindowTasks.cpp`) watches the detail
  browser's viewport. On a left press it maps the point to document
  coordinates (adding the scroll offsets) and asks
  `taskmarkdown::taskItemIndexAt()`, which uses
  `QAbstractTextDocumentLayout::blockWithMarkerAt()` and returns the box's
  position among all boxes in the pane. Press and release over a toggleable box
  are consumed, so the click neither selects text nor follows a link; the
  toggle fires on release over the same box.
- `taskCheckboxToggle()` turns that pane-wide index into a field: the first
  `taskItemCount(details)` boxes belong to Details (counted on the text without
  attachment lines, as rendered), the next ones to How to test.
- `taskmarkdown::toggleTaskItem()` flips the n-th `[ ]`/`[x]` in the source
  text, skipping fenced code blocks. It refuses (returns a null string) when
  its own line count disagrees with Qt's parse (`taskItemCount()`), e.g. an
  indented code block that looks like a list item, so a click can never edit
  the wrong line.
- Qt 6.8's Markdown importer copies a ticked item's marker onto a following
  non-list block (a fenced code block after `- [x] test` reports
  `MarkerType::Checked`). A block counts as a box only when it is in a list.
- The PATCH reply handling shared with Edit lives in
  `settleOrganizationTaskEdit()`.

`forkmesh-task-markdown-tests` covers toggling (ticking, clearing, numbered and
quoted items, fences, refused indexes and indented code) and hit-testing boxes
in the exported HTML laid out in a `QTextDocument`.

## List view: Compact, Zen, Full (2026-09-30)

The Tasks toolbar has the same **View** button as the Agents list
([agents-ui.md](agents-ui.md#list-view-compact-zen-full-2026-09-29)). It sits
between Delete closed and the sort button, names the current view, and steps
to the next one on each click: Compact, then Zen, then Full, then Compact. The
icons, captions, tooltip wording ("Zen view: ... Click for Full view") and the
accessible name ("Task list view: Zen") come from the shared helper
`desktop/src/ListViewMode.{h,cpp}`, so the two pages cannot drift apart. The
button is a `VerticalIconButton` like the rest of the Tasks toolbar (the
Agents toolbar is built from `ActivityRailButton`s); both draw the icon over a
10 px caption, and Space or Enter clicks it like any push button. Its width is
fixed to fit "Compact", the longest caption, so the buttons beside it do not
move when the view changes.

- **Full** is the two-line row described under "Layout" above: progress rail,
  creator identicon with assignee or agent badge, priority and title, the
  field columns underneath, image previews on the right. It is the default,
  so the page looks as it did until the button is clicked.
- **Compact** is one 32 px line per task: the progress rail, the priority
  number, the title, then right-aligned columns for status, assignee (the
  agent's avatar, with its spinning fireball while it works, for an agent
  task; empty when unassigned) and age, and a paperclip when the task has
  attachments. Columns are dropped from the left (status first) when the list
  is too narrow to leave the title 120 px.
- **Zen** is one 32 px line with nothing but the title (struck through when
  done). The hover card still lists every field and the detail pane shows the
  selected task. Zen draws no spinner, so the spinner timer does not run in
  Zen (`updateOrganizationTaskSpinTimer` checks the view).

The choice is kept in the `tasks/listView` setting (`compact`, `zen` or
`full`), separate from the Agents list's `agents/listView`. It is written
only when it changes: `storeListViewMode` goes through
`forkmesh::settings::put`, because every `QSettings` write rewrites the whole
settings file ([diagnostics.md](diagnostics.md)). Before this change the Agents
list wrote its key on every start.

The delegate (`OrganizationTaskRowDelegate`) holds the view and paints
`paintZen`, `paintCompact` or the Full row; `setOrganizationTaskListView`
sets it, resizes every row (`taskRowHeight`) and repaints. The row data is the
same in every view, so switching does not rebuild the list.

### Tests

- `forkmesh-list-view-mode-tests` (`desktop/tests/test_list_view_mode.cpp`)
  checks the cycle order, keys, icons, captions, tooltip and accessible-name
  wording, the fallback for an unknown or missing key, that storing the same
  view twice writes once, and that the Tasks and Agents keys are separate.
- `forkmesh-task-list-view-window-tests`
  (`forkmesh-window-tests --task-list-view-only`) loads three tasks, steps the
  button through Full, Compact, Zen and back, and checks the caption, tooltip,
  accessible name, stored key, row height, that the progress rail is drawn in
  Full and Compact but not Zen, that the spinner timer stops in Zen, and that
  the stored view is what the page opens with. Set
  `FORKMESH_TASK_ROWS_SHOT=/path/prefix` to save the window and one row in
  each view.

## Real profile pictures for creators and assignees (2026-09-30)

Rows for tasks created by `jett` (and every other person) showed the seeded
stand-in portrait instead of the picture the account uses in Chat, the Agents
list and on the web.

### Cause

The row delegate drew the creator avatar, the assignee badge and the Compact
assignee column with `taskIdenticon()`, which only renders
`forkMeshAvatarPng(seed)`. It never looked at the real pictures. Those live in
`MainWindow::m_avatars` under `user:<account>`, filled from the `avatarPng`
field of the user directory (`/api/accounts/users`, which on app.forkmesh.com
carries both jett's and mdkaifansari04's pictures) and from the avatar disk
cache. The key was not the problem: `createdBy` and `assignee` are account
names, the same key the directory uses. The page also never asked for the
directory, and nothing repainted its rows when pictures arrived.

### Fix

- `appendOrganizationTaskRows()` sets two more roles on each row,
  `kTaskCreatorPictureRole` and `kTaskAssigneePictureRole`, from
  `chatSenderAvatarSource(QString(), name)`, the lookup Chat uses
  (`applyOrganizationTaskPeopleFaces()`).
- The delegate draws through `taskPersonFace()`. It uses the picture when
  there is one, rounded and cached by the picture's `cacheKey()`, size and
  device pixel ratio. Otherwise it falls back to the same seeded stand-in as
  before, which is also what the issue views use for someone with no picture.
  The Full creator avatar, the Full assignee badge and the Compact assignee
  column all go through it. Zen draws no avatars.
- `refreshOrganizationTaskPeopleFaces()` runs wherever the issue views already
  swap in pictures: when the user directory brings new or changed pictures
  (`mergeChatUserDirectory`), when a peer avatar arrives (`onAvatar`) and when
  the disk cache is decoded (`loadCachedAvatars`). It only changes a row's
  role when the picture is different, and `setData` repaints that row. There is
  no network call while painting and no timer.
- Showing the Tasks page calls `refreshChatUserDirectory()`, which is behind
  its five-minute limit like the other pages that show people.

### Tests

`checkTaskPeopleAvatars()` in `desktop/tests/test_window_resize.cpp` runs
with `forkmesh-window-tests --task-agent-avatar-only`. It loads a task created
by `jett` and assigned to `mdkaifansari04`, plus one created by an account with
no picture. With no pictures loaded, jett's row shows the stand-in. After solid
green and blue pictures are put in for the two accounts
(`testSetAccountAvatar`), the Full row repaints with green as the creator
avatar and blue in the assignee badge. The row with no picture stays without
green, Compact shows the blue picture in the assignee column, and Zen shows
neither.

## Open is the default tab (2026-10-01)

Clicking Tasks shows the Open tab, not All: the list is for work still to do,
and closed tasks are one click away. `m_organizationTaskStatusFilter` starts
as `"open"` and the Open tab is the checked one.

Opening a task by id (a transcript link, `openOrganizationTask()`, or a task
just created) must not land on a tab that hides it. If the task is closed
while Open is selected, or open while Closed is selected,
`revealOrganizationTaskStatus()` switches to All first. A refresh that only
keeps the current selection does not switch tabs.

## Icon tabs with counts and the people strip (2026-10-06)

The user asked for "the tabs like icons with counts" and "avatars for org
members so we can check others tasks", the same change the web Tasks page got
([task-board-filters](../app/task-board-filters.md)).

- **Mine, Open, Closed and All** are icon tiles captioned with a count instead
  of a word. The word is the button's accessible name, and the tooltip reads
  "Open: 3 tasks". The counts follow the search box, the Mine toggle and the
  selected person. Mine counts your own tasks on the current status tab.
- **The people strip** (`organizationTaskPeople`, under the toolbar, scrolls
  sideways) replaces the "All assignees" menu. It shows Everyone, each member
  from the catalog's `members` list (you first, then A to Z), Bot and
  Unassigned. Each button shows that person's open-task count, a
  number that ignores the search and the status tab. Clicking a face shows only
  that person's tasks, and clicking it again goes back to Everyone. Picking a
  person turns Mine off, and turning Mine on picks Everyone.
- Faces come from `chatSenderAvatarSource()` through `taskPersonFace()`, the
  same real-picture-then-identicon path the task rows use, and
  `refreshOrganizationTaskPeopleFaces()` redraws them when the avatar directory
  lands. The buttons are rebuilt only when the member list changes
  (`rebuildOrganizationTaskPeopleStrip()`); a render only updates counts,
  faces and the checked button (`updateOrganizationTaskPeopleStrip()`).
- Opening a linked task (`openOrganizationTask()`) now also resets the person
  to Everyone, so a person filter cannot hide the task it was asked to show.

## Remind and check in without a password session (2026-10-06)

A desktop that signed in normally holds its account key and no session token.
**Remind** and **Check in** had no key-signed form, so both failed with "This
change needs a password sign-in on this device" for an operator who was plainly
signed in.

- The relay's `_org_task_signed_session` (`app/src/entry.py`) now accepts
  `forkmesh-org-task-remind-v1` on `POST /api/tasks/<id>/remind` and
  `forkmesh-org-task-checkin-v1` on `POST /api/tasks/<id>/checkin`. Each proof
  names the task and, when the body is not empty, the body digest
  (`org_task_update_digest`, the same one Delete uses), so a captured proof
  cannot be replayed on another task, with another note, or as the other
  action.
- `organizationTaskProof` in `MainWindowTasks.cpp` signs both. The task API
  still applies its own rules to the resolved account: a reminder needs an
  assignee other than you and respects the 10-minute cooldown; a check-in is
  for the active assignee only.
- Timers (start/stop) and QA verdicts still need a session token.
- `app/tests/test_org_task_signed_writes.py` covers both proofs and checks the
  desktop and relay proof strings agree.

## Delete closed finishes the run and says what it did (2026-10-09)

Delete closed on the Tasks toolbar looked like it did nothing:

- Completing a task sends it to QA (`completed_at` and `qa_requested_at` are
  set together), so most closed tasks wait for a QA verdict. Delete closed
  skipped them up front, and when every closed task was waiting it only wrote
  one line in the status label: no dialog, no notice, and an owner had no way
  to trash them in bulk.
- The run stopped at the first refused DELETE: a closed task whose agent
  session still looked live (`task_delete_blocked`, `agent_running`), a task
  already gone (`task_not_found`), or a relay backoff.
- Its "Moved N closed tasks to the trash." line was cleared by the refresh
  that followed, and that refresh was dropped when a catalog read was already
  in flight (the relay's `tasks` push starts one on every delete), so the
  trashed rows stayed on screen.

Now (`deleteClosedOrganizationTasks`, `deleteNextClosedOrganizationTask`,
`finishClosedOrganizationTaskDelete` in `desktop/src/MainWindowTasks.cpp`):

- Every closed task is sent, reviewed ones first. The confirmation says how
  many are still awaiting QA.
- A trashed or already-gone task leaves the list at once. A backoff waits
  and retries the same task (up to 5 times, as Delete does). A
  `task_delete_blocked` answer is set aside and the run goes on; 404 and 409
  are expected statuses, so they do not log ERR lines.
- When the run ends with tasks set aside whose answer allowed `force`
  (`canForce`, an organization owner), one dialog names the reasons and offers
  to move them to the trash anyway; Yes resends those with `force: true`.
- The result ("Moved N closed tasks to the trash. M stayed on the board (it is
  waiting for QA review)…") is shown as a notice and in the status label. The
  rows are re-rendered without the trashed tasks, and the catalog re-read
  runs after any read already in flight.
- Any other error still stops the run and shows how many were moved.

## Comments show at once, with read receipts (2026-10-09)

A comment posted from the desktop used to take a while to appear. The
desktop threw away the relay's reply, cleared the cached timeline and
re-read the whole catalog. Until a new `GET /history` came back, Activity
said "No activity recorded on this task yet". Activity only showed the
200-character event summary, and nothing said whether anyone had read
the comment.

- **Shown at once.** `commentOnOrganizationTask()` adds the comment to
  the task's Activity before the request goes out, marked "Sending…". The
  `POST /replies` reply carries `task.history` and `task.replies`, which
  replace the cache straight away. A failure puts the earlier timeline
  back. A comment no longer re-reads the catalog, because it changes no
  catalog field.
- **Full comment in Activity.** `taskHistoryWithReplies()` matches each
  `replied` event to its reply by author and `createdAt` (both are written
  with the same timestamp). Activity then shows the full Markdown body,
  not the short summary.
- **Read receipts.** Under each comment: "✓ Read by @name", "Not read yet",
  or "Sending…".
  - The relay (`_observe_comment_read` in `world/src/world_office_tasks.py`)
    records a member as a reader when they open the task: `GET` on the
    task, `/history` or `/replies`.
  - It records them only if there is a comment by someone else newer than
    their last read. So it writes at most once per new comment per reader.
  - The record is a `commentReads` map (name → time) sealed inside the
    task's `data`, capped at 100 readers. It needs no migration, and the
    row's `updatedAt` stays the same.
  - Each reply is returned with `readBy: [{name, at}]`: every reader other
    than the author whose read time is at or after the comment.
  - `/history` now returns `replies` too, so one request fills Activity.
  - The web dashboard shows the same line under each comment.
- **Receipts stay current.** A cached timeline older than 30 seconds is
  re-read in the background the next time the detail pane draws
  (`kOrganizationTaskHistoryStaleMs`). The old copy stays on screen while
  it loads.
  - A history reply that left before a newer write is dropped
    (`m_organizationTaskHistoryAt`). An in-flight read therefore cannot
    replace the comment you just posted.
  - A failed re-read keeps the timeline already shown.
- **Without a password session.** The relay now accepts two more
  key-signed proofs:
  - `forkmesh-org-task-reply-v1` on `POST /api/tasks/<id>/replies`. It
    names the task and the body digest, and an empty body is refused.
  - `forkmesh-org-task-history-v1` on `GET /api/tasks/<id>/history`. It
    names the task.

  A desktop holding only its account key can now comment and read
  Activity. Before, both failed or showed an empty timeline.
- **Tests:**
  - `world/tests/test_world_office_tasks.py`
    (`test_a_comment_shows_who_has_read_it`).
  - `app/tests/test_org_task_signed_writes.py`
    (`test_comment_and_history_proofs_bind_the_task_and_the_comment`).
