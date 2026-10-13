# Task email

Since 2026-10-07 every organization task activity is also sent as an email,
and what each recipient does with that email (opens it, clicks a link, opens
the task from it) is written to the task's activity timeline and to the
private [#email-log](../MAIL_LOG.md) channel. The operator asked for it in
these words: "when a new task is created it should send an email about it and
also send emails about all task activity, log that activity to the task as
well, clicks, opens and views should all be logged, make sure we track all of
the emails we send and update activity to the email-log channel".

## Code map

| Piece | Where |
|---|---|
| Recipients, message text, sending, timeline entries for opens, clicks and views | `world/src/world_task_email.py` (lazy module, `entry.task_email`) |
| Send records, open/click/view counters, #email-log notes, view tokens | `app/src/mail_activity.py`, shared by every email kind ([MAIL_LOG.md](../MAIL_LOG.md), "Every email is tracked") |
| Send hook | `notify_organization_task_update` in `world/src/world_office_routes.py`, after the Tasks Ping |
| Open and click events | `route_handlers.process_mail_event` → `mail_activity.tracked` → `task_email.mail_event` (Postfix pixel and click redirect through `mail_tracking_handler`, or a Mailtrap webhook) |
| Task views | `GET /api/tasks/<id>?mail=<token>` → `_observe_email_view` in `world/src/world_office_tasks.py` → `_OfficeMarketingTasksRuntime.task_email_viewed` → `task_email.viewed` → `mail_activity.viewed` |
| Web | `app/public/dashboard/js/09-tasks.js`: `takeTaskEmailToken` passes the token once, then removes it from the address bar; timeline labels in `TASK_EVENT_STYLES` |
| Desktop | `taskEventLabel` in `desktop/src/MainWindowTasks.cpp` |
| Table | `email_sends` (kind `task`, `ref` = `task:<org_bi>:<task_id>`), migration `0247_email_sends.sql`. Migration `0246_task_email_sends.sql` widened the `organization_task_events.kind` CHECK and created the task-only `task_email_sends`, which 0247 folded into `email_sends` |
| Tests | `world/tests/test_task_email.py`, `world/tests/test_task_pings.py`, `app/tests/test_mail_activity.py`, `app/tests/test_mail_record_retention.py`, `app/tests/test_mail_tracking_kinds.py` |

## Who gets the email

Every change that files a Tasks Ping ([task-pings.md](task-pings.md),
"Events") also sends email. The recipients are the Ping recipients (the
creator, the assignee, the previous assignee, everyone who commented or
checked in, and the organization owners), with two differences:

- On **create**, the person who created the task is emailed too, so a new task
  always produces an email, even one you create and assign to yourself.
- Someone @mentioned in a comment gets the `mention` Ping instead of the Tasks
  Ping, but still gets the activity email.

The actor of any other change is never emailed about it. At most 10 people are
emailed per change (`MAX_RECIPIENTS`).

An account is skipped unless it is an active user account with a verified
address and email notifications on (`email_notifications`, the same switch
the badge email honours). The mail kind is `task`, which is in
`mail_tracking.UNSUBSCRIBE_KINDS`, so each email carries one-click
`List-Unsubscribe`, an open pixel and tracked links, and a recipient who
unsubscribed from notification mail gets none.

**Hourly cap.** One address gets at most 30 task emails an hour
(`HOURLY_CAP`, counted from `email_sends` rows of kind `task`). A board-wide edit would
otherwise use up the core Postfix limit (300 an hour per client) that
verification and password mail share. A held email is noted in #email-log as
`🗂️ Task activity · 🚫 suppressed` with the reason
"task email limit, 30 an hour per person" and the body the person would have
received.

Sending runs inside the task request, like badge mail: the Worker has no
detached tasks. A failed or refused send never fails the task change or its
Ping.

## The email

- Subject: the Ping title, `New task: <title>`, `Task updated: <title>` or
  `Task deleted: <title>`.
- Body: one line per change, the same sentences as the Ping
  (`Progress 40% → 60% by @jett`, `New comment by @carol: <first 80
  characters>`), then the organization line (department, team, priority,
  assignee, status, repository).
- **Open the task** links to `/dashboard/tasks/?task=<id>&mail=<token>`. A
  deleted task links to the task list without a token.
- Task descriptions are never copied into the email, as in Pings.

## Tracking

The send record, the counters, the #email-log notes and the view token are
the shared tracking every email gets ([MAIL_LOG.md](../MAIL_LOG.md), "Every
email is tracked"). Task email adds its `ref` (`task:<org_bi>:<task_id>`) and
the recipient's name, and is the `task` follower in
`mail_activity.FOLLOWERS`: it writes the timeline entries below.

| Event | How it is seen | Timeline kind | #email-log result |
|---|---|---|---|
| Sent | `send_activity`, one entry per change listing everyone emailed | `emailed` ("set off an email") | `accepted` (the normal send note) |
| Open | The open pixel (`/api/mail/t/o/<token>.gif`), or Mailtrap's `open` webhook | `email_opened` | `opened` |
| Click | Any tracked link (`/api/mail/t/c/<token>`), or Mailtrap's `click` webhook | `email_clicked` | `clicked` |
| View | The web Tasks page opened from the email's link with a valid `mail` token | `email_viewed` | `viewed` |

- **Timeline.** The first open, the first click and the first view of each
  email add one timeline entry, with the recipient (or, for a view, the
  signed-in person who opened it) as the actor and the subject as the summary.
  Repeats only raise the counters. The timeline keeps 250 events per task, and
  a pixel reloaded on every open would otherwise push the task's real history
  out.
- **#email-log.** Every open, click and view is a note: `Task activity ·
  opened · <time>` with To, Subject, `Detail: @bob opened the email (3
  times)` (a view reads `@bob opened the page linked in the email`) and the message id of the original send, so it lines up with the
  `accepted` note. The Postfix open and click events are already
  deduplicated per send per minute.
- **The view token** is the shared `mail_activity.view_token`
  (`blind_index("mail-view:<send id>")`; until the tracking became shared it
  was `task-email-view:`, and those older tokens no longer count a view). It
  is not the pixel and unsubscribe token, so a dashboard address bar never
  carries a token that can unsubscribe someone. The view counts only when the
  token and the row's `ref` (the task and the organization) match, and only for a signed-in member who can read
  the task. The web page drops the token from the address bar after using
  it, so a reload does not count again.

Automated link scanners and image proxies (Outlook Safe Links, Gmail's image
cache) can register an open or a click the person did not make; the log
records what the server saw.

## Fixed along the way: tracked links in every notification email

`mail_tracking.TOKEN_RE` accepted only a 36-character send id (a hyphenated
UUID), but `_ap_uuid` returns 32 hex characters. Every tracked link and open
pixel in a Postfix-sent notification email therefore answered "This link is
not valid." (404), and no open or click was ever recorded. The pattern now
accepts both shapes.
