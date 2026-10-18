# Desktop pings on the phone

When a phone is linked to a desktop, every ping that desktop raises also
reaches the phone as a push notification: an agent finishing, waiting or
needing approval, a workflow run, a usage window resetting, a planned restart,
and the rest of the desktop's Pings page.

## Code map

| Piece | Where |
| --- | --- |
| Which pings go, wire shape, phone-link rule, dedupe and storm gate | `desktop/src/DesktopPingForward.{h,cpp}`, `desktop/tests/test_desktop_ping_forward.cpp` |
| Sending (signing, POST, on/off switch) | `desktop/src/MainWindowPingForward.cpp`; hook in `MainWindow::recordNotification` (`MainWindowActions.cpp`) |
| Route | `app/src/desktop_pings.py` (`POST` and `DELETE /api/notifications/desktop`); tests `app/tests/test_desktop_pings.py` |
| Ping kind | `desktop_ping` in `NOTIFICATION_KINDS`, `devices` group of `PING_KIND_CATALOG` ("Pings from your desktop") |
| Banner | `_apns_ping_alert` (`entry.py`), `apns.alert_payload` (adds `aps.category` and top-level `sessionId`) |
| Power alarm | `desktop/src/DesktopPowerAlert.{h,cpp}`, `PowerSourceReader.{h,cpp}`, `MainWindowPowerAlert.cpp`; relay `DESKTOP_PING_ALARMS`, `apns.APNS_ALARM_SOUNDS`; iOS `DesktopAlarm.swift`; Android `PingAlarm.kt`, `PingNotifier` |
| One-tap fixes | [PING_QUICK_FIXES.md](PING_QUICK_FIXES.md); the route adds the desktop's `pingId` to `meta` |
| Chat on the phone's Pings | `ChatHub.filePing`, `LocalPingLog.recordChat` (iOS `FMApi/Inbox/InboxStores.swift`, Android `inbox/Inbox.kt`) |
| Desktop ping detail pane | `showPingDetail`, `renderPingDetail`, `refreshPingDetail`, `markWebAlertRead` (`MainWindowActions.cpp`) |
| Tapping the banner | iOS `ApplePushBridge` → `PushOpen.agent` → `HomeShellModel.routePush`; Android `PushTarget.forPing` → `PushFeature.open` → `AgentsFeature.agentScreen` |

## Architecture

A backgrounded phone app has closed its desktop-control socket, so a desktop
ping becomes an account ping (as timer expiries do, `timer_pings.py`). The
desktop POSTs it to the relay, `enqueue_notification` files a `desktop_ping`
row in the account inbox, and the `pings` topic carries it to iOS over APNs and
to Android over the account event socket its background service holds.

### Which pings go

Every ping filed through `recordNotification`: rows that announce themselves
(`addNotification`, action pings, usage reset, planned restarts), a
`flashPingMessage` ping whose kind is enabled (`pingKindEnabled`), and OS
notifications from `postNotification`. Plain `flashMessage` feedback is never
forwarded.

Skipped: chat, DMs and mentions (the relay pushes those), saved-search results
(stay local), log-stream errors, dialogs, local-operation feedback, timers and
alarms, relay alerts echoed back (`ClientErrorReports::echoesRelayAlert`), and
`desktop_ping` itself.

### When a phone counts as linked

`LocalControlServer::pairedDevices()` keeps each approved phone with its
`lastSeenMs` (browser sessions are not stored). A ping is forwarded while at
least one paired phone of this account is connected or connected within 30 days
(`kPhoneRecentMs`). Settings > Pings > "Send this desktop's pings to my phone"
(`notifications/phonePings`, default on) is the main switch. On the phone the
account's Pings settings apply (the "Pings from your desktop" mute, pause and
quiet hours). The desktop's own pause and quiet hours do not hold the phone.

### Already on screen

A ping is not forwarded while the desktop window is active and shows it: an
agent ping while the agent list is visible, any ping while the Pings page is
open ([PUSH_ON_SCREEN.md](PUSH_ON_SCREEN.md)). Power alarms always go.

### Duplicates and storms

`pingforward::Gate` drops a ping that repeats one forwarded in the last 15
seconds (same agent session, or one ping's first 48 letters and digits inside
the other's), and caps forwarding at 30 pings per 10 minutes. The relay's
dedupe key is `desktop:<node>:<pingId>`, so a retried POST rewrites its row.

### Agent-done text

Title `Agent #<id>`, body the agent's description (`AgentSession::issueTitle`,
else its prompt). `_push_alert_item` leaves the subtitle empty for an
`agent-done` `desktop_ping`. The repository is still sent for `threadId`
grouping and the `href`.

### Coming online

