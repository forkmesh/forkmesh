# Organization tasks

Part of [the App docs](README.md). An organization task is a row on the web
Tasks page, the desktop [Tasks page](../desktop/tasks-page.md) and the phone
apps, read and written through `/api/tasks` and the MCP task tools. This page
covers the web Tasks page, Markdown task text, progress and work items, the
trash, and the Pings and email every task change sends. Turning an issue into
a task is in [issue-to-task.md](issue-to-task.md).

## Tasks page

The web Tasks page shows seven status tabs (Open, In progress, Mine, QA, Done,
Trash, All) as a segmented control: icon, label and count. The labels hide
when the toolbar is narrower than 46rem (a container query), leaving the
icons and counts. A people strip (`[data-tasks-people]`: Everyone, each
member's avatar, Bot, Unassigned) shows each person's open task count.
Clicking an avatar filters to that person; clicking it again returns to
Everyone.

### Layout (Tracker)

The layout follows Linear and Plane issue views: the detail pane shows only
the actions that apply now, sections are not boxed, and status, priority and
progress each have a shape to scan by.

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
  in the menu, greyed out, so nothing disappears. Under the action bar is the
  quick comment box (`taskQuickCommentHtml`, Ctrl+Enter posts), so a comment
  never needs a scroll past a long timeline. Below come the check-in and
  remind panels (boxed, `data-panel`), Description, completion note, work
  items, images, completion history and Activity (timeline only).
- Detail side panel (`.task-detail-aside`): Status, Priority, Assignee, Created
  by, Repository, Department, Team, Follows and Routed to
  (`taskEditablePropsHtml`). Each field is edited in place and saved on
  change (`submitTaskProperty`): Status runs start, stop, complete or undo;
  the rest send a one-field PATCH. A field is a control only when the server
  would accept the write (`taskPropertyAccess`: managers edit a task that is
  not active or done, priority at any time; status follows the action rules);
  otherwise it is plain text with the reason as its tooltip. Then Progress
  with its slider; the persistent
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

### Count rules (`taskFilterCounts`, `taskPeopleOpenCounts`)

- Tab counts follow the selected person and the search box.
- **Mine** always counts your own open tasks; picking it resets the person.
- **Trash** has no count until the trash is loaded (no extra request per view).
- Avatar counts are open tasks only and ignore search and tab.

### Where it lives

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

## Task text is Markdown

Task `details`, QA `howToTest`, completion notes (`completionNote`,
`completionHistory[].note`) and reply bodies are Markdown. They keep line breaks
end to end and are rendered on the web Tasks page and the desktop Tasks view.
Titles and other routing values stay single-line.

### Server

`world/src/world_office_tasks.py` bounds multi-line fields with
`_multiline(value, maximum, fallback)`: CRLF to `\n`, tab to four spaces, other
control characters to a space, `<` and `>` kept, trailing spaces and extra blank
lines trimmed. Caps: details 4000, howToTest 720, completion note 4000, reply
2000. Every other field uses single-line `_text()`, which collapses whitespace
and strips `<`/`>`; plain consumers (Pings, mentions, event summaries) use it too.

### Web rendering

`taskMarkdownHtml()` in `app/public/dashboard/js/09-tasks.js` uses the shared
`renderMarkdown()` (`06-repo-content.js`): it escapes text before adding markup,
flattens raw HTML, allows only `http:`, `https:` and `mailto:` links
(`mdSafeUrl()`), and opens external links with
`rel="noopener noreferrer nofollow ugc"`. `.task-markdown` resets `white-space`
to `normal`. Rebuild with `app/tools/build_dashboard_assets.py`.

Tests: `world/tests/test_world_office_tasks.py`,
`app/tests/test_task_markdown_frontend.py`,
`tools/test_forkmesh_mcp_server.py`, and the desktop
`forkmesh-task-markdown-tests` ([desktop tasks page](../desktop/tasks-page.md)).

## Progress and work items

Every organization task carries `progress` (0 to 100), a plain column
`organization_tasks.progress` so updates do not re-seal the task body. A done
task reports `100`; the stored value returns on "Mark undone".

### Progress API

- `PATCH /api/tasks/<id>` with exactly `{"progress": n}` sets it (rounded and
  clamped; non-numeric answers `400 invalid_progress`). Allowed to owners,
  admins, `maintain` members, the creator and the assignee. Writes
  `organization.task_progress_changed`, an `updated` event and a
  [Tasks Ping](#pings).
- Mixed with other fields, a PATCH goes through the manager-only general update.

Signed path for desktops and agents:

```
progress: "forkmesh-org-task-progress-v1\n<account>\n<task id>\n<N>\n<ts>"
any body: "forkmesh-org-task-update-v1\n<account>\n<task id>\n<sha256 hex>\n<ts>"
```

The digest is SHA-256 of canonical JSON (sorted keys, compact, `sessionToken`
omitted): `org_task_update_digest` (relay), `task_update_digest` (MCP),
`desktop/src/TaskWorkItems.cpp`.

### Work items

`workItems` (sealed) lists up to 20 `{"repository", "branch", "worktree",
"node"}` entries, unique on `(repository, branch, node)`. `PATCH` with
`workItems` replaces the list; `attachWorkItems` / `detachWorkItems` merge on
the relay atomically (`UPDATE ... WHERE data=<sealed copy read>`, retried up to
`WORK_ITEM_UPDATE_ATTEMPTS`, then `409 task_busy`). Task editors may change work
items while a task is active or done. Changes write
`organization.task_work_items_changed`.

### MCP and web

MCP tools: `list_tasks`, `get_task`, `create_task`, `update_task`,
`complete_task`, `reopen_task`, `attach_work_item`, `detach_work_item`, plus the
[trash tools](#trash). The web Tasks page (`09-tasks.js`) draws a
progress bar and slider, a Work items card and image thumbnails. An image
preview is a named native dialog whose Close control returns focus to its
attachment. Production
serves it from `marketplace/packages/tasks/content/web/view.html`; copy any
`.task-*` style there (`app/tests/test_task_pagination.py` checks).

## Trash

Deleting an organization task moves it to a trash for 30 days, so a mistaken
delete (including one made by an agent over MCP) can be undone. Code:
`world/src/world_office_tasks.py` (`_delete`, `_delete_batch`, `_restore`,
`purge_trash`), `app/src/entry.py`, migration `0222_organization_task_trash.sql`.

### Trash API

| Request | Effect |
| --- | --- |
| `DELETE /api/tasks/<id>` `{"reason"?, "force"?}` | Trash one task; answers `purgeAt` and the task's `workItems`. |
| `POST /api/tasks/delete` `{"ids": [...], ...}` | Trash 1 to 50 ids, all or nothing. |
| `POST /api/tasks/<id>/restore` | Restore within 30 days. |
| `GET /api/tasks?status=deleted` | The trash (`trash: true`; each task adds `deletedAt`, `deletedBy`, `deleteReason`, `purgeAt`). |

Errors: `forbidden`, `task_delete_blocked` (`blockedBy`, `canForce`),
`force_requires_owner`, `task_delete_batch_refused`, `task_not_deleted`,
`task_trash_expired`. Refusals also return a `request` block (account, `via`,
node, agent header, client class) and log a `task delete refused:` line.

### Trash rules

- Trashing sets `deleted_at` / `deleted_by_bi`, seals the reason, and stops a
  running timer. Trashed tasks are hidden from every board read, item route,
  the QA desk and the agent-run completion hook; child rows remain until purge.
- A delete counts the task as done: an open task gets `completed_at` set to
  the delete time (so it reports `done` and progress 100, and anything counting
  `completed_at>0` treats it as finished). The sealed trash record keeps
  `completed: true` for that case, and restore clears `completed_at` again; a
  task that was already done keeps its original completion time either way.
- Deleting emails the task's creator and assignee (a person, not the bot),
  organization members only, never the deleter, once each per request: a batch
  sends each recipient one email listing every deleted task they are on (see
  [Deleted tasks](#deleted-tasks)).
- Allowed to creator, assignee or manager. A bot token acts with its creator's
  current rights.
- Guards `agent_running` and `qa_in_progress` block deletion; only an owner may
  override with `force: true` (which also cancels a queued session).
- Audit: `organization.task_delete` / `organization.task_restore` in
  `sensitive_audit_log`, metadata only (no IP or raw User-Agent).
- Purge: the hourly `sensitive-cleanup` cron phase removes at most 25 expired
  tasks per run, child rows first, so an interrupted run is resumable.
- Signed proofs: `forkmesh-org-task-delete-v1\n<node>\n<id>\n<ts>` (a body adds
  its digest before `ts`), `forkmesh-org-task-restore-v1`, and
  `forkmesh-org-task-delete-batch-v1\n<node>\n<digest>\n<ts>`.

### Trash clients

MCP (`tools/forkmesh_mcp_server.py`): `delete_task` (no `force`; refuses unless
the relay lists `trash: true`), `restore_task`, `list_tasks(status="deleted")`.
The dashboard shows an inline Undo line and a Trash filter; desktop, phones and
World use the same routes. Restore is web-only.

## Pings

Every change to an organization task files a Ping of kind
`organization_task_update` (label **Tasks**, on by default): an inbox row that
wakes web and desktop clients over the account socket and pushes to phones via
APNs. The change is also emailed ([Email](#email)); a delete is emailed as one
deletion notice per recipient instead.

Code: `_notify_activity` in `world/src/world_office_tasks.py`,
`notify_organization_task_update` in `world/src/world_office_routes.py`, text and
coalescing in `world/src/world_task_pings.py`. Tests:
`world/tests/test_task_pings.py`.

### Events and recipients

Create, every field edit, start/stop, done/undone, QA hand-off and verdict,
comments, check-ins and delete ping; agent status telemetry does not.
Recipients: creator, assignee, previous assignee, commenters, organization
owners; members only, never the actor. An @mention gets a `mention` Ping
instead. The body is one sentence per change; `href` is
`/dashboard/tasks/?task=<id>`. Task descriptions are never copied.

### Coalescing

Within 60 s of the first change, per recipient: a change folds into an open
unread digest or same-task Ping (no push); unread Pings for several tasks are
replaced by one `Tasks updated: N tasks` digest that pushes once.

### Check-in reminders

`POST /api/tasks/<id>/remind` (optional `note`) sends one `mention` Ping to the
assignee. Refused for done tasks (`task_done`), no person assignee
(`task_has_no_assignee`) and self (`cannot_remind_yourself`); one per task per
10 minutes (`REMINDER_COOLDOWN_MS`, else `429 reminder_too_soon`). It bypasses
mutes and quiet hours, is emailed at once, and is projected as `lastReminder`
with delivery and seen state (timeline kinds `reminded`, `reminder_seen`).

### Settings and logs

`PING_KIND_CATALOG` holds the Tasks row (desktop copy in
`desktop/src/PingSettings.h`). Logs: `journalctl -u forkmesh-app | grep -E "task ping|apns:"`.

## Email

Every task activity that files a [Tasks Ping](#pings) is also emailed (mail
kind `task`). Opens, clicks and views are written to the task timeline and
the private [#email-log](../operations/mail-log.md) channel.

Code: `world/src/world_task_email.py`; shared tracking in
`app/src/mail_activity.py`; records in `email_sends` (`ref` =
`task:<org_bi>:<task_id>`). Tests: `world/tests/test_task_email.py`,
`app/tests/test_mail_activity.py`.

### Recipients and limits

The Ping recipients plus the creator on create and @mentioned people; never the
actor otherwise. At most 10 recipients per change (`MAX_RECIPIENTS`) and 30 task
emails per address per hour (`HOURLY_CAP`). Only active, verified accounts with
`email_notifications` on. Each email has one-click `List-Unsubscribe`, an open
pixel and tracked links. A failed send never fails the task change. Task
descriptions are never copied.

### Deleted tasks

A delete skips the per-change email. `_delete` and `_delete_batch` call
`notify_organization_task_deletions` (`world/src/world_office_routes.py`) once
per request. It groups the deleted tasks per recipient (creator and person
assignee, members only, never the deleter), and `send_deleted` sends each one
email: `Task deleted: <title>` or `N tasks deleted`, a line per task with who
deleted it and the reason, a note that a deleted task counts as done and stays
restorable for 30 days, and a link to the Tasks list (no view token). The same
opt-outs, verification check and hourly cap apply; each task's timeline gets an
`emailed` entry. Commenters and owners still get the delete Ping.

### Tracking

| Event | Source | Timeline kind |
|---|---|---|
| Sent | `send_activity` | `emailed` |
| Open | pixel `/api/mail/t/o/<token>.gif` or Mailtrap webhook | `email_opened` |
| Click | `/api/mail/t/c/<token>` or Mailtrap webhook | `email_clicked` |
| View | Tasks page opened with a valid `mail` token | `email_viewed` |

Only the first of each per email reaches the timeline. The view token
(`mail_activity.view_token`) is distinct from the unsubscribe token, counts only
for a signed-in member who can read the task, and is stripped from the address
bar after use. Tracking routes accept 32-hex and 36-character send ids, and the
tracker HTML-unescapes each `href` before signing it.
