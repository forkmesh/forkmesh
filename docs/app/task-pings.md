# Tasks pings

Every change to an organization task files a Ping of kind
`organization_task_update` (label **Tasks** in Ping settings, on by default,
no email). A Ping is an inbox row: `enqueue_notification` wakes the
recipient's web and desktop clients over the account socket and pushes it to
their phones over APNs, per app bundle (migration 0202). Pings are not email,
so the daily mail budget (migration 0208) does not apply.

The same change is also sent as its own email (kind `task`, not the Ping
digest), with opens, clicks and views logged on the task and in #email-log:
[task-email.md](task-email.md).

## Code map

| Path | Role |
| --- | --- |
| `world/src/world_office_tasks.py` | Every mutation calls `_notify_activity` with the action and a list of `changes` (field, from, to). General edits diff the stored copy (`_edit_changes`). |
| `world/src/world_office_routes.py` | `notify_organization_task_update` (module level, exported from `entry.py`): recipients, then `world_task_pings.deliver`. `_TaskPingStore` reads and writes the `notifications` rows. |
| `world/src/world_task_pings.py` | Pure text and coalescing: per-task Ping, 60 s window, bulk digest. No Worker globals, tested directly. |
| `world/src/world_qa.py` | QA verdicts (passed or failed) ping through the same function. |
| `app/src/route_handlers.py` | A cloud agent run that completes its task pings as `<provider>-bot`. |
| `world/tests/test_task_pings.py` | Event coverage, recipients, coalescing, digest, mute, no secrets. |

## Events

Create, title and description edits, assignment and unassignment (the
previous assignee is told too), priority, progress, repository, a move to
another department or team, test steps (`howToTest`), the parent task, work
items attached or detached (on their own or inside an edit), start, stop
(including the World-exit auto stop), done, undone, sent to QA, QA passed or
failed, returned to the list, comments, check-ins, and delete. The web
session, the desktop's signed requests and organization bot tokens all reach
the same handlers, so they ping alike. Agent run status telemetry
(`agent-status`) does not ping: a desktop reports it for every local session.

Tasks have no due date field, so there is no due date event.

## Recipients

The task's creator, its current assignee (a person, not the bot), a previous
assignee on reassignment, everyone who commented or checked in (read from the
task timeline, `organization_task_events` kinds `replied` and `checkin`), and
every organization owner. Only current organization members are pinged. The
actor never is. Someone @mentioned in a comment or check-in gets the
`mention` Ping instead, not both.

## Content

- Title: `Task updated: <title>`, `New task: <title>` or `Task deleted: <title>`.
- Body: one change reads as a sentence, `Progress 40% → 60% by @jett`,
  `Assigned to @dana (was @bob) by @jett`, `New comment by @carol: <first 80 characters>`.
- `href`: `/dashboard/tasks/?task=<id>`. The Tasks page selects that task on
  load (`applyLinkedTask` in `app/public/dashboard/js/09-tasks.js`).
- `meta` keeps the old organization task fields (organization, department,
  team, priority, status, assignee, repository) so the web Ping modal still
  lists them, plus `entries`, `count`, `actors`, `firstAt` and `dedupe`.
  Task descriptions are never copied into a Ping.

## Coalescing and the digest

Per recipient, `deliver` reads the newest 12 rows filed in the last 60 s and
keeps this kind's rows whose `firstAt` is inside the window:

1. An unread digest is open: the change folds into it, no push.
2. An unread Ping for the same task is open: the change folds into it, no
   push. The body becomes `3 changes: progress 60%, priority 5, comment by @jett`.
3. Unread Pings for other tasks are open: they are replaced by one digest,
   `Tasks updated: N tasks`, which pushes once. A board-wide reprioritisation
   is one digest per recipient and at most two phone banners.
4. Otherwise a new Ping, which pushes unless the recipient already read a
   Ping for this task inside the window.

The window is anchored on the first change (`firstAt`), so a long burst
starts a new Ping every minute.

## Check-in reminders

Any organization member who can see a task can press **Remind** on it: the
web Tasks page, the desktop Tasks page, the World office board and the iOS
and Android task screens. `POST /api/tasks/<id>/remind` (optional `note`, up
to 500 characters) files one `mention` Ping to the task's assignee asking
them to check in. It is refused for a done task (`task_done`), a bot or
unassigned task (`task_has_no_assignee`) and the assignee reminding
themselves (`cannot_remind_yourself`).