`pingforward::cameOnline` is true for the first accepted signed heartbeat after
launch and for a beat 10 minutes (`kOnlineGapMs`) or more after the previous
one. `announceDesktopOnline` then forwards a `desktop-online` ping ("<node> is
online", body "ForkMesh started" or "Back online after <span> offline.").
Setting: "Tell my phone when this desktop comes online"
(`notifications/phoneOnlinePing`, default on, disabled while the main switch is
off). The relay files it as `desktop_ping` with `meta.desktopKind`
`desktop-online`.

### Power alarm

The desktop alarms the phone when it switches from mains to battery (laptop
unplugged, UPS took over).

- Power source read every 5 s (`kPollMs`), all local: Linux
  `/sys/class/power_supply` plus UPower `OnBattery` (UPower wins, it sees USB
  UPS units; `scope=Device` batteries are ignored), macOS
  `IOPSGetProvidingPowerSourceType`, Windows `GetSystemPowerStatus`.
- `diag.health` answers `power: {onBattery, percent}` (`percent` -1 when
  unknown). Phone Desktop screens show a Battery bar only while on battery,
  orange at 40%, red at 20%.
- `poweralert::Watch` rings only on a mains-to-battery switch that lasts 15 s
  (`kConfirmMs`), once per outage, and files a quiet "<node> is back on mains
  power" when mains returns. Kinds `power-battery` and `power-restored`.
  Setting: "Ring an alarm on my phone when this desktop loses power"
  (`notifications/phonePowerAlarm`, default on).
- The relay maps `power-battery` to alarm `power` (`DESKTOP_PING_ALARMS`),
  stores `meta.alarm` and files with `force=True`, bypassing pause, quiet hours
  and mutes. APNs gets `aps.sound` `desktop_power_alarm.wav`,
  `interruption-level` `time-sensitive` and a top-level `alarm`.
- iOS: `Resources/Sounds/desktop_power_alarm.wav` (12 s, 8 kHz mono). In the
  foreground `DesktopAlarm` loops the tone under a full-screen "Stop alarm"
  cover for up to 5 minutes.
- Android: `pingAlarm` passes pause, quiet hours and mutes; `PingNotifier` posts
  on the high-importance `desktop-alarms` channel as `CATEGORY_ALARM` with
  `FLAG_INSISTENT`.

### Chat on the phone's Pings

Phones file a local `chat` row for each live message from someone else in a
public channel (relay rooms, public office rooms) through `ChatHub.filePing`.
Private conversations (`@` and `direct:` DMs, relay private channels, private
office rooms: Android `ChatHub.isPrivateRoom`) are never filed: the ping log is
plain preferences, so the decrypted text would outlive disappearing-message
expiry, deletion and sign-out. On Android `ChatHub.purgePrivatePings` removes
such rows already stored when the log is attached, a private channel is joined
or office rooms change. Own messages, messages older than 10 minutes and
duplicates by message id are skipped. Rows live only in `LocalPingLog` (`inbox/local-pings.v1`, 50 rows),
open the conversation, and are marked read when the conversation is open. They
are not account inbox rows, because the relay already pushes chat banners.

### The desktop's own inbox

Rows whose `meta.node` is this desktop's `machineNodeName()` are dropped from
the desktop's website-pings list and unread count (`pingforward::isOwnEcho`).

### Deleting a ping

- `forwardPingToPhones` returns `<timestampMs>-<row id>`, kept as `phonePingId`
  and saved in the notification journal (row ids change per start).
- Delete collects the `phonePingId`s and sends
  `DELETE /api/notifications/desktop` in batches of 50 (`kDeleteBatch`). A
  failure is logged; the desktop row stays deleted.
- Clear empties the whole account inbox (`DELETE /api/notifications` with
  `all`).
- Both delete routes send the `pings` account event with no record, so clients
  re-read the inbox without a banner.
- iOS re-inserts pings whose banners are still in Notification Center for ten
  minutes from the ping's own time (`PingSnapshot.insert`).
- Banners already in the notification shade are not removed, and deletes on a
  phone or the website do not reach the desktop.

### Desktop Pings page interaction

- Only Link (`openNotificationRow`) and Repository
  (`openNotificationRepository`) open something on a single click; they carry
  `kPingOpensRole`. Any other cell opens the detail pane; double-click opens
  the destination.
- The detail pane (`m_pingDetailPane`, a `QScrollArea` in a splitter) shows the
  full text (`kPingFullTextRole`), an Open button (`pingOpenLabel`), actions
  (Send to prompt, Copy text, Copy link, Open repository, Quick fixes, Ping
  settings, Delete ping) and details. It follows its ping by key
  (`notificationRowKey`: `local:<id>`, `web:<id>`, `run:<id>`), redraws only
  when its signature changes, and closes when the ping is gone. Esc or x closes
  it; widgets are replaced with `deleteLater`.
- Opening a website ping marks it read (`POST /api/notifications` with `ids`,
  signed with `kAccountAlertReadProof`).

## Protocol

`POST /api/notifications/desktop`, JSON body:

| Field | Rule |
| --- | --- |
| `pingId` | `[A-Za-z0-9_.:-]{1,64}`; the desktop sends `<timestampMs>-<row id>` |
| `node` | `[A-Za-z0-9_.-]{1,64}`, lowercased by the relay |
| `kind` | the desktop's kind, `[A-Za-z0-9_-]{0,40}` |
| `category` | `""`, `agent-finished`, `agent-failed` or `ci-failed` |
| `sessionId` | digits, at most 12 |
| `runId` | optional; a desktop Actions run id, 1 to 12 digits, no leading zero |
| `repo` | `""` or `owner/name` |
| `warning` | JSON boolean |
| `title`, `body` | at most 160 and 500 characters; not both empty; refused rather than trimmed |

Authorised by the account session, or by `user`/`ts`/`sig` over

```
forkmesh-account-desktop-ping-v1
<user>
<pingId>\n<node>\n<kind>\n<category>\n<sessionId>\n[<runId>\n]<repo>\n<0|1>\n<title>\n<body>
<ts>
```

The `runId` line is there only when the ping names a run, so a desktop that
sends no run signs the same proof as before. Deploy the relay before the
desktop: an older relay refuses `ci-failed` and does not sign over `runId`.

signed by the account key (`signOrgTaskProof`), so a captured proof announces
exactly one ping into the signer's own inbox.

`DELETE /api/notifications/desktop` takes `{"node": "<node>", "pingIds": [...]}`
(1 to 50 ids) and is authorised by the session or a signature over

```
forkmesh-account-desktop-ping-delete-v1
<user>
<node>
<pingId>        (one line per id, in body order)
<ts>
```

The row's `meta` holds `node`, `desktopKind`, `warning`, and `category`,
`sessionId` and `runId` when set. `href` is `/<repo>/agents/<sessionId>` for an
agent and `/<repo>/actions/runs/<runId>` for an Actions run, so every surface
that follows the href lands on the record, not the repository.

Categories: an agent ping that is not a warning is `agent-finished` (Open,
Continue, Merge & clean up); a warning about an agent is `agent-failed` (Open,
Retry, Send to agent). A failed Actions run is `ci-failed` (Open log, Send to
agent); a run that did not fail carries its `runId` with no category.

### Tapping the banner

A tap on an agent ping opens that agent's session, not the `href` (a desktop
session is not in the relay's agent list).

- iOS: `ApplePushBridge.receivedTap` passes `sessionId` and the category;
  `PushOpen.agent` resolves the session; `HomeShellModel.routePush` marks the
  row read and opens `.agent(id:)` (`AgentRouteDestination.rowId` resolves
  `desktop:<id>`).
- Android: `PushTarget.forPing` carries the agent id in the
  `com.forkmesh.push.agent` extra; `PushFeature.open` marks the row read and
  opens `AgentRouteScreen`, which resolves `desktop:<n>` for a number and
  `node:<id>` otherwise.

A tap on an Actions ping opens that run. The APNs payload carries `runId`
next to `href`; the iOS `NotificationDeepLink` keeps `actions/runs/<id>` as
its `reference` and `InboxRoutes` turns it into `.run`. Android's
`NotificationDeepLink.run` routes to `Route.Run`, which opens
`ActionRunScreen` (loading the runs itself when opened on its own) instead of
the Actions list. The web Actions tab opens the run named by
`/<owner>/<repo>/actions/runs/<id>`.

### Every banner lands on its record

`_push_alert_item` and the digest email keep the stored href's tail under the
repository (`_notification_stored_tail`) for every kind, not only code pushes,
rewriting just the `owner/repo` prefix to the public identity. Before this a
ping with a repository and no record number (an agent, an Actions run, a
branch) opened the repository's front page.

## Traps

- The time-sensitive level only breaks through Focus with the
  `com.apple.developer.usernotifications.time-sensitive` entitlement, which
  `Config/ForkMesh-*.entitlements` does not carry yet. Add it together with the
  App ID capability, or signing fails.
- `/api/notifications/agent-approval` has no route on the served stack, so
  approvals arrive as a plain `agent-finished` ping without Approve and Deny
  buttons.
- The desktop cuts title and body to fit before signing (`fitText`), counting
  UTF-16 units, never fewer than Python's code points.
