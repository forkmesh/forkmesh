# Organization task progress

Every organization task carries `progress`, an integer from 0 to 100
(migration 0207, column `organization_tasks.progress`, default 0). It is a
plain column rather than part of the sealed task copy: a percentage is routing
metadata like `priority`, and updating it must not re-seal the task body.

## API

- `GET /api/tasks` and `GET /api/tasks/<id>` include `progress` on every task.
  A task that is done always reports `100`; the stored value is kept, so
  "Mark undone" brings back the value it had before completion.
- `PATCH /api/tasks/<id>` with a body of exactly `{"progress": n}` sets it.
  `n` may be a number or a numeric string; it is rounded and clamped to 0..100.
  Anything non-numeric (including booleans and NaN) answers
  `400 invalid_progress`. Progress combined with any other field is not a
  progress edit and falls through to the general, manager-only update.
- Older clients ignore the field; nothing else in the task shape changed.

## Who may set it

Task editors: an organization owner or admin, a member with `maintain`, the
task's creator, and its assignee. Anyone else gets `403 forbidden`. This is
wider than priority (managers only) on purpose: the person doing the work is
the one who knows how far along it is.

Every change writes the audit entry `organization.task_progress_changed`
(details `progress` and `previous`) and a timeline event of kind `updated`
whose summary reads `progress N%`. A change also files a Tasks Ping
([task-pings.md](task-pings.md)).

## Signed (desktop and agent) path

A desktop without a session token signs the edit with its account key, the
same way it signs priority edits:

```
PATCH /api/tasks/<id>?node=<account>&ts=<ms>&sig=<base64 ed25519>
body: {"progress": N}

canonical = "forkmesh-org-task-progress-v1\n<account>\n<task id>\n<N>\n<ts>"
```

The signature names the task and the exact value, so a captured request cannot
be replayed against another task or as another value, and it cannot be used as
a priority proof (or the reverse). The resolved account then passes the same
editor check as a web session.

## Full signed update (2026-09-25)

Any other PATCH body can be signed too. The proof is bound to the task and to
the SHA-256 of the body's canonical JSON (keys sorted, `(",", ":")`
separators, `ensure_ascii=False`, `sessionToken` left out):

```
canonical = "forkmesh-org-task-update-v1\n<account>\n<task id>\n<sha256 hex>\n<ts>"
```

`org_task_update_digest` in `app/src/entry.py` computes the hash, and the MCP
server's `task_update_digest` computes the same one. A body with priority or
progress alone still uses its own proof. The resolved account then goes
through the manager check in `world_office_tasks`, the same as a session.

For moving tasks between boards without losing anything, the update verb also
takes `parentTaskId` (checked to exist, and walked up to 32 levels to refuse a
cycle or the task itself), `department` and `team` (validated like create;
the routing snapshot and the columns move together) and `howToTest`.

## MCP tools

`tools/forkmesh_mcp_server.py` exposes the board to agents, signed as the
desktop's account: `list_tasks` (status open/done/all, text query,
department, team, parent, `full` for every field), `get_task`, `create_task`
(every create field: routing, destination, assignee, repository, priority,
parent, kind, bounty, howToTest, sendToQa, agent), `update_task`,
`complete_task` and `reopen_task`. A published connector gates reads like the
CRM tools, and gates writes like every other write tool. `delete_task` and
`restore_task` trash and restore tasks; see [task-trash.md](task-trash.md).

`workItems` is in `TASK_CREATE_FIELDS`, `TASK_UPDATE_FIELDS`, the create and
update input schemas, and the `list_tasks` summary. Two write tools edit the
list without retyping it:

- `attach_work_item(task_id, branch, repository?, worktree?, node?)` sends
  `{"attachWorkItems": [item]}`; the relay merges it (see below). The relay
  refuses a 21st item.
- `detach_work_item(task_id, branch, repository?, node?)` reads the task to
  check the item is on it (an item that is not is an error), then sends
  `{"detachWorkItems": [item]}`. Nothing on disk is touched.

Use them to link a branch the desktop did not create itself.

## Work items: branches and worktrees (2026-09-25)

A task carries `workItems`, the branches and worktrees that belong to it. It
lives in the sealed task copy, next to `repository` and `howToTest`, so a
worktree path never reaches a plaintext column.

