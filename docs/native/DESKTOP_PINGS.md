# Desktop pings on the phone

When a phone is linked to a desktop, every ping that desktop raises also
reaches the phone as a push notification: an agent finishing, waiting for a
reply or needing approval, a workflow run, a usage window resetting, a planned
restart, and the rest of what lands on the desktop's Pings page.

## Code map

| Piece | Where |
| --- | --- |
| Which pings go, the wire shape, the phone-link rule, the dedupe and storm gate | `desktop/src/DesktopPingForward.{h,cpp}`, `desktop/tests/test_desktop_ping_forward.cpp` |
| Sending (signing, the POST, the on/off switch) | `desktop/src/MainWindowPingForward.cpp`; the hook in `MainWindow::recordNotification` (`MainWindowActions.cpp`) |
| The route | `app/src/desktop_pings.py` (`POST` and `DELETE /api/notifications/desktop`), wired in `app_runtime.py` and `entry.py`; tests in `app/tests/test_desktop_pings.py` |
| The ping kind | `desktop_ping` in `NOTIFICATION_KINDS` and the `devices` group of `PING_KIND_CATALOG` ("Pings from your desktop") |
| The banner | `_apns_ping_alert` (`entry.py`) and `apns.alert_payload` add `aps.category` and a top-level `sessionId` |
| Phones | iOS `InboxRoutes.route` and Android `InboxRoutes.route` open the agent; Android `InboxFeature.payload` reads `meta.category` for the notification actions |
| The power alarm (desktop on battery) | `desktop/src/DesktopPowerAlert.{h,cpp}`, `PowerSourceReader.{h,cpp}`, `MainWindowPowerAlert.cpp`; relay `DESKTOP_PING_ALARMS` and `apns.APNS_ALARM_SOUNDS`; iOS `DesktopAlarm.swift`; Android `PingAlarm.kt`, `PingNotifier` (see "Power alarm") |
| One-tap fixes on a ping | [PING_QUICK_FIXES.md](PING_QUICK_FIXES.md); the route adds the desktop's `pingId` to `meta` |
| Chat on the phone's Pings | iOS `ChatHub.filePing` (`FMChat`), `LocalPingLog.recordChat` (`FMApi/Inbox/InboxStores.swift`); Android `ChatHub.filePing`, `LocalPingLog.recordChat` (`inbox/Inbox.kt`), wired in `InboxFeature.install`; tests `ChatPingsTests.swift`, `ChatPingsTest.kt` (see "Chat on the phone's Pings") |
| The desktop's ping detail pane | `showPingDetail`, `renderPingDetail`, `refreshPingDetail`, `markWebAlertRead` (`desktop/src/MainWindowActions.cpp`); window test in `desktop/tests/test_window_resize.cpp` (see "The ping detail pane") |
| Tapping the banner | iOS `ApplePushBridge` → `PushOpen.agent` → `HomeShellModel.routePush` (`.agent`); Android `PushTarget.forPing` → `PushFeature.open` → `AgentsFeature.agentScreen` (`AgentRouteScreen`) |

## Architecture

The desktop has no push channel of its own to the phone: a phone app in the
background has closed its desktop-control socket. So a desktop ping becomes an
account ping, exactly as timer expiries do (`timer_pings.py`). The desktop
POSTs it to the relay, `enqueue_notification` files a `desktop_ping` row in
the account's inbox, and the `pings` topic carries it to iOS over APNs and to
Android over the account event socket its background service keeps open.

### Which pings go

Every row the desktop files through `recordNotification` that is a ping:

- a row that announces itself (`addNotification`, action pings, the usage
  reset, planned restarts);
- a `flashPingMessage` ping whose kind is switched on (`pingKindEnabled`);
  plain `flashMessage` feedback ("Copied", "Saved") is filed on the Pings
  page but never forwarded;
- an OS notification filed by `postNotification` (for example an agent that
  finished while the window was in the background).

Skipped, because the relay already pushes them or they must stay local: chat,
DMs and mentions (the relay pushes those itself), saved-search results (a
search stays on the machine), log-stream errors, dialogs, local-operation
feedback, timers and alarms (`timer_pings.py` sends those), relay alerts
echoed back (`ClientErrorReports::echoesRelayAlert`), and `desktop_ping`
itself.

