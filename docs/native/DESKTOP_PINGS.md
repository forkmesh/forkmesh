# Desktop pings on the phone

Part of the [phone apps docs](README.md). When a phone is linked to a desktop,
every ping that desktop raises also reaches the phone as a push notification:
an agent finishing, waiting or needing approval, a workflow run, a usage window
resetting, a planned restart, and the rest of the desktop's Pings page. A ping
that reports a problem leads with a one-tap fix ([Quick fixes](#quick-fixes)).
Push delivery itself is in [MESH_PUSH.md](MESH_PUSH.md) and
[PUSH_BANNERS.md](PUSH_BANNERS.md).

## Code map

| Piece | Where |
| --- | --- |
| Which pings go, wire shape, phone-link rule, dedupe and storm gate | `desktop/src/DesktopPingForward.{h,cpp}`, `desktop/tests/test_desktop_ping_forward.cpp` |
| Burst batching and summaries (OS cards, arrival sound, phone) | `desktop/src/PingBatch.{h,cpp}`, `desktop/tests/test_ping_batch.cpp`; `batchNativeNotificationCard`, `admitPingSound` (`MainWindowActions.cpp`), `flushPhonePingBatch` (`MainWindowPingForward.cpp`) |
| Sending (signing, POST, on/off switch) | `desktop/src/MainWindowPingForward.cpp`; hook in `MainWindow::recordNotification` (`MainWindowActions.cpp`) |
| Route | `app/src/desktop_pings.py` (`POST` and `DELETE /api/notifications/desktop`, `POST /api/notifications/agent-approval`); tests `app/tests/test_desktop_pings.py` |
| Agent approvals | sender `pushAgentApprovalPings` (`MainWindowLocalControlAgentOps.cpp`), proof and body in `LocalControlAgentOps.{h,cpp}`; ping kind `agent_approval`; phone actions `AgentApprovalNotification` (iOS `FMAgents/Remote`, Android `agents/ops`) |
| Ping kind | `desktop_ping` in `NOTIFICATION_KINDS`, `devices` group of `PING_KIND_CATALOG` ("Pings from your desktop") |
| Banner | `_apns_ping_alert` (`entry.py`), `apns.alert_payload` (adds `aps.category` and top-level `sessionId`) |
| Power alarm | `desktop/src/DesktopPowerAlert.{h,cpp}`, `PowerSourceReader.{h,cpp}`, `MainWindowPowerAlert.cpp`; relay `DESKTOP_PING_ALARMS`, `apns.APNS_ALARM_SOUNDS`; iOS `DesktopAlarm.swift`; Android `PingAlarm.kt`, `PingNotifier` |
| One-tap fixes | [Quick fixes](#quick-fixes); the route adds the desktop's `pingId` to `meta` |
| Chat on the phone's Pings | `ChatHub.filePing`, `LocalPingLog.recordChat` (iOS `FMApi/Inbox/InboxStores.swift`, Android `inbox/Inbox.kt`) |
| Desktop ping detail pane | `showPingDetail`, `renderPingDetail`, `refreshPingDetail`, `markWebAlertRead` (`MainWindowActions.cpp`) |
| Tapping the banner | iOS `ApplePushBridge` → `PushOpen.agent` → `HomeShellModel.routePush`; Android `PushTarget.forPing` → `PushFeature.open` → `AgentsFeature.agentScreen` |

## Architecture

A backgrounded phone app has closed its desktop-control socket, so a desktop
ping becomes an account ping (as timer expiries do, `timer_pings.py`). The
desktop POSTs it to the relay on the core nodes, `enqueue_notification` files a `desktop_ping`
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
open ([PUSH_BANNERS.md](PUSH_BANNERS.md#no-banner-for-what-is-already-on-screen)). Power alarms always go.

### Duplicates and storms

`pingforward::Gate` drops a ping that repeats one forwarded in the last 15
seconds (same agent session, or one ping's first 48 letters and digits inside
the other's), and caps forwarding at 30 pings per 10 minutes. The relay's
dedupe key is `desktop:<node>:<pingId>`, so a retried POST rewrites its row.

### Bursts become summaries

`pingbatch::Batcher` batches pings that arrive one after another on three
rails, each with its own burst count: the desktop's OS cards (`postNotification` and
`showPingTrayMessage`), the ping arrival sound, and the push to the phone.

- A burst starts with the first ping after 30 quiet seconds (`kQuietMs`). Its
  first three pings (`kSoloCards`) go out one by one.
- The rest are held. One summary goes out when the burst reaches 10 pings,
  again at 100, and after that at every further 100 (`nextThreshold`).
- Pings still held go out when the burst has been quiet for 30 seconds, or 5
  minutes after the first one was held (`kMaxHoldMs`), so a steady trickle is
  not silent until the next threshold. If only one ping is held it goes out
  as itself.
- A summary's title is `N more pings`, plus `, K with a problem` when any were
  warnings. The body has one line per kind of ping, problems first and then
  the most frequent: a single ping shows its title and detail, and repeats
  show as `8× Agent is done! — #4, #5, #6, #7, #8 +3`, so the numbers that
  differ are still visible. `#<digits>` is taken out of the title to group
  pings and shown as these numbers. More than four kinds fold into
  `+ N more of K other kinds`, and the last line gives the burst total and
  points to the Pings page, which still lists every ping. The OS card opens
  that page (`fm:pings`) and stays up 10 seconds on the tray.
- Bypass: dismiss-only (acknowledgement) cards and resident tray balloons
  (`timeoutMs` 0) on the desktop; power alarms and dismiss-only pings on the
  phone.
- The sound rail plays the cue for the solo pings and once at each threshold.
- A phone summary is sent as kind `desktop` with pingId
  `<flushMs>-batch`. No desktop row holds that id, so deleting rows on the
  desktop does not remove the summary from the phone. Held pings have no
  `phonePingId`. Every send, summaries included, still goes through
  `pingforward::Gate`; with batching a storm rarely gets near its cap.

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
- Each row in the list shows two small icons on its second line, right
  aligned: the project's logo (`currentProjectLogo`, the instance logo a
  project adopts) and where the ping lives, `device-desktop` for one raised on
  this desktop or `cloud` for one from the account's ping inbox
  (`kPingCloudRole` on the Type cell). Hovering them names both. Table mode
  puts the same local/cloud icon in the Status cell.
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

signed by the account key (`signOrgTaskProof`), so a captured proof announces
exactly one ping into the signer's own inbox. The `runId` line is present only
when the ping names a run. Deploy the relay before the desktop: an older relay
refuses `ci-failed` and does not sign over `runId`.

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

### Agent approvals

When an agent on the desktop waits for a tool approval (the same queue the
phone's Approvals screen reads over desktop control), the desktop sends one
`POST /api/notifications/agent-approval` per approval it has not announced yet
(`ApprovalPingLedger`, 256 ids, kept across restarts; a 408, 429, 5xx or
network failure is retried on the next tick). JSON body:

| Field | Rule |
| --- | --- |
| `sessionId` | `[A-Za-z0-9_.:-]{1,64}`; the desktop sends the agent row number |
| `approvalId` | 32 lowercase hex characters (`validApprovalId`) |
| `agent` | at most 80 characters on one line; empty becomes "An agent" |
| `summary` | at most 200 characters on one line, such as "Wants to run a command in alice/app"; empty becomes "Wants your approval" |

Authorised by the account session, or by `user`/`ts`/`sig` over

```
forkmesh-account-agent-approval-ping-v1
<user>
<sessionId>\n<approvalId>\n<agent>\n<summary>
<ts>
```

The relay files an `agent_approval` row ("Agent approvals from your desktop"
in the ping settings, devices group) titled "<agent> needs your approval" with
the summary as its body, deduplicated on the approval id. Its `meta` holds
`category: agent-approval`, `sessionId`, `approvalId` and `agent`. The APNs
payload carries `aps.category` `agent-approval`, the time-sensitive level, an
`agent-approval:<sessionId>` thread and top-level `sessionId` and
`approvalId`; Android reads the same fields from the ping's `meta`.

The banner offers Approve, Deny and Open. The phone answers through desktop
control (`agentops.prepare` + `agentops.answer`), never through the relay, so
the ping only says that an approval is waiting and cannot approve anything by
itself. Approve asks for the device passcode or biometrics (iOS approves once
in place; Android opens the Approvals screen), Deny answers without opening
the app, and Open lands on the approval. When the desktop does not take the
answer, iOS opens the approval and Android says so.

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

#### Loading the agent page a tap opens (iOS)

A tap often lands before the Agents list has loaded (a cold launch still has
to reach the desktop). The page (`AgentRouteLookup`,
`Features/Agents/Remote/AgentRouteScreens.swift`):

- waits for `center.loading` to clear, since `AgentControlCenter.reload`
  returns at once while another refresh is in flight;
- sees relay runs early: `loadCloud` publishes relay rows as soon as they land,
  without waiting for the desktop link or `agents.list` timeouts (20 s each);
  the desktop's answer then folds in any run it took over;
- resolves the id with `AgentControlCenter.row(forRoute:)` (over
  `UnifiedAgentRow.forRoute`), which also finds a ping from another paired
  desktop (`<key>:desktop:<n>`, when only one desktop has that number), a
  relay run by its `cloudSessionId`, and a relay run the active desktop took
  over as its desktop row;
- keeps the row it found when a later refresh fails; while a load is still
  running it follows the row if its id changes (`.id(row.id)` rebuilds the run
  screen).

While it looks, the page shows a skeleton shaped like the agent page
(`agent-route-loading`); when nothing matches it says why (the desktop's error
when there is one) and offers Try again (`agent-route-missing`). Tests:
`AgentControlTests.testAnAlertsAgentIdFindsItsRowOnAnyDesktopOrTheQueue`,
`testNodeRunsShowBeforeTheDesktopAnswers`,
`testARelayRunTheDesktopTookOpensAsItsDesktopRow`.

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
rewriting just the `owner/repo` prefix to the public identity, so a ping for
an agent, an Actions run or a branch opens that record, not the repository's
front page.

## Quick fixes

A ping that reports a problem leads with a one-tap fix. On the phone it is
the first row under **Actions** on the ping's page; on the desktop it is the
first entry in the right-click menu on the Pings page. For a run that failed
because the build Mac was not on the network, that fix is **Turn off this
action**.

The desktop decides which fixes to offer and in what order. It learns from
the fixes you pick, and once a fix has proven itself it applies that fix by
itself and tells you, with an undo.

### Code map

| Piece | Where |
| --- | --- |
| Causes, fixes, ranking, the learning ledger, trust | `desktop/src/PingFixes.{h,cpp}`, `desktop/tests/test_ping_fixes.cpp` (`forkmesh-ping-fixes-tests`) |
| Applying a fix, the `pings.` control family, the auto-apply hook, the Pings menu | `desktop/src/MainWindowPingFixes.cpp`; hooks in `recordNotification` and `onRunFinished` (`MainWindowActions.cpp`) |
| The ping id on the phone | `app/src/desktop_pings.py` puts the desktop's `pingId` in the row's `meta` |
| iOS | `FMControl/DesktopPingFixes.swift` (client), `InboxItem.desktopPingId` / `fromDesktop`, rows in `Features/Inbox/InboxItemScreen.swift` |
| Android | `inbox/DesktopPingFixes.kt` (client), `InboxItem.desktopPingId` / `fromDesktop`, rows in `feature/inbox/InboxItemScreen.kt` |

### Why the desktop owns it

The desktop raised the ping, holds the run, its log and the repository, and
is the only place a fix can run. Keeping the rules and the ledger there gives
one brain for every phone and the desktop's own Pings page, and an older
phone needs no update when the rules improve.

### The wire

Two methods in the owner-only `pings.` family on local control (LAN or
relay, [../desktop/networking.md](../desktop/networking.md#phone-control-over-the-lan)):

- `pings.fixes {pingId?, title, body, ts}` returns
  `{found, event, cause, signature, fixes: [{id, label, detail, icon, score, trusted, autoSafe, primary}]}`,
  best first.
- `pings.fix {…same, fix}` applies one and returns the refreshed list plus a
  `message` the phone shows inline under the ping's Actions. Errors: `unknown_ping`,
  `fix_unavailable`, `unknown_repo`, `unknown_run`, `unknown_agent`,
  `wrong_state`, `not_started`.

The desktop finds the ping by `pingId` (`<timestampMs>-<localId>`, as it was
forwarded), then by the same timestamp, then by the forwarded title and body
within 15 minutes. The Pings journal keeps each row's `runId`, so a run's ping
stays fixable after a restart.

### Causes and fixes

`causeOf` reads the ping's `Cause:` line (or the body) and sorts it into
host-offline, network, timeout, missing-tool, auth, disk, test, build or
unknown. The event comes from the ping: action failed, stopped, waiting for
approval, turned off automatically, or an agent that failed. Disk covers
the runner's own pre-checkout check ("Not enough free disk space on this
node") as well as ENOSPC from a step.

| Event | Fixes |
| --- | --- |
| Action failed | Turn off this action (or turn it back on), Run it again, Ask an agent to fix it |
| Action stopped | Run it again, Turn off this action |
| Waiting for approval | Approve and run, Reject this run |
| Turned off automatically | Undo: turn it back on |
| Agent failed | Tell the agent to try again |

Each fix has a starting weight per cause. An offline host puts turning the
action off first (0.85); a failing test or compile error puts the agent first
(0.8); a network error or timeout puts running it again first.

### Learning

`Ledger` keeps counts per fix under two keys: the exact signature
(event, cause, repository, workflow) and the family (event, cause), so what
you teach one workflow carries over to the same kind of failure elsewhere.
Counts: offered, chosen, auto-applied, ok, failed, undone, and the current
streak of successes. It is stored in `<dataRoot>/ping-fixes.json` and capped
at 400 signatures.

- **Offered** is counted when a fixable ping is filed.
- **Picking** a fix that finishes at once (turning off, approving) counts as
  a success.
- **Running it again** succeeds or fails with the new run.
- **An agent fix or retry** waits: if the same failure comes back within a
  day it failed; if not, it succeeded.
- **Turning an action back on** counts as an undo against turning it off.

The score blends the success rate and how often you pick the fix, each
smoothed toward the starting weight; the exact signature counts twice as much
as the family.

### Self-approval

A fix becomes trusted for one workflow when it is safe to apply unattended
(today: turning an action off), and you have picked it with success the last
three times that workflow failed the same way (`kTrustStreak`). The phone
then shows it with an **Auto** badge. On the next such failure the desktop
turns the action off itself and files "Action turned off automatically",
whose fix is **Undo: turn it back on**. An undo resets the streak, so that
fix needs three more picks before it acts alone again. Trust never carries
over to another workflow, and running again or starting an agent is never
applied unattended.

## Traps

- The time-sensitive level only breaks through Focus with the
  `com.apple.developer.usernotifications.time-sensitive` entitlement, which
  `Config/ForkMesh-*.entitlements` does not carry yet. Add it together with the
  App ID capability, or signing fails.
- The desktop cuts title and body to fit before signing (`fitText`), counting
  UTF-16 units, never fewer than Python's code points.