- Shape: a list of at most 20 objects
  `{"repository", "branch", "worktree", "node"}`.
  - `repository`: `owner/name` (the same rule as the task's `repository`), or
    empty.
  - `branch`: required, at most 200 characters from `A-Za-z0-9._/+-`. Refused:
    `..`, `//`, a leading `-`, `/` or `.`, a trailing `/` or `.`, a `.lock`
    suffix on any path component, a component starting with `.`, and `HEAD`.
    Spaces and control characters never pass the character class.
  - `worktree`: an absolute path on the owning machine (`/...` or `C:\...`), at
    most 400 characters, no control characters and no `..` segment. Empty for
    a branch-only item.
  - `node`: the node (or account) name that owns the worktree, lowercased, with
    the node-name rule `^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$`, or empty.
  - Entries are de-duplicated on `(repository, branch, node)`, with the
    repository compared case-insensitively. The last entry wins, so attaching
    the same branch again updates its worktree path.
  - A bad entry answers `400 invalid_work_item`; more than 20 answers
    `400 too_many_work_items`; a non-list answers `400 invalid_work_items`.
- `POST /api/tasks` accepts `workItems`. `PATCH /api/tasks/<id>` with
  `workItems` replaces the whole list.
- `PATCH /api/tasks/<id>` with `attachWorkItems` and/or `detachWorkItems`
  (each a list in the same shape, plus `sessionToken`, nothing else) edits the
  list on the relay (2026-09-27). Detach removes entries with the same
  `(repository, branch, node)` key; attach then adds or replaces entries by
  that key, so re-attaching keeps the item's position. Same permissions, same
  audit entry and timeline event as the `workItems` narrow edit.
- A PATCH carrying only `workItems` (plus `sessionToken`) is its own narrow
  edit, like progress: task editors (manager, creator or assignee) may make it,
  and it is allowed while the task is active or done. A desktop attaches a
  worktree while its agent is running, which is exactly when the general
  update refuses (`active_task_cannot_be_updated`). An unchanged list writes
  nothing. `workItems` mixed with other fields goes through the general,
  manager-only update like every other field.
- Attach and detach are atomic. Before 2026-09-27 the desktop and the MCP
  tools read the task, merged, and sent the whole list, so two nodes attaching
  to one task at the same moment could drop one of the items. The relay now
  writes the merged list with `UPDATE ... WHERE data=<the sealed copy it
  read>`. Each seal is fresh ciphertext, so any other write since the read
  (another attach, an edit, completion) makes the update touch no rows; the
  relay then re-reads the task and merges again, up to 5 times
  (`WORK_ITEM_UPDATE_ATTEMPTS`), and answers `409 task_busy` after that. The
  row count comes from `meta.changes`, which D1 and both self-hosted adapters
  (`app/host/forkmesh_host/d1_sqlite.py`, `d1_rqlite.py`) report. A full
  `workItems` replacement and the general update still write whatever they
  read last. Tests: `test_work_item_attach_and_detach_merge_on_the_server` and
  `test_concurrent_work_item_attaches_keep_both_items` in
  `world/tests/test_world_office_tasks.py`.
- Every change writes the audit entry `organization.task_work_items_changed`
  (details `workItems` and `previous`, the counts) and an `updated` timeline
  event whose summary reads `attached <branch>` or `detached <branch>`.
- Every task projection (list and item) returns `workItems`. Completion,
  QA hand-off and Mark undone keep the list, because they re-seal the opened
  task copy; the general update re-seals it explicitly.
- `DELETE /api/tasks/<id>` returns the deleted task's `workItems`, so the
  client that deleted it knows what to clean. The deletion audit records the
  count.
- The signed path is the full signed update above: the digest covers
  `{"workItems": [...]}`. The desktop computes that digest in
  `desktop/src/TaskWorkItems.cpp` (`canonicalJson`, `updateDigest`), and
  `app/tests/test_org_task_signed_writes.py` and
  `desktop/tests/test_task_work_items.cpp` share one fixed test vector.

What the desktop does with the list when a task is deleted is in
[the desktop Tasks page doc](../desktop/tasks-page.md#work-items-and-delete-cleanup).

## Web UI

The Tasks page (`app/public/dashboard/tasks/index.html`, JS in
`app/public/dashboard/js/09-tasks.js`) draws a slim bar with the percentage on
every row and a Progress card in the detail pane. Editors get a slider and a
number input with Save. The bar is `role="progressbar"` with `aria-valuenow`.
Colours come from the dashboard theme tokens, so light, dark and phone layouts
need no extra rules. Browser test: `app/browser_tests/tests/dashboard-task-progress.spec.js`.

Work items (2026-09-27): the detail pane has a wide **Work items (N)** card
listing each item's branch, its repository (linked to the repository page),
and `on <node> · <worktree>` when those are set. List rows show a branch icon
with the count, the search box matches branch, repository and node names, and
**Copy prompt** adds a `Branches:` line. Items without a branch are skipped.
Test: `test_task_detail_lists_work_items_escaped_with_repository_links` in
`app/tests/test_dashboard_tasks_management.py` runs the card, search and
prompt helpers from `09-tasks.js` under Node.

Rows also show a task's attached images (2026-09-27): up to three 2.5 rem
square previews at the right end of the row, with "+N" on the last one when
there are more. The previews come from each attachment's inline `thumbnail`,
so the list never fetches full images. An attachment without a thumbnail
shows an image icon instead. The web task editor now makes the same 48 px
thumbnail the desktop and chat uploads send (`taskImageThumbnail`: a
centre-cropped WebP under 10,000 characters; none for GIFs, which the server
would drop). Tasks created on the web now show a preview in every client's
list. Tests: `test_task_rows_show_attached_image_thumbnails` and
`test_web_task_uploads_carry_a_list_thumbnail` in
`app/tests/test_dashboard_tasks_management.py`. The row styles pushed the
Tasks package to 1.0.2.

Production serves `/dashboard/tasks/` from the Tasks package view,
`marketplace/packages/tasks/content/web/view.html` (built to
`dist/dashboard-plugins/tasks/default/index.html`). Any CSS or markup change to
the tasks partial must be copied there as well, or it ships unstyled.
`app/tests/test_task_pagination.py` fails when a `.task-*` rule in the partial
is missing from the package view. The package moved to 1.0.1 for the synced
styles, so the Market page and desktop package installs see the change too, and
to 1.0.4 for the work-items card styles (`.task-work-items`, `.task-work-item`).

## Not done yet

- The work-items desktop code (and its switch to `attachWorkItems`) reaches
  users with the first desktop release after 1.2.5; cutting that release is a
  separate, signed release step.
- The 161 landscape tasks have no work items yet. Their branch-to-task map is
  a `landscape_log.json` scratchpad that is not in the repository; with it,
  one `attach_work_item` call per branch backfills them.

- Desktop (Qt) Tasks tab and the native iOS/Android apps do not show or set
  progress yet. The signed proof above is what they should use.
- The World office task board does not draw the bar.