### Chat on the phone's Pings

The desktop files a `chat` row on its own Pings page for every live message
someone else sends (`MainWindow::onMessage`), and keeps it there. The phones
do the same on their own Pings list (the Inbox), so a chat or DM received on
the phone shows up in Pings as it does on the desktop:

- relay rooms and relay DMs (`RelayService` / `RelayRoom` `onMessage`),
  sealed direct messages (`DirectMessageService.onMessage`) and live
  messages in office rooms (`OfficeChannelService.onMessage`) all go to
  `ChatHub.filePing`;
- the phone's own messages, messages older than 10 minutes (history replayed
  on connect) and messages already filed (by message id) are skipped;
- the title follows the desktop: "Pat sent you a message" for a DM,
  "Pat in #room" for a room, and "New user joined" with "Pat said hello in
  #welcome" for `#welcome`; a file reads "📎 name" over the text;
- a message in the conversation already open on the phone is filed read,
  and opening a conversation (`ChatHub.select`) marks its chat pings read.

The rows live on the phone only, in the same local log as "On this phone"
failure rows (`LocalPingLog`, `inbox/local-pings.v1`, 50 rows), with
`kind: "chat"` and the conversation. They show a chat icon and the label
"Chat", and tapping one opens the conversation (`Route.channel`). They are
not account inbox rows: the relay already pushes chat and DMs as banners,
and an inbox row would reach the desktop as a second copy of the ping it
files itself.

### Coming online

The desktop also tells the phone when it comes online, though that is not a
row on its Pings page. `applyNodeHeartbeatResult` (`MainWindowSetup.cpp`)
keeps the time of the last accepted signed heartbeat. `pingforward::cameOnline`
is true for the first accepted beat after launch, and for a beat 10 minutes
(`kOnlineGapMs`) or more after the previous one. Beats run every minute, so a
gap that long means the desktop was asleep, in offline mode, set offline on
the Node page, or cut off from the relay. `announceDesktopOnline`
(`MainWindowPingForward.cpp`) then sends a ping of kind `desktop-online`
through `forwardPingToPhones`, so the phone-link rule, the main switch and
the gate all apply. The title is "<node> is online". The body says ForkMesh
started, or "Back online after <span> offline." (`onlineSource`,
`offlineSpan`).

The ping waits for an accepted heartbeat, not for the window to open. At
launch the network may not be up, and the POST needs the account key and a
reachable relay anyway. Settings > Pings > "Tell my phone when this desktop
comes online" (`notifications/phoneOnlinePing`, default on) turns off just this
ping. It is greyed out while the main phone switch is off. On the relay the
ping is filed as `desktop_ping` (`meta.desktopKind` `desktop-online`), so
"Pings from your desktop" mutes it on the phone, not "Host came online". That
kind stays the relay's own per-repository host presence.

### Power alarm

The desktop also alarms the phone when it loses mains power and runs on
battery: a laptop unplugged, or a desktop whose UPS took over after a power
cut. Code: `desktop/src/DesktopPowerAlert.{h,cpp}` (rules and text, tested by
`desktop/tests/test_desktop_power_alert.cpp`), `PowerSourceReader.{h,cpp}`
(the platform read) and `MainWindowPowerAlert.cpp` (the watch).

- Reading the power source. Linux reads `/sys/class/power_supply` and asks
  UPower's `OnBattery` over the system bus. UPower wins when it answers,
  because it also sees a USB UPS, which has no sysfs entry. A battery whose
  `scope` is `Device` (a mouse, a phone on a charging cable) is ignored.
  macOS uses `IOPSGetProvidingPowerSourceType` (battery and UPS both count),
  Windows `GetSystemPowerStatus`. Each is a local read every 5 seconds
  (`kPollMs`). No network request is made, so this is not display polling.