- It is a `mention`, not an `organization_task_update`, because it is
  addressed at one person. It is not coalesced or digested, and the task's
  other watchers are not pinged. The desktop pops it like any task mention
  (its `meta.taskId` is set), and `href` opens the task.
- One reminder per task per 10 minutes (`REMINDER_COOLDOWN_MS`). An earlier
  retry gets `429 reminder_too_soon` with `retryAt` and `lastReminder`.
- The latest reminder is kept in the task's sealed copy and projected as
  `lastReminder`: `at`, `by`, `to` (the assignee), `note`, `delivered`,
  `phones`, `seenAt` and `seenVia`. A reminder does not move `updatedAt`. The
  write compares the stored sealed copy, so a concurrent edit makes it fail
  with `task_state_conflict` instead of losing that edit.
- It always rings (2026-10-02). `enqueue_notification(..., force=True)` skips
  the assignee's muted kinds (a switched-off Mentions row), the pause and quiet
  hours, the same as a `security_alert`, because a person asked this person
  directly. The desktop raises it as a resident in-app card plus an OS card
  even while pings are paused (`refreshWebAlerts` in `MainWindowActions.cpp`).
  Android's `PingAlertLedger` posts it past muted kinds and quiet hours. iOS
  gets the APNs banner the unpaused ping already sends. The web dashboard has
  no browser notification, only the bell.
- Each reminder is a timeline event (`reminded`, 2026-10-02), recorded by the
  sender. Its summary says what was delivered: `Reminded @bob — ping sent,
  pushed to 2 phones: "note"`, or `Reminder to @bob could not be delivered`.
  `enqueue_notification(..., receipt={})` fills in the ping id and the number
  of phones APNs accepted. The notifier returns them, and `_remind` stores them
  in `reminder.delivery`. A ping that was not filed is also written to the
  error log (`organization/tasks/remind`). The response then carries
  `reminderError`, and every client shows "saved, but could not be pinged"
  instead of "sent".
- Seen tracking. A `reminder_seen` event is recorded once per reminder, and
  `seenAt`/`seenVia` are set when either of these happens first:
  - The assignee opens or acts on the task. Any request on
    `/api/tasks/<id>/...` by them counts, including the desktop's
    `/history` read when a task is selected and a check-in. That is
    `seenVia: "task"`.
  - The reminder's ping is marked read. A `GET` of the task or its history,
    by anyone, looks up `notifications.read_at` for the stored ping id
    (`reminder_ping_read_at` on the task runtime). That is `seenVia: "ping"`.
    The desktop never marks web pings read, so for a desktop-only assignee
    opening the task is the signal.
  A list read never marks a reminder seen.
  `organization_task_events.kind` has a CHECK list, so migration
  `0230_organization_task_reminder_events.sql` rebuilds the table to admit
  `reminded` and `reminder_seen`. Until it is applied, `_record_event` drops
  those rows, and the reminder and its ping are unaffected.
- It is emailed to the assignee right away, not held for the hourly digest:
  its `meta.context` is `reminder`, which `notification_email_immediate`
  treats as immediate. The Mentions email setting turns it off.
- Copy (`organization_task_reminder_copy` in `app/src/entry.py`):
  `@carol asked you for a check-in on "<title>" in <org>`, with the note
  quoted in the body.

## Settings and older clients

`PING_KIND_CATALOG` replaces the two retired rows
(`organization_task_started`, `organization_task_activity`, no longer raised
or accepted) with the Tasks row. Web and phones read the catalog from the
account payload. The phones' offline fallback catalogs carry the same row
(the iOS and Android `PingSettings` models), and every client's icon and label maps know the new kind while still
rendering inbox rows filed under the two retired ones. The desktop keeps its own copy in `desktop/src/PingSettings.h`;
older desktops without the row still receive and show the Pings (their
`organization_task_` prefix check matches the new kind) and can mute them from
the web.

## Operations

Each task ping writes one line to the app journal, and each APNs fan-out
another:

```
task ping: task 1a2b3c4d action updated by fmnext to jett,alice filed 2
apns: topic pings delivered 1 of 1
```

Read them on a core with `journalctl -u forkmesh-app | grep -E "task ping|apns:"`.

## History

Built on ForkMesh Next (`feature/task-pings`, never merged there) on
2026-09-24 and ported to production main on 2026-09-27. The port added pings
for the task fields production has and Next did not: routing, test steps,
parent task and work items.
