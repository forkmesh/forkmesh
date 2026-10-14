# Task email

Every task activity that files a [Tasks Ping](task-pings.md) is also emailed
(mail kind `task`). Opens, clicks and views are written to the task timeline and
the private [#email-log](../MAIL_LOG.md) channel.

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