- The battery gauge. The watch keeps its latest reading, and `diag.health`
  answers with `power: {onBattery, percent}` (`percent` is -1 when the
  platform gives none; the key is left out while the source is unknown).
  `MainWindow::currentPowerReading()` reads once directly when the watch
  has not started yet. The phones' Desktop screens (iOS
  `DesktopHealthGauges`, Android `HealthGauges`) add a Battery bar under
  CPU, Memory and Disk only while `onBattery` is true; it turns orange at
  40% and red at 20%, the reverse of the other bars. macOS reads the level
  from the internal battery's current and maximum capacity. Asked for on
  2026-10-03 ("show the battery level ... on mobile where the desktop stats
  are"); the desktop's own top bar shows nothing.
- When it rings. `poweralert::Watch` only alarms on a switch from mains to
  battery, so starting ForkMesh on battery never rings. Battery must last 15
  seconds (`kConfirmMs`), which keeps a UPS self-test or a brownout quiet.
  It rings once per outage. When mains comes back it files a quiet second
  ping, "<node> is back on mains power", with how long the outage lasted.
- The pings. Both are ordinary desktop pings, with kinds `power-battery` (a
  warning) and `power-restored`. They land on the desktop's Pings page and
  go to the phone through `forwardPingToPhones`, so the phone-link rule, the
  main switch and the gate all apply. Settings > Pings > "Ring an alarm on
  my phone when this desktop loses power" (`notifications/phonePowerAlarm`,
  default on) keeps both off the phone.
- The relay. `desktop_pings.py` maps the desktop kind to an alarm
  (`DESKTOP_PING_ALARMS`, `power-battery` to `power`). It stores
  `meta.alarm` and files the row with `force=True`, so the account's pause,
  quiet hours and the "Pings from your desktop" mute do not hold it, just as
  for intrusion alerts. A power cut at night is the case the alarm is for.
  `_apns_ping_alert` copies the alarm into the alert. `apns.alert_payload`
  then sets `aps.sound` to `desktop_power_alarm.wav` and
  `aps.interruption-level` to `time-sensitive`, and adds a top-level `alarm`.
  The banner icon is the low battery (`push_icons.PUSH_DESKTOP_ALARM_ICONS`).
- iOS. The sound is `native/ios/ForkMesh/Resources/Sounds/desktop_power_alarm.wav`,
  12 seconds of two-tone beeps (8 kHz mono PCM; APNs plays at most 30
  seconds). The synchronized `ForkMesh` group bundles it, so nothing in the
  project file names it. `PushOpen.alarm` carries the alarm. If the push
  arrives while the app is open, `ApplePushBridge` shows the banner with no
  sound and `DesktopAlarm` rings instead: the same tone, looped, under a
  red full-screen "Stop alarm" cover (`HomeShell`). It stops when tapped or
  after 5 minutes.
- Android. `pingAlarm` (core `push/PingAlarm.kt`) recognises the row. The
  ledger lets it through pause, quiet hours and mutes, as it does reminders.
  `PingNotifier` posts it on the `desktop-alarms` channel, which has high
  importance, the system alarm sound with alarm audio usage, and DND bypass
  where the user allows it. It is posted as `CATEGORY_ALARM` with
  `FLAG_INSISTENT`, so the sound repeats until the notification is opened or
  dismissed.

### When a phone counts as linked

`LocalControlServer::pairedDevices()` keeps each approved phone with its
`lastSeenMs`; browser sessions (`platform == "web"`) are never stored. A ping
is forwarded while at least one paired phone of this account (its `user` is
the account owner, or unknown) is connected now or connected within the last
30 days (`kPhoneRecentMs`). Pairing a phone turns forwarding on; forgetting it
on the desktop, or a month without the phone connecting, turns it off.
Settings > Pings > "Send this desktop's pings to my phone" turns it off
outright (`notifications/phonePings`, default on). On the phone side the
account's own Pings settings apply: switching off "Pings from your desktop"
mutes the kind, and the account's pause and quiet hours hold the banner as
for any ping. The desktop's own pause and quiet hours are local to the
desktop and do not hold the phone.

### Duplicates and storms

One event can file two rows: a new issue files a `flashPingMessage` ping and
an `addNotification` row, and an agent finishing files its ping and, with the
window in the background, an OS notification. `pingforward::Gate` drops a
ping that repeats a ping forwarded in the last 15 seconds: the same agent
session, or text where one ping's first 48 letters and digits appear in the
other's. It also caps forwarding at 30 pings per 10 minutes, so a failure
loop cannot flood the phone. The relay's dedupe key is
`desktop:<node>:<timestampMs>-<row id>`, so a retried POST rewrites its row.

