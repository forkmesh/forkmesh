# Task trash: soft delete, restore and purge

Added 2026-09-29. Deleting an organization task used to erase it and its
replies, check-ins, QA reviews, attachments and timeline at once. An agent that
can delete tasks (the `delete_task` MCP tool) makes a mistake cheap to make and
impossible to undo, so deletion now moves a task to a trash for 30 days first.

Code: `world/src/world_office_tasks.py` (`_delete`, `_delete_batch`,
`_restore`, `purge_trash`), `world/src/world_office_routes.py` (the runtime's
`delegated_principal` and `agent_session_running`), `app/src/entry.py` (signed
proofs, `purge_trashed_organization_tasks`), migration
`app/migrations/0222_organization_task_trash.sql`.

## API

All routes are under `/api/tasks` (the configured organization, or the bot
token's organization). The legacy Marketing prefix has no trash routes.

| Request | Effect |
| --- | --- |
| `DELETE /api/tasks/<id>` with optional `{"reason": "...", "force": true}` | Moves the task to trash. Answers `{ok, deleted, trashed, restorable, id, deletedAt, purgeAt, workItems}`. |
| `POST /api/tasks/delete` with `{"ids": [...], "reason"?, "force"?}` | Trashes 1 to 50 ids, all or nothing. Answers `{ok, deleted: [ids], count, purgeAt, workItems: {id: [...]}}`. |
| `POST /api/tasks/<id>/restore` | Brings a trashed task back within 30 days. Answers the task, like any other task write. |
| `GET /api/tasks?status=deleted` (also `status=trash`) | The trash, newest deletion first, paged like the board. Each task adds `deletedAt`, `deletedBy`, `deleteReason`, `purgeAt` and `canRestore`; the page carries `trash: true`. |

`force` must be a JSON boolean and `reason` a string (kept up to 500
characters, sealed with the task copy). `workItems` stays in the DELETE answer
because the desktop uses it to offer cleanup of the task's local worktrees.

Errors:

| Status | `error` | Meaning |
| --- | --- | --- |
| 403 | `forbidden` | The caller is not the creator, the assignee or a manager. |
| 409 | `task_delete_blocked` | `blockedBy` lists `agent_running` and/or `qa_in_progress`; `forceRequires: "owner"`; `canForce` says whether this caller is an owner and so may resend with `force`. |
| 403 | `force_requires_owner` | `force` was sent by someone who is not an organization owner. |
| 404 / 403 / 409 | `task_delete_batch_refused` | `failures` lists each refused id with its own error; nothing was trashed. The status is the most serious failure (403, then 404, then 409). |
| 409 | `task_not_deleted` | Restore of a task that is not in the trash. |
| 410 | `task_trash_expired` | Restore after the 30 days; the purge will remove it. |

### Who asked: the `request` block

Added 2026-09-30, after a desktop delete of a finished, merged task came back
as only `DELETE /api/tasks/<id> failed: task delete blocked`, with nothing to
say which guard refused it, or who, which client or which agent sent it.

Every refused delete (single or batch: `forbidden`, `task_delete_blocked`,
`force_requires_owner`, `task_delete_batch_refused`) answers a `request`
object along with the error:

| Field | Value |
| --- | --- |
| `method`, `path` | The refused request. |
| `user` | The account the request acted as. |
| `via` | `session` (a session token or cookie), `signed-key` (a desktop or MCP key signature) or `bot-token`. |
| `node` | The node named in a key signature. |
| `bot` | A bot token's provider. |
| `agent` | The `X-ForkMesh-Agent` header: the MCP server sends its chat name and the name of the process that launched it (`claude`, `codex`, ...). It is the client's claim, not proof. |
| `ip` | The client address the edge saw (`cf-connecting-ip`). |
| `userAgent` | The client's User-Agent: `ForkMeshDesktop/<version>`, `ForkMeshMCP/0.2`, or a browser. |
| `client` | The User-Agent reduced to `browser`, `mobile-browser`, `automated-client` or `other-client`. |
| `at` | Server time of the refusal. |

The caller sees only its own address. The node also prints one
`task delete refused:` line per refused id with all of these fields, so the
server log has the address and the full User-Agent as well.

## What trashing does

`organization_tasks.deleted_at` is set to the server time and
`deleted_by_bi` to the account's blind index; `data` gains a sealed
`trash: {by, reason, at}`. A running timer is stopped: the elapsed time is
added, `status` goes back to `idle` and `active_assignee_bi` is cleared, so the
assignee can start another task (the one-active-task index would refuse it
otherwise). Restoring clears both columns and the sealed `trash` entry; the
task comes back idle.

A trashed task is hidden everywhere the board is read:

- the board list, and so the dashboard, desktop, phones, World boards, the
  remote MCP and the MCP `list_tasks` (all of them page `GET /api/tasks`), and
  every count and search built from it;
- every item route (`GET/PATCH/DELETE /api/tasks/<id>`, replies, history,
  attachments, start, complete, undo, check-in, agent status): `_task` only
  finds rows with `deleted_at=0`;
- the World QA desk (`world/src/world_qa.py`) and its verdict write;
- the agent-run completion hook in `app/src/route_handlers.py`, so a job that
  finishes after its task was trashed does not move it to QA.

The replies, check-ins, QA reviews, attachments and timeline rows stay where
they are, unreadable through the API until the task is restored or purged.
Trashed rows still count toward the 2,000-task catalog trigger until purged.

## Permissions

Trash and restore are allowed to the task's creator, its assignee and an
organization manager (owner, admin, or maintain permission). That is the union
of who may complete a task and who may edit it; before this change only
managers could delete.

An organization bot token (`fmbot_...`) acts with the rights of the member who
created it, read fresh on every request (`delegated_principal`): its creator's
current role and permission, and that creator's own tasks. A token whose
creator left the organization or was deactivated can trash nothing. Without
this a member could mint a task-write bot token, which the board treats as a
manager, and delete tasks the member could not. The MCP server signs as the
desktop's account, so it already has exactly that account's rights.

## Guards

A task is refused with `task_delete_blocked` when:

- `agent_running`: an unfinished task's sealed `agent.status` is `queued`,
  `running` or `waiting`, or its `agent_session_id` names an
  `org_agent_sessions` row in `security_pending`, `queued` or `running` (a
  failed lookup counts as running);
- `qa_in_progress`: QA was requested and no verdict is recorded yet
  (`qa_requested_at > 0` and `qa_reviewed_at = 0`).

Only an organization owner may pass `force: true` to trash it anyway. A forced
delete of a task with a node-queued session also cancels that session, the same
way Return to list does. The audit row records which guards were overridden.

A consequence: a completed task waits for its QA verdict before it can be
trashed. The dashboard's Clear closed therefore skips closed tasks awaiting QA
and says how many it kept. The desktop's Delete closed sends them too, and when
the relay refuses them it offers an owner to resend them with `force`
([../desktop/tasks-page.md](../desktop/tasks-page.md#delete-closed-finishes-the-run-and-says-what-it-did-2026-10-09)).

Deleting an agent session on the desktop (`deleteAgentSessionOrgTask` in
`desktop/src/MainWindowAgentLaunch.cpp`) also trashes the task it opened. Until
2026-09-30 a `task_delete_blocked` answer stopped the whole delete with "Could
not delete attached task …; agent session was kept", so a session whose task
was awaiting QA, or whose own run still showed as running on the board, could
never be deleted. Now a refusal from the guard is retried once with
`force: true` and the reason "Agent session #N deleted on the desktop."; for an
owner that trashes the task. If the retry is refused too
(`force_requires_owner`), the task stays on the board, the desktop says so, and
the session is deleted anyway. When the first refusal names `agent_running`,
the desktop first reports the run `stopped` (`POST /api/tasks/<id>/agent-status`)
and then sends the forced DELETE: the agent being deleted is that run, and once
it is no longer live nothing blocks the delete, so a member who is not an owner
can trash the task too. Attached tasks are trashed with the agent as well
(`docs/desktop/tasks-page.md`). When the blocker was `agent_running` the session
keeps its task link through the teardown, so the usual close note completes
the task instead of leaving it running; otherwise the link is dropped first. A
network or server failure still keeps the session, as before. A signed DELETE
that carries a body adds the body digest to its proof, as the relay requires.

## Audit and timeline

Every trash writes `organization.task_delete` and every restore
`organization.task_restore` to `sensitive_audit_log` (target type
`organization_task`, the task id). Details are metadata only: `force`,
`overrode`, `batch`, `hasReason`, `workItems` count, and for a restore
`trashedForMs`.

A refused delete writes an `organization.task_delete` row too, with outcome
`denied`, one per refused id: `state: "refused"`, `error`, `blockedBy`,
`force`, `batch`, `via`, `node`, `agent` and the generalized `client`. The
address and the raw User-Agent stay out of the audit log, which never stores
either (`sanitize_audit_details` drops any key containing `ip`); they are in
the response and the server log line.

The task timeline records a `deleted` event (its summary is the reason, or
"moved to trash"). A restore is recorded as an `updated` event with the field
"restored from trash": the `organization_task_events.kind` CHECK constraint
lists the allowed kinds, and adding one would mean rebuilding the table.

## Purge

The hourly `sensitive-cleanup` cron phase (minute 52) calls
`purge_trashed_organization_tasks`, which runs `purge_trash`. Each run takes at
most 25 tasks whose `deleted_at` is 30 days old or older, oldest first. For
each it checks the row again (still trashed, still past the cutoff), deletes
the task's attachments, check-ins, QA reviews, replies, timeline rows and
promoted legacy Marketing rows, and deletes the task row last with the same
condition. A run interrupted part way leaves the task row, so the next run
finishes it; running twice purges nothing new. The phase result carries
`{purged, limit, cutoff}` into the cron tick record.

## Signed desktop and MCP requests

Key-signed callers (`node`, `ts`, `sig` on the URL) have one proof per
operation:

- `DELETE` without a body: `forkmesh-org-task-delete-v1\n<node>\n<id>\n<ts>`,
  unchanged, so the desktop and phones keep working.
- `DELETE` with a body: the same prefix with the body digest
  (`org_task_update_digest`, sha256 of the sorted compact JSON) before `ts`,
  so a captured signature cannot be replayed with `force` added.
- `POST /api/tasks/<id>/restore`: `forkmesh-org-task-restore-v1\n<node>\n<id>\n<ts>`.
- `POST /api/tasks/delete`: `forkmesh-org-task-delete-batch-v1\n<node>\n<digest>\n<ts>`;
  an empty body is refused.

The list proof covers `GET /api/tasks?status=deleted`, as it covers the other
unsigned list filters.

## MCP tools

`tools/forkmesh_mcp_server.py`:

- `delete_task(task_id | task_ids[≤50], reason?)`: one id goes as a DELETE,
  a list as the batch. It has no `force` argument; an owner overrides a guard
  from the Tasks page. Before sending anything it reads
  `GET /api/tasks?status=deleted` and refuses unless the page says
  `trash: true`: the desktop runs the MCP server from its checkout, which can
  be newer than the relay, and an older relay would hard-delete.
- `restore_task(task_id)`.
- `list_tasks(status="deleted")` lists the trash with the deletion fields.

Both new tools are connector-gated writes; `delete_task` is the only tool with
`destructiveHint: true`. API refusals are turned into sentences naming the
guard (`an agent job is running on it`, `it is waiting for QA review`) or each
refused id of a batch, then the `request` block (`request: user ..., via ...,
agent ..., ip ...`). Every task request carries `X-ForkMesh-Agent`. The server instructions say the only deletion is this
soft, restorable task delete.

## Clients

- **Dashboard** (`app/public/dashboard/js/09-tasks.js`): Delete (for creator,
  assignee or manager) asks for an optional reason, trashes, and shows "Task
  moved to trash. Undo" as a line above the task list, where the task was
  (`[data-tasks-notice]`, `setTaskListNotice`). Delete closed shows "Moved N
  closed tasks to trash. Undo" there. Until 2026-10-09 this was a toast that
  vanished after 10 seconds; the user asked for no toasts, with information
  shown inline, so the line stays until it is replaced by the next result,
  the status tab changes, or Undo is used (Undo then reports "Task restored."
  in the same line). Restoring from the Trash tab reports there too. A blocked
  delete says why and switches the button to
  "Move to trash anyway", which only an owner's request passes. The **Trash**
  filter lists the trash with who deleted each task, the reason and the purge
  date, and restores it. The tasks package view
  (`marketplace/packages/tasks/content/web/view.html`, served in production)
  carries the same markup; the package moved to 1.0.3.
- **Desktop**: reads the same list, so trashed tasks drop out. Delete and
  Delete closed now trash; Delete closed offers an owner to force the closed
  tasks the relay refuses (awaiting QA, or a run that still looks live). A blocked
  Delete says why (`This task is busy: it is waiting for QA review`). When the
  answer's `canForce` is true (or missing, from an older relay) it asks "Move
  it to the trash anyway?" and resends with `force: true`, the same override
  the dashboard offers. Without this an owner could not trash a finished task
  still awaiting QA from the desktop at all. The key-signed DELETE proof
  covers that body with its digest (see below). Task requests send
  `User-Agent: ForkMeshDesktop/<version>`, and a failed task request's log
  line ends with the server's `request` block in brackets. When a task
  disappears from the board the desktop still offers to clean its local
  worktrees (clean, merged ones go without a prompt; unmerged ones ask and
  keep a backup ref), so a task trashed elsewhere can come back without a
  worktree that was already merged. There is no desktop trash view; restore is
  on the web.
- **Phones**: read and delete through the same routes; a delete now trashes.
  Android's "move to issue" files the issue and then deletes the task, so a
  task awaiting QA stays on the board with the issue filed.
- **World**: the office task wall's delete now trashes. It reports "Task
  deleted." inline (no toasts since 2026-10-09): on the Office task panel's
  status line, next to the Work tab control, or under the island's action bar
  when the delete came from the physical wall.

## Deploying

The change is additive. `tools/next_deploy.py` applies migration 0222 in its
`migrate` step before the rolling restart; the app's lazy schema applier adds
the columns too (`SCHEMA_PRE_CREATE_ALTER_STATEMENTS`). During the rolling
restart an old node still hard-deletes and still lists trashed rows, until it
restarts. The migration is numbered 0222 because another branch already uses
0221; whichever lands second moves the schema fingerprint stamp to its own
latest file (`app/tests/test_local_migrations.py`).
