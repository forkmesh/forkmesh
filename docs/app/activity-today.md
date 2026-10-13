# Today's activity across devices (2026-10-06)

The post-merge pages on the desktop, iOS, Android and (new) the web
dashboard show **Today's activity**, a heat grid of the member's interactions
in 288 five-minute slots. Each device used to count only its own input, so a
morning on the phone and an afternoon at the desktop showed as two half-empty
grids. Each device now shares its counts through the App, and every chart
shows the whole day from every device the member uses.

## Route

`POST /api/accounts/activity-today` (`app/src/account_activity.py`, reached
through `accounts_handler` like `user-status`). It takes a signed-in session
(cookie, Bearer or body `sessionToken`).

| Field | Meaning |
|---|---|
| `day` | The device's local day, `YYYY-MM-DD`. It must be within one day of the server's UTC day, so a device with a wrong clock cannot write far-off days. |
| `device` | A random id the device generates once and keeps: 8–64 of `A-Z a-z 0-9 . _ : -`. |
| `kind` | `desktop`, `ios`, `android` or `web`. Used only for the "On this desktop, iOS device and browser" line. |
| `counts` | Optional: exactly 288 non-negative integers, each clamped to 100000. When present, it replaces this device's row for the day. When absent, the call only reads. |

The answer has the shape `{"ok", "nodeName", "day", "others", "counts",
"sources"}`:

- `others` is the slot-by-slot sum of every *other* device's counts for that
  day, and `counts` adds this device's stored row to it.
- `sources` lists one `{kind, total, updatedAt, self}` per device.

A client draws its own live local counts plus `others`, so its own new input
shows at once without waiting for the next send. As with chat retention,
clients drop an answer whose `nodeName` is not the account they asked for.

## Storage

Table `account_activity_days` (migration 0235) holds one row per
`(account_bi, day, device_key)`: the kind, the 288 counts as a comma-separated
string, and `updated_at`.

- **Limits.** One account may write at most 24 devices a day; the 25th gets
  `429 too_many_activity_devices`.
- **Pruning.** When a device writes its first row of a day, rows older than
  three days (by the server's UTC day) are deleted.
- **Account deletion.** Deleting the account deletes its rows.
- **Re-key.** `tools/next_rekey.py` recomputes `account_bi`
  ([REKEY.md](../REKEY.md)).

## Days and time zones

The day is the device's local day, and the slots are its local five-minute
slots. Devices in the same time zone line up exactly. A device in another
time zone adds its counts under its own local day and slot, so its hours
appear shifted. That case was judged too rare for the cost of re-binning on
the server.

## When clients sync

All four clients follow the same rules:

- **Every 2 minutes** while the app or tab is in the foreground, a client
  sends its counts if anything new was counted since its last successful
  send. Otherwise it only reads, and at most every 10 minutes.
- **On first use after launch or sign-in**, a client always sends its counts.
- **On opening a chart**, a client asks for a read, at most every 10 seconds.
  The desktop does this from the post-merge page and the Git leaderboard, and
  the phones from the merged report. The web card does it at most once a
  minute, because the agent detail it sits in redraws often.
- **On sign-out or an account switch**, other devices' counts are dropped.

Each client's code:

| Client | Where |
|---|---|
| Desktop | `MainWindow::syncUserActivity` / `startUserActivitySync` (`MainWindowAccounts.cpp`). It starts from `activateAccountSession`, and the merged-page renderers call it. `UserActivityLog` keeps the device key in `user-activity-today.json` and holds the other devices' counts in memory. |
| Android | `UserActivitySync` (core `insights/`), run by `FmUserActivity.startSync` from `MainActivity` while `AppLifecycle.foreground` is true. The device key is in prefs under `user_activity_device`. |
| iOS | `UserActivitySync` (`FMBase`), run by `FmUserActivity` while `AppLifecycle` is resumed. The device key is in the key-value store under `user_activity_device`. |
| Web | `syncUserActivity` in `dashboard/js/06-repo-content.js`. Counts are kept in `localStorage` (`forkmesh.userActivityToday`) and updated by every tab, so tabs of one browser share one device row. |

The web dashboard now counts input too: pointer presses, key presses (not
auto-repeat) and the start of each wheel scroll (a new scroll after a
400 ms pause). Only trusted (real) events count. The card appears on a merged
agent session's detail page, under the Actions block.

Tests: `app/tests/test_account_activity.py` (the route against SQLite),
`app/tests/test_dashboard_user_activity.py` (the web log, sync and card),
`forkmesh-user-activity-tests` (desktop), `UserActivitySyncTest` and
`UserActivityTest` (Android core), `UserActivitySyncTests` and
`UserActivityTests` (FMBaseTests).