### What an agent-done push says

An agent finishing reaches the phone as the title `Agent #<id>` and, as the
body, the agent's description: its title in the Agents list
(`AgentSession::issueTitle`), or its prompt when it has none. Nothing else:
no repository, duration, cost, model or closing summary, which read as noise
on a lock screen. `MainWindow::forwardPingToPhones` writes the two fields,
and `_push_alert_item` leaves the subtitle empty for a `desktop_ping` whose
`meta.desktopKind` is `agent-done`, so the repository does not come back as
the banner's second line. The repository is still sent: it groups the banners
(`threadId`) and builds the `href`.

### The desktop's own inbox

The desktop reads the same account inbox for its website-pings list. Rows
whose `meta.node` is this desktop's `machineNodeName()` are dropped from that
list and from its unread count (`pingforward::isOwnEcho`), so the desktop does
not show its own pings twice. Another desktop on the same account does show
them there as website pings; the desktop only announces error groups,
operational alerts and task pings from that list (a sound and, for a warning,
the red border; no card), so they never pop up.

### Deleting a ping

Deleting pings on the desktop's Pings page deletes the copies it forwarded,
so they leave the website and the phones too. Asked for on 2026-10-06 ("when
we clear / delete pings on desktop also delete them from mobile / web so they
are synced").

- Each forwarded row remembers the id it went out under:
  `forwardPingToPhones` returns `<timestampMs>-<row id>` and
  `recordNotification` keeps it as `phonePingId`. The notification journal
  saves it, because row ids are handed out again on every start and the id
  could not be rebuilt after a restart. A row that was never forwarded has
  none, so deleting it sends nothing.
- Delete (`deleteSelectedNotifications`) collects the `phonePingId` of every
  deleted desktop row and calls `deleteForwardedPings`. That sends
  `DELETE /api/notifications/desktop` in batches of 50 (`kDeleteBatch`),
  signed like the POST (`signPingForwardRequest`). A failure is logged and
  the desktop row stays deleted.
- Clear already empties the whole account inbox (`clearWebAlerts`, `DELETE
  /api/notifications` with `all`), forwarded pings included, so it sends
  nothing extra.
- The relay recomputes each row's dedupe key, `desktop:<node>:<pingId>`, and
  deletes those rows from the signer's own inbox only. Both this route and
  `DELETE /api/notifications` then send the `pings` account event with no
  record. That makes the website, the Android inbox and the iOS badge re-read
  the inbox at once, and no banner goes out.
- iOS used to bring a deleted ping back. On every resume it re-inserts the
  pings whose banners are still in Notification Center, and an inserted row
  outlived refreshes for ten minutes from the insert. The ten minutes now run
  from the ping's own time (`PingSnapshot.insert`).
- Not synced yet: a banner already in the phone's notification shade stays
  there. Android cancels a banner only when its row comes back read
  (`PingAlertLedger.decide`), and iOS removes delivered ping banners nowhere.
  Deleting a ping on a phone or the website does not delete it on the
  desktop either, which drops its own echoes from the inbox it reads.

### Which cells of a row open something

On the desktop's Pings page only Link and Repository jump somewhere on a
single click. Asked for on 2026-10-07 ("the whole ping row is not clickable
only the parts that make sense to click"): clicking the Detail or When of a
row to select it used to jump to the ping's issue, run or chat.

- Link opens the ping's destination (`openNotificationRow`) when it has one:
  an action run, a `NotificationLink`, or a website `href`. It is underlined
  and carries `kPingOpensRole`.
- Repository opens that repository (`openNotificationRepository`) when it is
  on this node, and also carries `kPingOpensRole`.
- Any other cell opens the ping's detail pane ("The ping detail pane").
  Double-clicking any cell other than Link and Repository opens the
  destination directly. Ctrl- and Shift-clicks only change the selection.
- The Settings and Prompt buttons, and Open in the right-click menu, work as
  before. The menu also has Show details.

### The ping detail pane

Clicking a ping opens a pane on the right of the Pings page with the whole
ping and what can be done with it, as tapping a ping does on iOS and Android
(`PingDetailScreen`). Asked for on 2026-10-07 ("allow for clicking on a ping
on desktop to open the detail window on the right with more information and
action like we have on mobile").

- The pane (`m_pingDetailPane`, a `QScrollArea` in a splitter beside the
  grid) is built in `buildNotificationsSection` and stays hidden until a row
  is clicked. `showPingDetail` opens it and `renderPingDetail` fills it from
  the row's cells: the full text (`kPingFullTextRole`), never the clipped
  preview, and the Type cell's run id, `NotificationLink`, `href`, local id
  and website id.
- It shows the Type icon and label, the whole title and body (selectable),
  an Open button named after the destination (`pingOpenLabel`, for example
  "Open issue" or "Open in browser") when there is one, then Actions: Send to
  prompt, Copy text, Copy link (website pings), Open repository (when it is
  on this node), Quick fixes (the same `addPingFixMenuActions` menu as the
  right-click menu, when the ping has any), Ping settings and Delete ping
  (desktop and website pings). Details lists Type, Kind, Repository, From,
  Status, When and Link, with the status reason underneath.
- The pane follows its ping by key, not by row number, because a sort or a
  refresh moves rows: `notificationRowKey` is `local:<id>`, `web:<id>` or
  `run:<id>`. `refreshNotificationsTable` calls `refreshPingDetail`, which
  selects the ping's row again and redraws the pane only when something it
  shows changed (`m_pingDetailSignature`). A ping that is gone (deleted, an
  approval that was answered, a website ping on another page) closes the
  pane. While the pane is open, the arrow keys move it with the selection.
- Opening a website ping marks it read, as the phones do: `markWebAlertRead`
  sets `readAt` locally, drops the unread count and posts
  `POST /api/notifications` with `ids`, signed with
  `kAccountAlertReadProof`. If that fails the next inbox fetch is forced so
  the row shows what the relay has.
- The x button and Esc close the pane. Widgets in it are replaced with
  `deleteLater`, since Delete ping and the Open button run from inside the
  pane they rebuild.

## Protocols and interfaces

`POST /api/notifications/desktop`, JSON body:

| Field | Rule |
| --- | --- |
| `pingId` | `[A-Za-z0-9_.:-]{1,64}`; the desktop sends `<timestampMs>-<row id>` |
| `node` | `[A-Za-z0-9_.-]{1,64}`, the desktop's node name, lowercased by the relay |
| `kind` | the desktop's own kind, `[A-Za-z0-9_-]{0,40}` |
| `category` | `""`, `agent-finished` or `agent-failed` |
| `sessionId` | digits, at most 12; set for pings about a desktop agent |
| `repo` | `""` or `owner/name` |
| `warning` | a JSON boolean |
| `title`, `body` | at most 160 and 500 characters; not both empty; refused rather than trimmed, because the proof covers them |

Authorised by the account session, or by `user`/`ts`/`sig` over

```
forkmesh-account-desktop-ping-v1
<user>
<pingId>\n<node>\n<kind>\n<category>\n<sessionId>\n<repo>\n<0|1>\n<title>\n<body>
<ts>
```

signed by the account key (`signOrgTaskProof`), so a captured proof announces
exactly one ping, and only into the signer's own inbox.

`DELETE /api/notifications/desktop` removes forwarded pings. Its body is
`{"node": "<node>", "pingIds": ["<pingId>", ...]}`: the node as on the POST,
and 1 to 50 ping ids with the POST's `pingId` rule. It is authorised by the
account session, or by `user`/`ts`/`sig` over

```
forkmesh-account-desktop-ping-delete-v1
<user>
<node>
<pingId>        (one line per id, in body order)
<ts>
```

A captured proof can delete only the rows it names, from the signer's inbox. The row's `meta` holds
`node`, `desktopKind`, `warning`, and `category` and `sessionId` when set; its
`href` is `/<repo>/agents/<sessionId>` when both are known.

Categories: an `agent-done` ping, and any ping about an agent that is not a
warning (waiting for a reply, needing approval), is `agent-finished`, whose
actions are Open, Continue and Merge & clean up; a warning about an agent is
`agent-failed` (Open, Retry, Send to agent). On iOS the category rides in
`aps.category` and the session in the payload's `sessionId`, so
`NotificationActionRouter` opens `.agent(id:)`, which resolves `desktop:<id>`
(`AgentRouteDestination.rowId`).

### Tapping the banner

A plain tap on an agent ping opens that agent's session on the phone, not the
repository's agent list and not the inbox. The `href` (`/<repo>/agents/<id>`)
cannot do it: it names the relay's copy of a repository's agent sessions, and
a desktop session is not there, and a ping about an agent with no repository
has no `href` at all.

- iOS: the default tap goes through `ApplePushBridge.receivedTap`, not
  `NotificationActionRouter`. The bridge passes on `sessionId` and the
  notification's category; `PushOpen.agent` is the session when the category
  is `agent-finished` or `agent-failed`. `HomeShellModel.routePush` marks the
  row read and returns `.agent(id)`, which the shell shows as `.agent(id:)`.
  When the push itself names no session, a row whose `InboxRoutes.route` is
  `.agent` (a `desktop_ping` with `meta.sessionId`) opens the same way.
- Android: `PingNotifier.post` builds the tap's `PushTarget` with
  `PushTarget.forPing`, whose `agent` is the `Route.Agent` id `InboxRoutes`
  gives the row, carried in the `com.forkmesh.push.agent` extra.
  `PushFeature.open` marks the row read and pushes
  `AgentsFeature.agentScreen(id)` before looking at the `href`.
  `AgentRouteScreen` refreshes `AgentControlCenter` (which connects to the
  desktop, over the LAN or the relay) and replaces itself with
  `UnifiedAgentScreen` for the row `agentRouteRowId(id)` names: `desktop:<n>`
  for a number, `node:<id>` otherwise, the same rule as iOS
  `AgentRouteDestination.rowId`. `Route.Agent` resolves through the same
  screen, so an inbox row, Merge, and the Open action land there too; before,
  a bare session id fell back to the All agents list.

## Traps

- The time-sensitive interruption level only breaks through Focus once the
  app has the `com.apple.developer.usernotifications.time-sensitive`
  entitlement. It is not in `Config/ForkMesh-*.entitlements` yet. A
  provisioning profile without the capability would fail signing, so add it
  together with the App ID capability. Until then the alarm sound plays
  whenever the phone's Focus allows sound.

- The desktop's `/api/notifications/agent-approval` POST
  (`LocalControlAgentOps`) still has no route on the served stack: it was
  written for the Next server branch and never merged. Approvals therefore
  reach the phone as the plain desktop ping "needs your approval to continue"
  (category `agent-finished`, which opens the agent), not with Approve and
  Deny buttons.
- The route refuses a title over 160 or a body over 500 characters. The
  desktop cuts to fit before signing (`fitText`), counting UTF-16 units, which
  is never fewer than Python's code-point count.

## Recent changes

- 2026-09-29: added. Desktop pings reach a linked phone as push notifications.
- 2026-09-29: tapping an agent ping opens the agent's session on iOS and
  Android. Before, iOS followed the `href` to the repository's agent list (or
  the inbox), and Android followed it to the relay's session loader, which
  never lists a desktop session.
- 2026-09-29: an agent-done push shows only `Agent #<id>` and the agent's
  description. Before, it read "Agent #<id> is done" over the repository and
  a body of issue, repository, duration, cost, model and the closing summary.
- 2026-09-29: the desktop pings the phone when it starts and first reaches
  the relay, and when it reconnects after 10 minutes or more offline.
- 2026-10-03: the desktop rings an alarm on the phone when it loses power and
  switches to battery, and pings again when power returns (Power alarm).
- 2026-10-04: the phones' Desktop screens show the desktop's battery level
  while it runs on battery (Power alarm, "The battery gauge").
- 2026-10-06: deleting pings on the desktop deletes the copies it forwarded
  from the account inbox, so the website and phones drop them too, and every
  inbox delete wakes open clients ("Deleting a ping").
- 2026-10-07: chat and DMs received on the phone show up in its Pings list,
  as they do on the desktop ("Chat on the phone's Pings").
- 2026-10-07: on the desktop's Pings page only Title, Link and Repository
  open something; the rest of the row just selects it ("Which cells of a row
  open something").
- 2026-10-07: clicking a ping on the desktop opens a detail pane on the
  right with the whole ping, an Open button and its actions; Title no longer
  jumps straight to the destination, and double-click does ("The ping detail
  pane").
