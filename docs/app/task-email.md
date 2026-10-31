# Task email

Part of [the App docs](README.md). Every task activity that files a [Tasks Ping](task-pings.md) is also emailed
(mail kind `task`). Opens, clicks and views are written to the task timeline and
the private [#email-log](../operations/mail-log.md) channel.

Code: `world/src/world_task_email.py`; shared tracking in
`app/src/mail_activity.py`; records in `email_sends` (`ref` =
`task:<org_bi>:<task_id>`). Tests: `world/tests/test_task_email.py`,
`app/tests/test_mail_activity.py`.

## Recipients and limits

The Ping recipients plus the creator on create and @mentioned people; never the
actor otherwise. At most 10 recipients per change (`MAX_RECIPIENTS`) and 30 task
emails per address per hour (`HOURLY_CAP`). Only active, verified accounts with
`email_notifications` on. Each email has one-click `List-Unsubscribe`, an open
pixel and tracked links. A failed send never fails the task change. Task
descriptions are never copied.

## Deleted tasks

A delete skips the per-change email. `_delete` and `_delete_batch` call
`notify_organization_task_deletions` (`world/src/world_office_routes.py`) once
per request. It groups the deleted tasks per recipient (creator and person
assignee, members only, never the deleter), and `send_deleted` sends each one
email: `Task deleted: <title>` or `N tasks deleted`, a line per task with who
deleted it and the reason, a note that a deleted task counts as done and stays
restorable for 30 days, and a link to the Tasks list (no view token). The same
opt-outs, verification check and hourly cap apply; each task's timeline gets an
`emailed` entry. Commenters and owners still get the delete Ping.

## Tracking

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
