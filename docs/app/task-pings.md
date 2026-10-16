# Tasks pings

Every change to an organization task files a Ping of kind
`organization_task_update` (label **Tasks**, on by default): an inbox row that
wakes web and desktop clients over the account socket and pushes to phones via
APNs. The change is also emailed ([task-email.md](task-email.md)); a delete
is emailed as one deletion notice per recipient instead.

Code: `_notify_activity` in `world/src/world_office_tasks.py`,
`notify_organization_task_update` in `world/src/world_office_routes.py`, text and
coalescing in `world/src/world_task_pings.py`. Tests:
`world/tests/test_task_pings.py`.

## Events and recipients

Create, every field edit, start/stop, done/undone, QA hand-off and verdict,
comments, check-ins and delete ping; agent status telemetry does not.
Recipients: creator, assignee, previous assignee, commenters, organization
owners; members only, never the actor. An @mention gets a `mention` Ping
instead. The body is one sentence per change; `href` is
`/dashboard/tasks/?task=<id>`. Task descriptions are never copied.

## Coalescing

Within 60 s of the first change, per recipient: a change folds into an open
unread digest or same-task Ping (no push); unread Pings for several tasks are
replaced by one `Tasks updated: N tasks` digest that pushes once.

## Check-in reminders

`POST /api/tasks/<id>/remind` (optional `note`) sends one `mention` Ping to the
assignee. Refused for done tasks (`task_done`), no person assignee
(`task_has_no_assignee`) and self (`cannot_remind_yourself`); one per task per
10 minutes (`REMINDER_COOLDOWN_MS`, else `429 reminder_too_soon`). It bypasses
mutes and quiet hours, is emailed at once, and is projected as `lastReminder`
with delivery and seen state (timeline kinds `reminded`, `reminder_seen`).

## Settings and logs

`PING_KIND_CATALOG` holds the Tasks row (desktop copy in
`desktop/src/PingSettings.h`). Logs: `journalctl -u forkmesh-app | grep -E "task ping|apns:"`.
