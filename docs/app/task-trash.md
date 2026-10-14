# Task trash: soft delete, restore and purge

Deleting an organization task moves it to a trash for 30 days, so a mistaken
delete (including one made by an agent over MCP) can be undone. Code:
`world/src/world_office_tasks.py` (`_delete`, `_delete_batch`, `_restore`,
`purge_trash`), `app/src/entry.py`, migration `0222_organization_task_trash.sql`.

## API

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

## Rules

- Trashing sets `deleted_at` / `deleted_by_bi`, seals the reason, and stops a
  running timer. Trashed tasks are hidden from every board read, item route,
  the QA desk and the agent-run completion hook; child rows remain until purge.
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

## Clients

MCP (`tools/forkmesh_mcp_server.py`): `delete_task` (no `force`; refuses unless
the relay lists `trash: true`), `restore_task`, `list_tasks(status="deleted")`.
The dashboard shows an inline Undo line and a Trash filter; desktop, phones and
World use the same routes. Restore is web-only.
