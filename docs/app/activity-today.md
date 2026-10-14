# Today's activity across devices

**Today's activity** is a heat grid of a member's interactions in 288
five-minute slots, shown on post-merge pages (desktop, iOS, Android, web). Each
device shares its counts through the App so the chart covers every device.

## Route

`POST /api/accounts/activity-today` (`app/src/account_activity.py`), session
required. Body: `day` (device-local `YYYY-MM-DD`, within one day of server UTC),
`device` (persistent random id, 8–64 of `A-Za-z0-9._:-`), `kind`
(`desktop`/`ios`/`android`/`web`), optional `counts` (288 integers, clamped to
100000; absent means read only). Answer: `{"ok", "nodeName", "day", "others",
"counts", "sources"}`; clients draw local counts plus `others` and drop answers
for another `nodeName`.

## Storage

`account_activity_days`, one row per `(account_bi, day, device_key)`. At most
24 devices per day (`429 too_many_activity_devices`); rows older than three days
are pruned; rows are deleted with the account and re-keyed by
`tools/next_rekey.py` ([REKEY.md](../REKEY.md)).

## Sync

Clients send every 2 minutes in the foreground when something new was counted
(else read at most every 10 minutes), always send after launch or sign-in, read
on opening a chart, and drop other devices' counts on sign-out. Code:
`MainWindow::syncUserActivity` (desktop), `UserActivitySync` (Android, iOS),
`syncUserActivity` in `06-repo-content.js` (web, counting only trusted input
events, shared across tabs via `localStorage`). Tests:
`app/tests/test_account_activity.py`, `app/tests/test_dashboard_user_activity.py`.
