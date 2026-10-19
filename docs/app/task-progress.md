# Organization task progress

Every organization task carries `progress` (0 to 100), a plain column
`organization_tasks.progress` so updates do not re-seal the task body. A done
task reports `100`; the stored value returns on "Mark undone".

## API

- `PATCH /api/tasks/<id>` with exactly `{"progress": n}` sets it (rounded and
  clamped; non-numeric answers `400 invalid_progress`). Allowed to owners,
  admins, `maintain` members, the creator and the assignee. Writes
  `organization.task_progress_changed`, an `updated` event and a
  [Tasks Ping](task-pings.md).
- Mixed with other fields, a PATCH goes through the manager-only general update.

Signed path for desktops and agents:

```
progress: "forkmesh-org-task-progress-v1\n<account>\n<task id>\n<N>\n<ts>"
any body: "forkmesh-org-task-update-v1\n<account>\n<task id>\n<sha256 hex>\n<ts>"
```

The digest is SHA-256 of canonical JSON (sorted keys, compact, `sessionToken`
omitted): `org_task_update_digest` (relay), `task_update_digest` (MCP),
`desktop/src/TaskWorkItems.cpp`.

## Work items

`workItems` (sealed) lists up to 20 `{"repository", "branch", "worktree",
"node"}` entries, unique on `(repository, branch, node)`. `PATCH` with
`workItems` replaces the list; `attachWorkItems` / `detachWorkItems` merge on
the relay atomically (`UPDATE ... WHERE data=<sealed copy read>`, retried up to
`WORK_ITEM_UPDATE_ATTEMPTS`, then `409 task_busy`). Task editors may change work
items while a task is active or done. Changes write
`organization.task_work_items_changed`.

## MCP and web

MCP tools: `list_tasks`, `get_task`, `create_task`, `update_task`,
`complete_task`, `reopen_task`, `attach_work_item`, `detach_work_item`, plus the
[trash tools](task-trash.md). The web Tasks page (`09-tasks.js`) draws a
progress bar and slider, a Work items card and image thumbnails. An image
preview is a named native dialog whose Close control returns focus to its
attachment. Production
serves it from `marketplace/packages/tasks/content/web/view.html`; copy any
`.task-*` style there (`app/tests/test_task_pagination.py` checks).
